#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compile a single C source file to an EXF v1 image or a flat binary.

    exf.py -a cmd.c -o cmd.exf
    exf.py -a cmd.c -o cmd.bin

The output type follows the -o extension: .exf writes an EXF v1 image (via
elf2exf.py), anything else writes a flat binary holding the ELF loadable
segments packed from the lowest virtual address. No other options are needed.
"""

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import elf2exf  # noqa: E402  (same directory, reused for the EXF writer)

# Freestanding compile flags, kept in line with the kernel's Makefile but
# without the kernel code model: these are plain userland-style programs.
CFLAGS = [
    "-ffreestanding",
    "-fno-pic",
    "-fno-stack-protector",
    "-nostdlib",
    "-O2",
    "-Wall",
    "-c",
]

# Preferred entry symbol, then the fallback for files that just define main.
ENTRY_CANDIDATES = ("_start", "main")


def run(argv, description):
    """Run one build step, exiting with a readable message on failure."""
    try:
        result = subprocess.run(argv, capture_output=True, text=True)
    except OSError as error:
        sys.exit(f"exf.py: {description}: {error}")
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        sys.exit(f"exf.py: {description} failed:\n{detail}")


def defined_symbols(obj_path):
    """Return the set of global symbols defined in an object file."""
    result = subprocess.run(["nm", str(obj_path)], capture_output=True, text=True)
    if result.returncode != 0:
        return set()
    symbols = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        # "addr T name" for defined symbols; undefined ones have no address.
        if len(parts) == 3 and parts[1] in "TDBWRtv":
            symbols.add(parts[2])
    return symbols


def pick_entry(obj_path, requested):
    """Choose the ELF entry symbol: explicit --entry, else _start, else main."""
    if requested:
        return requested
    defined = defined_symbols(obj_path)
    for candidate in ENTRY_CANDIDATES:
        if candidate in defined:
            return candidate
    return None  # let ld fall back to the start of .text with its own warning


def build_elf(source, elf_path, entry, base):
    """Compile source to an ELF at base; returns the entry symbol chosen."""
    with tempfile.TemporaryDirectory(prefix="exfpy-") as tmp:
        obj_path = Path(tmp) / (source.stem + ".o")
        run(["gcc", *CFLAGS, "-o", str(obj_path), str(source)], f"compiling {source.name}")

        chosen_entry = pick_entry(obj_path, entry)
        link = ["ld", "-m", "elf_x86_64"]
        if base is not None:
            link += ["-Ttext", hex(base)]
        if chosen_entry:
            link += ["-e", chosen_entry]
        link += ["-o", str(elf_path), str(obj_path)]
        run(link, f"linking {source.name}")
        return chosen_entry


def flat_binary(elf_path):
    """Pack the ELF PT_LOAD segments into one flat binary image."""
    entry, segments, _symbols = elf2exf.parse_elf(elf_path.read_bytes())
    lowest = min(seg["virt_addr"] for seg in segments)
    highest = max(seg["virt_addr"] + seg["mem_size"] for seg in segments)
    if highest - lowest > 64 * 1024 * 1024:
        sys.exit("exf.py: flat binary would exceed 64 MiB; check --base")

    image = bytearray(highest - lowest)
    for seg in segments:
        start = seg["virt_addr"] - lowest
        image[start:start + seg["file_size"]] = seg["data"]
        # mem_size - file_size stays zero, matching ExfLoad's zero fill.
    return entry, bytes(image)


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="exf.py",
        description="Compile a C file to an EXF image or a flat binary.",
    )
    parser.add_argument("-a", "--archive", required=True, type=Path,
                        metavar="SOURCE.c", help="C source file to compile")
    parser.add_argument("-o", "--output", required=True, type=Path,
                        metavar="OUTPUT", help=".exf for an EXF image, .bin for a flat binary")
    parser.add_argument("--kind", default="program", choices=sorted(elf2exf.KIND_VALUES),
                        help="EXF image kind (default: program)")
    parser.add_argument("--entry", default=None, metavar="SYMBOL",
                        help="entry symbol (default: _start, then main)")
    parser.add_argument("--base", type=lambda value: int(value, 0), default=None,
                        metavar="ADDR", help="link address (default: ld's 0x400000)")
    args = parser.parse_args(argv)

    if not args.archive.is_file():
        parser.error(f"no such file: {args.archive}")
    if args.archive.suffix != ".c":
        parser.error(f"input must be a .c file: {args.archive}")

    with tempfile.TemporaryDirectory(prefix="exfpy-") as tmp:
        elf_path = Path(tmp) / (args.archive.stem + ".elf")
        entry = build_elf(args.archive, elf_path, args.entry, args.base)

        if args.output.suffix == ".exf":
            try:
                parsed_entry, segments, symbols = elf2exf.parse_elf(elf_path.read_bytes())
                exf_data = elf2exf.build_exf(
                    parsed_entry, segments, symbols,
                    elf2exf.KIND_VALUES[args.kind],
                )
                args.output.write_bytes(exf_data)
            except (OSError, elf2exf.ExfError, struct.error) as error:
                sys.exit(f"exf.py: {error}")
            print(f"EXF: {args.output} ({len(exf_data)} bytes, "
                  f"{len(segments)} segments, {len(symbols)} symbols, "
                  f"entry {hex(parsed_entry)})")
        else:
            parsed_entry, image = flat_binary(elf_path)
            args.output.write_bytes(image)
            print(f"BIN: {args.output} ({len(image)} bytes, "
                  f"entry {hex(parsed_entry)})")

        if entry is None:
            print("exf.py: warning: no _start or main found; "
                  "entry defaults to the first linked address", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())

