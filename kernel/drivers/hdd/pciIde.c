/* SPDX-License-Identifier: BSD-3-Clause */
// PCI IDE bus-master controller: PCI configuration space access through the
// 0xCF8/0xCFC address/data pair, the scan for the IDE controller (class
// 0x01, subclass 0x01) with its bus-master BAR and the per-channel
// bus-master register block (command, status, descriptor-table pointer).
#include <kernel/drivers/hdd/pciIde.h>
#include <kernel/io/io.h>

// Devices per bus and functions per device on the classic PCI bus.
#define PciDeviceCount   32u
#define PciFunctionCount 8u

// Mask the device/function fields the address port expects.
#define PciDeviceShift   11u
#define PciDeviceMask    0x1Fu
#define PciFunctionShift 8u
#define PciFunctionMask  0x07u
#define PciBusShift      16u

// Register-offset selection: dwords live on 4-byte strides, words on
// 2-byte strides and bytes on any offset.
#define PciRegisterMask  0xFCu
#define PciWordBit       0x02u
#define PciWordShift     16u
#define PciByteStride    8u

// Build the configuration address of one dword for the 0xCF8 port: enable
// bit, bus, device, function and the dword-aligned register offset.
uint32_t PciConfigAddress(uint8_t Bus, uint8_t Device, uint8_t Function,
                          uint8_t Offset)
{
    return PciConfigEnable
         | ((uint32_t)Bus << PciBusShift)
         | ((uint32_t)(Device & PciDeviceMask) << PciDeviceShift)
         | ((uint32_t)(Function & PciFunctionMask) << PciFunctionShift)
         | ((uint32_t)Offset & PciRegisterMask);
}

// The two raw accesses every other configuration read is built from.
uint32_t PciConfigReadDword(uint8_t Bus, uint8_t Device, uint8_t Function,
                            uint8_t Offset)
{
    OutDword(PciConfigAddressPort,
             PciConfigAddress(Bus, Device, Function, Offset));
    return InDword(PciConfigDataPort);
}

void PciConfigWriteDword(uint8_t Bus, uint8_t Device, uint8_t Function,
                         uint8_t Offset, uint32_t Value)
{
    OutDword(PciConfigAddressPort,
             PciConfigAddress(Bus, Device, Function, Offset));
    OutDword(PciConfigDataPort, Value);
}

// A 16-bit register is the upper or lower half of its config dword.
uint16_t PciConfigReadWord(uint8_t Bus, uint8_t Device, uint8_t Function,
                           uint8_t Offset)
{
    uint32_t Value = PciConfigReadDword(Bus, Device, Function,
                                        (uint8_t)(Offset & PciRegisterMask));

    return (uint16_t)(Value >> ((Offset & PciWordBit) ? PciWordShift : 0));
}

// A byte register is one of the four bytes of its config dword.
uint8_t PciConfigReadByte(uint8_t Bus, uint8_t Device, uint8_t Function,
                          uint8_t Offset)
{
    uint32_t Value = PciConfigReadDword(Bus, Device, Function,
                                        (uint8_t)(Offset & PciRegisterMask));

    return (uint8_t)(Value >> ((Offset & 0x03u) * PciByteStride));
}

// Walk the functions of every device on bus 0 and take the first IDE
// controller (class 0x01, subclass 0x01) whose bus-master BAR4 is an I/O
// region. Returns ErrOk or, when nothing matches, ErrIo.
int PciIdeFind(PciIdeController *Controller)
{
    uint8_t Device;
    uint8_t Function;

    if (Controller == 0)
        return ErrIo;
    for (Device = 0; Device < PciDeviceCount; Device++) {
        for (Function = 0; Function < PciFunctionCount; Function++) {
            uint16_t Vendor = PciConfigReadWord(0, Device, Function,
                                                PciRegVendorId);
            uint8_t Class;
            uint8_t SubClass;
            uint32_t Bar;

            if (Vendor == PciVendorNone)
                continue; // the function holds no device
            Class = PciConfigReadByte(0, Device, Function, PciRegClass);
            SubClass = PciConfigReadByte(0, Device, Function, PciRegSubClass);
            if (Class != PciClassStorage || SubClass != PciSubClassIde)
                continue;
            Bar = PciConfigReadDword(0, Device, Function, PciRegBar4);
            if ((Bar & PciBarIoSpace) == 0)
                continue; // the bus-master interface must be an I/O region

            Controller->Bus = 0;
            Controller->Device = Device;
            Controller->Function = Function;
            Controller->ProgIf = PciConfigReadByte(0, Device, Function,
                                                   PciRegProgIf);
            Controller->VendorId = Vendor;
            Controller->DeviceId = PciConfigReadWord(0, Device, Function,
                                                     PciRegDeviceId);
            Controller->BusMasterBase = Bar & PciBarIoMask;
            return ErrOk;
        }
    }
    return ErrIo;
}

// Set the I/O-space and bus-master bits of the command register; the
// status half of the dword is preserved so its write-one-clear bits keep
// their meaning.
int PciIdeEnableBusMaster(const PciIdeController *Controller)
{
    uint32_t Value;

    if (Controller == 0)
        return ErrIo;
    Value = PciConfigReadDword(Controller->Bus, Controller->Device,
                               Controller->Function, PciRegCommand);
    Value |= (uint32_t)(PciCmdIoSpace | PciCmdBusMaster);
    PciConfigWriteDword(Controller->Bus, Controller->Device,
                        Controller->Function, PciRegCommand, Value);
    return ErrOk;
}

// Per-channel register port from the bus-master base and the two offsets
// that select the primary or the secondary channel.
static uint16_t PciIdePort(const PciIdeController *Controller, uint8_t Channel,
                           uint16_t Primary, uint16_t Secondary)
{
    return (uint16_t)(Controller->BusMasterBase +
                      (Channel ? Secondary : Primary));
}

uint8_t PciIdeCommandRead(const PciIdeController *Controller, uint8_t Channel)
{
    if (Controller == 0)
        return 0xFFu;
    return InByte(PciIdePort(Controller, Channel, PciIdePrimaryCommand,
                             PciIdeSecondaryCommand));
}

void PciIdeCommandWrite(const PciIdeController *Controller, uint8_t Channel,
                        uint8_t Value)
{
    if (Controller == 0)
        return;
    OutByte(PciIdePort(Controller, Channel, PciIdePrimaryCommand,
                       PciIdeSecondaryCommand), Value);
}

uint8_t PciIdeStatusRead(const PciIdeController *Controller, uint8_t Channel)
{
    if (Controller == 0)
        return 0xFFu;
    return InByte(PciIdePort(Controller, Channel, PciIdePrimaryStatus,
                             PciIdeSecondaryStatus));
}

// Writing the status back is how the error and interrupt bits are cleared.
void PciIdeStatusWrite(const PciIdeController *Controller, uint8_t Channel,
                       uint8_t Value)
{
    if (Controller == 0)
        return;
    OutByte(PciIdePort(Controller, Channel, PciIdePrimaryStatus,
                       PciIdeSecondaryStatus), Value);
}

uint32_t PciIdePrdRead(const PciIdeController *Controller, uint8_t Channel)
{
    if (Controller == 0)
        return 0;
    return InDword(PciIdePort(Controller, Channel, PciIdePrimaryPrd,
                              PciIdeSecondaryPrd));
}

void PciIdePrdWrite(const PciIdeController *Controller, uint8_t Channel,
                    uint32_t Value)
{
    if (Controller == 0)
        return;
    OutDword(PciIdePort(Controller, Channel, PciIdePrimaryPrd,
                        PciIdeSecondaryPrd), Value);
}
