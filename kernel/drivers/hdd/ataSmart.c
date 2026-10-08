/* SPDX-License-Identifier: BSD-3-Clause */
// ATA SMART: enable/disable, status, read data/thresholds, and attribute
// helpers. All commands use the SMART feature register (0x4F/0xC2) and
// return codes from errors.h.
#include <errors.h>
#include <kernel/drivers/hdd/ata.h>
#include <kernel/io/io.h>

// Timeout for SMART commands.
#define AtaSmartTimeoutMs 1000u

// SMART feature codes.
#define AtaSmartFeatureEnable   0xD8u
#define AtaSmartFeatureDisable  0xD9u
#define AtaSmartFeatureStatus   0xDAu
#define AtaSmartFeatureReadData 0xD0u
#define AtaSmartFeatureReadThresh 0xD1u

// SMART LBA mid/high values used with the feature codes above.
#define AtaSmartLbaMid  0x4Fu
#define AtaSmartLbaHigh 0xC2u

// SMART attribute table: 30 attributes, each 12 bytes.
#define AtaSmartAttrCount 30u
#define AtaSmartAttrBytes 12u

// Attribute indices (0-based) used by the helpers.
#define AtaSmartAttrTemperature 193u // 194 - 1
#define AtaSmartAttrPowerOnHours 8u  // 9 - 1
#define AtaSmartAttrReallocated  4u  // 5 - 1
#define AtaSmartAttrPending      196u // 197 - 1

// SMART status values returned by AtaSmartStatus.
#define AtaSmartStatusOk        0x4Fu
#define AtaSmartStatusThreshold 0xF4u

// One SMART attribute as parsed from the data/thresholds table.
typedef struct
{
    uint8_t  Id;        // attribute id (1..255)
    uint16_t Flags;     // attribute flags (word 2)
    uint8_t  Value;     // current normalized value
    uint8_t  Worst;     // worst normalized value seen
    uint64_t Raw;       // raw value (6 bytes, little-endian)
} AtaSmartAttribute;

// Turn a failed wait into an error code. Floating bus or ERR/DF is
// ErrIo and consumes the error register; a wait that ran out or a
// completed command without ERR/DF keeps FailCode instead.
static int AtaSmartStatusCheck(const AtaChannel *Channel, int FailCode)
{
    uint8_t Status = AtaReadStatus(Channel);

    if (Status == 0xFF)
        return ErrIo;
    if (Status & (AtaStatusErr | AtaStatusDfy)) {
        (void)InByte((uint16_t)(Channel->CmdBase + AtaRegError));
        return ErrIo;
    }
    return FailCode;
}

// Issue one SMART command to the selected drive and wait for it to
// complete. Returns ErrOk on success, ErrIo on ERR/DF/floating bus and
// ErrTimeout if the drive never clears BSY.
static int AtaSmartCommand(const AtaDrive *Drive, uint8_t Feature,
                           uint8_t Cmd)
{
    const AtaChannel *Channel;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if ((Drive->Flags & AtaDrivePresent) == 0)
        return ErrUnsupported;
    if ((Drive->Flags & AtaDriveSmart) == 0)
        return ErrUnsupported;

    AtaSelectDrive(Channel, Drive->Slave);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegDevice),
            (uint8_t)(0xE0u | AtaDeviceLba));
    AtaDelay400ns(Channel);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegFeatures), Feature);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegSectorCount), AtaSmartLbaMid);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegLba1), AtaSmartLbaHigh);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegCommand), Cmd);
    AtaDelay400ns(Channel);
    if (AtaWaitNotBusy(Channel, AtaSmartTimeoutMs) != 0)
        return AtaSmartStatusCheck(Channel, ErrTimeout);
    return AtaSmartStatusCheck(Channel, ErrOk);
}

// Enable SMART: SET FEATURES 0xD8.
int AtaSmartEnable(const AtaDrive *Drive)
{
    return AtaSmartCommand(Drive, AtaSmartFeatureEnable,
                           (uint8_t)AtaCmdSetFeatures);
}

// Disable SMART: SET FEATURES 0xD9.
int AtaSmartDisable(const AtaDrive *Drive)
{
    return AtaSmartCommand(Drive, AtaSmartFeatureDisable,
                           (uint8_t)AtaCmdSetFeatures);
}

// Read SMART status: SET FEATURES 0xDA. Returns AtaSmartStatusOk if SMART
// is enabled and the last self-test completed without error, or
// AtaSmartStatusThreshold if a threshold has been exceeded.
int AtaSmartStatus(const AtaDrive *Drive)
{
    const AtaChannel *Channel;
    uint8_t Status;
    int Rc;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if ((Drive->Flags & AtaDrivePresent) == 0)
        return ErrUnsupported;
    if ((Drive->Flags & AtaDriveSmart) == 0)
        return ErrUnsupported;

    Rc = AtaSmartCommand(Drive, AtaSmartFeatureStatus,
                         (uint8_t)AtaCmdSetFeatures);
    if (Rc != ErrOk)
        return Rc;

    Status = AtaReadStatus(Channel);
    if (Status == 0xFF)
        return ErrIo;
    if (Status & (AtaStatusErr | AtaStatusDfy)) {
        (void)InByte((uint16_t)(Channel->CmdBase + AtaRegError));
        return ErrIo;
    }
    if (Status == AtaSmartStatusOk)
        return AtaSmartStatusOk;
    if (Status == AtaSmartStatusThreshold)
        return AtaSmartStatusThreshold;
    return ErrIo;
}

// Read SMART data or thresholds: READ SMART DATA (0xD0) or READ SMART
// THRESHOLDS (0xD1). Fills Buffer with 512 bytes (256 words).
static int AtaSmartRead(const AtaDrive *Drive, uint8_t Cmd, void *Buffer)
{
    const AtaChannel *Channel;
    uint16_t *Words;
    uint32_t WordsCount;
    uint32_t I;
    int Rc;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if ((Drive->Flags & AtaDrivePresent) == 0)
        return ErrUnsupported;
    if ((Drive->Flags & AtaDriveSmart) == 0)
        return ErrUnsupported;
    if (Buffer == 0)
        return ErrIo;

    Rc = AtaSmartCommand(Drive, 0x00u, Cmd);
    if (Rc != ErrOk)
        return Rc;

    if (AtaWaitDrq(Channel, AtaSmartTimeoutMs) != 0)
        return AtaSmartStatusCheck(Channel, ErrTimeout);

    Words = (uint16_t *)Buffer;
    WordsCount = 256u;
    for (I = 0; I < WordsCount; I++)
        Words[I] = InWord(Channel->CmdBase);
    AtaDelay400ns(Channel);
    if (AtaWaitNotBusy(Channel, AtaSmartTimeoutMs) != 0)
        return AtaSmartStatusCheck(Channel, ErrTimeout);
    return AtaSmartStatusCheck(Channel, ErrOk);
}

// Read the SMART data area (512 bytes).
int AtaSmartReadData(const AtaDrive *Drive, void *Buffer)
{
    return AtaSmartRead(Drive, (uint8_t)AtaCmdReadSmartData, Buffer);
}

// Read the SMART thresholds area (512 bytes).
int AtaSmartReadThresholds(const AtaDrive *Drive, void *Buffer)
{
    return AtaSmartRead(Drive, (uint8_t)AtaCmdReadSmartThresholds, Buffer);
}

// Parse one attribute from the SMART data or thresholds table. The table
// starts at word 10 and holds 30 attributes of 12 bytes each: id (word 0),
// flags (word 1), value (word 2), worst (word 3), raw (words 4..6,
// little-endian).
static int AtaSmartParseAttribute(const uint16_t *Table, uint8_t Index,
                                  AtaSmartAttribute *Attr)
{
    const uint16_t *Entry;

    if (Table == 0 || Attr == 0)
        return ErrIo;
    if (Index >= AtaSmartAttrCount)
        return ErrIo;

    Entry = Table + (Index * AtaSmartAttrBytes / 2);
    Attr->Id = (uint8_t)Entry[0];
    Attr->Flags = Entry[1];
    Attr->Value = (uint8_t)Entry[2];
    Attr->Worst = (uint8_t)Entry[3];
    Attr->Raw = (uint64_t)Entry[4] |
                ((uint64_t)Entry[5] << 16) |
                ((uint64_t)Entry[6] << 32);
    return ErrOk;
}

// Find an attribute by id in the SMART data table and fill Attr.
static int AtaSmartFindAttribute(const uint16_t *Table, uint8_t Id,
                                 AtaSmartAttribute *Attr)
{
    uint8_t I;

    if (Table == 0 || Attr == 0)
        return ErrIo;
    for (I = 0; I < AtaSmartAttrCount; I++) {
        int Rc = AtaSmartParseAttribute(Table, I, Attr);

        if (Rc != ErrOk)
            return Rc;
        if (Attr->Id == Id)
            return ErrOk;
    }
    return ErrIo; // attribute id not found
}

// Read the SMART data table, parse the requested attribute and return its
// raw value.
static int AtaSmartReadAttribute(const AtaDrive *Drive, uint8_t Id,
                                 uint64_t *Raw)
{
    const AtaChannel *Channel;
    uint16_t Table[256];
    AtaSmartAttribute Attr;
    int Rc;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if ((Drive->Flags & AtaDrivePresent) == 0)
        return ErrUnsupported;
    if ((Drive->Flags & AtaDriveSmart) == 0)
        return ErrUnsupported;
    if (Raw == 0)
        return ErrIo;

    Rc = AtaSmartReadData(Drive, Table);
    if (Rc != ErrOk)
        return Rc;
    Rc = AtaSmartFindAttribute(Table, Id, &Attr);
    if (Rc != ErrOk)
        return Rc;
    *Raw = Attr.Raw;
    return ErrOk;
}

// Read the SMART thresholds table, parse the requested attribute and return
// its normalized value.
static int AtaSmartReadThresholdAttribute(const AtaDrive *Drive, uint8_t Id,
                                          uint8_t *Value)
{
    const AtaChannel *Channel;
    uint16_t Table[256];
    AtaSmartAttribute Attr;
    int Rc;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if ((Drive->Flags & AtaDrivePresent) == 0)
        return ErrUnsupported;
    if ((Drive->Flags & AtaDriveSmart) == 0)
        return ErrUnsupported;
    if (Value == 0)
        return ErrIo;

    Rc = AtaSmartReadThresholds(Drive, Table);
    if (Rc != ErrOk)
        return Rc;
    Rc = AtaSmartFindAttribute(Table, Id, &Attr);
    if (Rc != ErrOk)
        return Rc;
    *Value = Attr.Value;
    return ErrOk;
}

// Temperature (attribute 194): raw value is the current temperature in
// Celsius.
int AtaSmartTemperature(const AtaDrive *Drive, uint8_t *Celsius)
{
    uint64_t Raw;
    int Rc;

    if (Celsius == 0)
        return ErrIo;
    Rc = AtaSmartReadAttribute(Drive, 194u, &Raw);
    if (Rc != ErrOk)
        return Rc;
    *Celsius = (uint8_t)Raw;
    return ErrOk;
}

// Power-on hours (attribute 9): raw value is the number of hours the drive
// has been powered on.
int AtaSmartPowerOnHours(const AtaDrive *Drive, uint32_t *Hours)
{
    uint64_t Raw;
    int Rc;

    if (Hours == 0)
        return ErrIo;
    Rc = AtaSmartReadAttribute(Drive, 9u, &Raw);
    if (Rc != ErrOk)
        return Rc;
    *Hours = (uint32_t)Raw;
    return ErrOk;
}

// Reallocated sector count (attribute 5): raw value is the number of
// sectors reallocated since manufacture.
int AtaSmartReallocated(const AtaDrive *Drive, uint32_t *Count)
{
    uint64_t Raw;
    int Rc;

    if (Count == 0)
        return ErrIo;
    Rc = AtaSmartReadAttribute(Drive, 5u, &Raw);
    if (Rc != ErrOk)
        return Rc;
    *Count = (uint32_t)Raw;
    return ErrOk;
}

// Pending sector count (attribute 197): raw value is the number of sectors
// waiting to be reallocated.
int AtaSmartPending(const AtaDrive *Drive, uint32_t *Count)
{
    uint64_t Raw;
    int Rc;

    if (Count == 0)
        return ErrIo;
    Rc = AtaSmartReadAttribute(Drive, 197u, &Raw);
    if (Rc != ErrOk)
        return Rc;
    *Count = (uint32_t)Raw;
    return ErrOk;
}
