#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Convert a little-endian x86-64 ELF64 image to EXF v1."""

import argparse
import struct
import sys
import zlib
from pathlib import Path

ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
SYMBOL_ENTRY = struct.Struct("<IBBHQQ")

EXF_HEADER = struct.Struct("<IHHBBHHHQIIIIIII12s")
EXF_SEGMENT = struct.Struct("<IIQQQII")
EXF_SYMBOL = struct.Struct("<QII")

EXF_MAGIC = 0x00465845
EXF_VERSION = 1
EXF_HEADER_SIZE = 64
EXF_SEGMENT_SIZE = 40
EXF_ARCH_X86_64 = 1

ELFCLASS64 = 2
ELFDATA2LSB = 1
EV_CURRENT = 1
EM_X86_64 = 62
PT_LOAD = 1
PF_X = 1
PF_W = 2
PF_R = 4
SHT_SYMTAB = 2
SHT_STRTAB = 3
STT_OBJECT = 1
STT_FUNC = 2

KIND_VALUES = {
    "kernel": 1,
    "program": 2,
    "driver": 3,
    "library": 4,
}
U32_MAX = 0xFFFFFFFF


class ExfError(Exception):
    """Raised when an input ELF cannot be represented as EXF v1."""


def checked_slice(data, offset, size, description):
    if offset > len(data) or size > len(data) - offset:
        raise ExfError(f"{description} extends beyond the end of the ELF file")
    return data[offset : offset + size]


def parse_elf(data):
    if len(data) < ELF_HEADER.size:
        raise ExfError("input is too small to contain an ELF64 header")

    (ident, elf_type, machine, version, entry, phoff, shoff, _flags,
     ehsize, phentsize, phnum, shentsize, shnum, shstrndx) = ELF_HEADER.unpack_from(data)

    if ident[:4] != b"\x7fELF":
        raise ExfError("invalid ELF magic")
    if ident[4] != ELFCLASS64:
        raise ExfError("ELF class must be ELF64")
    if ident[5] != ELFDATA2LSB:
        raise ExfError("ELF byte order must be little-endian")
    if ident[6] != EV_CURRENT or version != EV_CURRENT:
        raise ExfError("unsupported ELF version")
    if machine != EM_X86_64:
        raise ExfError("ELF machine must be x86-64")
    if ehsize < ELF_HEADER.size:
        raise ExfError("ELF header size is smaller than the ELF64 header")
    if phnum and phentsize < PROGRAM_HEADER.size:
        raise ExfError("ELF program-header entry size is too small")

    segments = []
    for index in range(phnum):
        offset = phoff + index * phentsize
        raw = checked_slice(data, offset, PROGRAM_HEADER.size,
                            f"program header {index}")
        (ptype, pflags, file_offset, virt_addr, phys_addr,
         file_size, mem_size, _align) = PROGRAM_HEADER.unpack(raw)
        if ptype != PT_LOAD:
            continue
        if file_size > mem_size:
            raise ExfError(f"PT_LOAD segment {index} has p_filesz greater than p_memsz")
        segment_data = checked_slice(data, file_offset, file_size,
                                     f"PT_LOAD segment {index} data")
        if file_size > U32_MAX or mem_size > U32_MAX:
            raise ExfError(f"PT_LOAD segment {index} is too large for EXF v1")
        exf_flags = ((1 if pflags & PF_R else 0)
                     | (2 if pflags & PF_W else 0)
                     | (4 if pflags & PF_X else 0))
        segments.append({
            "flags": exf_flags,
            "virt_addr": virt_addr,
            "phys_addr": phys_addr,
            "file_size": file_size,
            "mem_size": mem_size,
            "data": segment_data,
        })

    if len(segments) > 16:
        raise ExfError(f"ELF has {len(segments)} PT_LOAD segments; EXF supports at most 16")
    if not segments:
        raise ExfError("ELF contains no PT_LOAD segments")

    symbols = parse_symbols(data, shoff, shentsize, shnum, shstrndx)
    return entry, segments, symbols


def parse_symbols(data, shoff, shentsize, shnum, shstrndx):
    if shnum == 0:
        return []
    if shentsize < SECTION_HEADER.size:
        raise ExfError("ELF section-header entry size is too small")

    section_headers = []
    for index in range(shnum):
        offset = shoff + index * shentsize
        raw = checked_slice(data, offset, SECTION_HEADER.size,
                            f"section header {index}")
        section_headers.append(SECTION_HEADER.unpack(raw))

    if shstrndx >= shnum:
        raise ExfError("ELF section-name string-table index is out of range")
    shstr_header = section_headers[shstrndx]
    section_names = checked_slice(data, shstr_header[4], shstr_header[5],
                                  "section-name string table")

    def section_name(name_offset):
        if name_offset >= len(section_names):
            return b""
        end = section_names.find(b"\0", name_offset)
        if end < 0:
            return b""
        return section_names[name_offset:end]

    symtab = next((header for header in section_headers
                   if header[1] == SHT_SYMTAB
                   and section_name(header[0]) == b".symtab"), None)
    if symtab is None:
        return []

    symtab_offset, symtab_size, symtab_link, symtab_entsize = (
        symtab[4], symtab[5], symtab[6], symtab[9]
    )
    if symtab_link >= shnum:
        raise ExfError(".symtab links to an invalid string-table section")
    if symtab_entsize < SYMBOL_ENTRY.size or symtab_entsize == 0:
        raise ExfError(".symtab entry size is too small")
    if symtab_size % symtab_entsize:
        raise ExfError(".symtab size is not a multiple of its entry size")

    string_header = section_headers[symtab_link]
    if string_header[1] != SHT_STRTAB:
        raise ExfError(".symtab linked section is not a string table")
    strings = checked_slice(data, string_header[4], string_header[5],
                            ".symtab string table")
    symbol_data = checked_slice(data, symtab_offset, symtab_size, ".symtab")

    symbols = []
    for index in range(symtab_size // symtab_entsize):
        offset = index * symtab_entsize
        name_offset, info, _other, _shndx, value, size = SYMBOL_ENTRY.unpack_from(
            symbol_data, offset
        )
        symbol_type = info & 0x0F
        if symbol_type not in (STT_FUNC, STT_OBJECT) or size == 0:
            continue
        if name_offset >= len(strings):
            raise ExfError(f"symbol {index} has a name offset outside its string table")
        name_end = strings.find(b"\0", name_offset)
        if name_end < 0:
            raise ExfError(f"symbol {index} name is not NUL-terminated")
        name = strings[name_offset:name_end]
        if not name:
            continue
        if size > U32_MAX:
            raise ExfError(f"symbol {name!r} is too large for EXF v1")
        symbols.append((value, size, name))

    symbols.sort(key=lambda symbol: symbol[0])
    return symbols


def build_exf(entry, segments, symbols, kind):
    segment_offset = EXF_HEADER_SIZE
    cursor = segment_offset + len(segments) * EXF_SEGMENT_SIZE
    segment_records = []
    data_chunks = []

    for segment in segments:
        aligned_offset = (cursor + 15) & ~15
        data_chunks.append((aligned_offset - cursor, segment["data"]))
        segment_records.append(EXF_SEGMENT.pack(
            1,
            segment["flags"],
            aligned_offset,
            segment["virt_addr"],
            segment["phys_addr"] if kind == KIND_VALUES["kernel"] else 0,
            segment["file_size"],
            segment["mem_size"],
        ))
        cursor = aligned_offset + segment["file_size"]

    symbol_offset = cursor
    symbol_records = []
    string_table = bytearray(b"\0")
    for value, size, name in symbols:
        name_offset = len(string_table)
        string_table.extend(name)
        string_table.append(0)
        if name_offset > U32_MAX:
            raise ExfError("EXF string table exceeds the 32-bit name-offset limit")
        symbol_records.append(EXF_SYMBOL.pack(value, size, name_offset))

    string_offset = symbol_offset + len(symbol_records) * EXF_SYMBOL.size
    file_size = string_offset + len(string_table)
    if file_size > U32_MAX:
        raise ExfError("EXF output exceeds the 32-bit file-size limit")
    if len(symbol_records) > U32_MAX or len(string_table) > U32_MAX:
        raise ExfError("EXF symbol or string table exceeds a 32-bit size field")

    header = EXF_HEADER.pack(
        EXF_MAGIC,
        EXF_VERSION,
        EXF_HEADER_SIZE,
        kind,
        EXF_ARCH_X86_64,
        0,
        len(segments),
        EXF_SEGMENT_SIZE,
        entry,
        segment_offset,
        len(symbol_records),
        symbol_offset,
        string_offset,
        len(string_table),
        file_size,
        0,
        b"\0" * 12,
    )

    output = bytearray(header)
    for record in segment_records:
        output.extend(record)
    for padding, chunk in data_chunks:
        output.extend(b"\0" * padding)
        output.extend(chunk)
    for record in symbol_records:
        output.extend(record)
    output.extend(string_table)

    if len(output) != file_size:
        raise ExfError("internal error: calculated EXF file size does not match output")

    checksum = zlib.crc32(output) & U32_MAX
    struct.pack_into("<I", output, 48, checksum)
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Convert a little-endian x86-64 ELF64 file to EXF v1."
    )
    parser.add_argument("input_elf", type=Path, help="input ELF file")
    parser.add_argument("output_exf", type=Path, help="output EXF file")
    parser.add_argument("--kind", required=True, choices=KIND_VALUES,
                        help="EXF image kind")
    args = parser.parse_args(argv)

    try:
        elf_data = args.input_elf.read_bytes()
        entry, segments, symbols = parse_elf(elf_data)
        exf_data = build_exf(entry, segments, symbols, KIND_VALUES[args.kind])
        args.output_exf.write_bytes(exf_data)
    except (OSError, ExfError, struct.error) as error:
        parser.error(str(error))

    print(f"EXF: {len(segments)} segments, {len(symbols)} symbols, "
          f"{len(exf_data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
