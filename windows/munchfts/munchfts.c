/*
 * munchfts - FocalTech FTS touch driver for Xiaomi munch (POCO F4) on Windows.
 *
 * The platform has no PEP, so Qualcomm's SPI stack cannot be used.  The UEFI firmware
 * (MunchTouchDxe) already enabled the QUP clocks, muxed the SPI pins, powered the touch
 * rails through RPMh and released the chip reset.  This driver therefore only maps the
 * serial engine registers (QUP0 SE4), runs the FTS register protocol over GENI full-duplex
 * SPI by polling, and reports the contacts to Windows as a HID touch screen through the
 * Virtual HID Framework (VHF).
 *
 * ACPI resources (see AcpiTables/munch/touch_fts.asl):
 *   0: SE4 GENI registers      (0x00990000, 0x4000)
 *   1: TLMM south, GPIO 39     (0x0F527000, 0x1000)  - only read for diagnostics
 */

#include <ntddk.h>
#include <stdarg.h>
#include <wdf.h>
#include <vhf.h>
#include <ntstrsafe.h>

#define POOL_TAG 'stfM'

// ---- GENI serial engine registers
#define GENI_FORCE_DEFAULT    0x020
#define GENI_OUTPUT_CTRL      0x024
#define GENI_CGC_CTRL         0x028
#define GENI_SER_M_CLK_CFG    0x048
#define GENI_IF_DISABLE_RO    0x064
#define GENI_FW_REVISION_RO   0x068
#define GENI_CLK_SEL          0x07C
#define GENI_BYTE_GRAN        0x254
#define GENI_DMA_MODE_EN      0x258
#define GENI_TX_PACKING_CFG0  0x260
#define GENI_TX_PACKING_CFG1  0x264
#define GENI_RX_PACKING_CFG0  0x284
#define GENI_RX_PACKING_CFG1  0x288
#define SPI_CPHA              0x224
#define SPI_LOOPBACK          0x22C
#define SPI_CPOL              0x230
#define SPI_DEMUX_OUTPUT_INV  0x24C
#define SPI_DEMUX_SEL         0x250
#define SPI_TRANS_CFG         0x25C
#define SPI_WORD_LEN          0x268
#define SPI_TX_TRANS_LEN      0x26C
#define SPI_RX_TRANS_LEN      0x270
#define GENI_M_CMD0           0x600
#define GENI_M_CMD_CTRL       0x604
#define GENI_M_IRQ_STATUS     0x610
#define GENI_M_IRQ_EN         0x614
#define GENI_M_IRQ_CLEAR      0x618
#define GENI_S_IRQ_CLEAR      0x648
#define GENI_TX_FIFO          0x700
#define GENI_RX_FIFO          0x780
#define GENI_TX_FIFO_STATUS   0x800
#define GENI_RX_FIFO_STATUS   0x804
#define GENI_RX_WATERMARK     0x810
#define GENI_RX_RFR_WATERMARK 0x814
#define DMA_TX_IRQ_CLR        0xC44
#define DMA_RX_IRQ_CLR        0xD44
#define SE_GSI_EVENT_EN       0xE18
#define GENI_SE_IRQ_EN        0xE1C
#define GENI_HW_PARAM_0       0xE24
#define DMA_GENERAL_CFG       0xE30

#define M_CMD_DONE            0x1
#define SPI_FULL_DUPLEX       3          // TX_ONLY | RX_ONLY
#define SPI_PROTOCOL          1
#define SPI_DIV               4          // 19.2 MHz / 4

// ---- FTS protocol
#define FTS_READ_CMD          0xA0       // read + CRC
#define FTS_REG_TOUCH         0x01
#define FTS_REG_CHIP_ID       0xA3
#define FTS_DATA_LEN          63         // 10 points * 6 + 3
#define FTS_XFER_LEN          (4 + 3 + FTS_DATA_LEN + 2)   // 72 bytes
#define FTS_MAX_POINTS        10
#define FTS_MAX_ID            0x0A

#define POLL_MS               8

typedef struct _DEVICE_CONTEXT {
    PUCHAR      Se;
    PUCHAR      Gpio;
    ULONG       TxDepth;
    VHFHANDLE   Vhf;
    PKTHREAD    Thread;
    volatile BOOLEAN Stop;
    BOOLEAN     Ready;
    BOOLEAN     WasTouching;
    ULONG       Failures;
    ULONG       Frames;
    ULONG       Polls;
    NTSTATUS    LastStatus;
    UCHAR       LastData[16];
    ULONG       MinX, MaxX, MinY, MaxY;
    BOOLEAN     ExtDirty;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT EvtD0Exit;
EVT_VHF_ASYNC_OPERATION EvtVhfGetFeature;

// ---------------------------------------------------------------- HID report descriptor
// 10-finger touch screen: per finger [tip|inrange|pad][contact id][x lo hi][y lo hi],
// then contact count, plus the contact-count-maximum feature report (id 2).
#define FINGER_DESC \
    0x09, 0x22,             /* Usage (Finger) */ \
    0xA1, 0x02,             /* Collection (Logical) */ \
    0x09, 0x42,             /*   Tip Switch */ \
    0x15, 0x00, 0x25, 0x01, \
    0x75, 0x01, 0x95, 0x01, \
    0x81, 0x02, \
    0x09, 0x32,             /*   In Range */ \
    0x81, 0x02, \
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03,     /* padding */ \
    0x09, 0x51,             /*   Contact Identifier */ \
    0x75, 0x08, 0x95, 0x01, \
    0x15, 0x00, 0x26, 0xFF, 0x00, \
    0x81, 0x02, \
    0x05, 0x01,             /*   Generic Desktop */ \
    0x09, 0x30,             /*   X */ \
    0x75, 0x10, 0x95, 0x01, \
    0x15, 0x00, 0x26, 0x30, 0x2A,           /* 0..10800 */ \
    0x35, 0x00, 0x46, 0xB7, 0x02,           /* physical 0..695 (0.1 mm) */ \
    0x65, 0x11, 0x55, 0xFE, \
    0x81, 0x02, \
    0x09, 0x31,             /*   Y */ \
    0x26, 0xC0, 0x5D,                       /* 0..24000 */ \
    0x46, 0x0A, 0x06,                       /* physical 0..1546 */ \
    0x81, 0x02, \
    0x05, 0x0D,             /*   back to Digitizer */ \
    0xC0

static const UCHAR gReportDescriptor[] = {
    0x05, 0x0D,             // Usage Page (Digitizer)
    0x09, 0x04,             // Usage (Touch Screen)
    0xA1, 0x01,             // Collection (Application)
    0x85, 0x01,             //   Report ID 1
    FINGER_DESC, FINGER_DESC, FINGER_DESC, FINGER_DESC, FINGER_DESC,
    FINGER_DESC, FINGER_DESC, FINGER_DESC, FINGER_DESC, FINGER_DESC,
    0x55, 0x00, 0x65, 0x00, // clear unit
    0x09, 0x54,             //   Contact Count
    0x15, 0x00, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x01,
    0x81, 0x02,
    0x85, 0x02,             //   Report ID 2 (feature)
    0x09, 0x55,             //   Contact Count Maximum
    0x25, 0x0A,
    0xB1, 0x02,
    0xC0
};

#pragma pack(push, 1)
typedef struct _HID_FINGER {
    UCHAR   Flags;          // bit0 tip, bit1 in range
    UCHAR   Id;
    USHORT  X;
    USHORT  Y;
} HID_FINGER;

typedef struct _HID_TOUCH_REPORT {
    UCHAR       ReportId;
    HID_FINGER  Finger[FTS_MAX_POINTS];
    UCHAR       Count;
} HID_TOUCH_REPORT;
#pragma pack(pop)

// ---------------------------------------------------------------- register helpers
static __forceinline ULONG SeRead(PDEVICE_CONTEXT C, ULONG Off)
{
    return READ_REGISTER_ULONG((PULONG)(C->Se + Off));
}

static __forceinline VOID SeWrite(PDEVICE_CONTEXT C, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((PULONG)(C->Se + Off), Val);
}

static VOID GeniPacking8(PDEVICE_CONTEXT C)
{
    ULONG Cfg[4];
    ULONG Idx = 7;
    ULONG I;

    for (I = 0; I < 4; I++) {
        Cfg[I] = (Idx << 5) | (1UL << 4) | (7UL << 1);
        Idx = ((I + 1) * 8) + 7;
    }
    Cfg[3] |= 1;
    SeWrite(C, GENI_TX_PACKING_CFG0, Cfg[0] | (Cfg[1] << 10));
    SeWrite(C, GENI_TX_PACKING_CFG1, Cfg[2] | (Cfg[3] << 10));
    SeWrite(C, GENI_RX_PACKING_CFG0, Cfg[0] | (Cfg[1] << 10));
    SeWrite(C, GENI_RX_PACKING_CFG1, Cfg[2] | (Cfg[3] << 10));
    SeWrite(C, GENI_BYTE_GRAN, 0);
}

// Same sequence as the firmware (and Linux geni_se_init + spi setup): FIFO mode, SPI mode 0.
static VOID GeniInit(PDEVICE_CONTEXT C)
{
    ULONG Depth;

    SeWrite(C, SE_GSI_EVENT_EN, 0);
    SeWrite(C, GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    SeWrite(C, GENI_S_IRQ_CLEAR, 0xFFFFFFFF);
    SeWrite(C, DMA_TX_IRQ_CLR, 0xFFFFFFFF);
    SeWrite(C, DMA_RX_IRQ_CLR, 0xFFFFFFFF);

    SeWrite(C, GENI_CGC_CTRL, SeRead(C, GENI_CGC_CTRL) | 0x7F);
    SeWrite(C, DMA_GENERAL_CFG, SeRead(C, DMA_GENERAL_CFG) | 0xF);
    SeWrite(C, GENI_OUTPUT_CTRL, 0x7F);
    SeWrite(C, GENI_FORCE_DEFAULT, 1);
    SeWrite(C, GENI_SE_IRQ_EN, SeRead(C, GENI_SE_IRQ_EN) | 0xF);

    Depth = (SeRead(C, GENI_HW_PARAM_0) >> 16) & 0x3F;
    C->TxDepth = (Depth != 0) ? Depth : 16;
    SeWrite(C, GENI_RX_WATERMARK, (Depth > 3) ? Depth - 3 : 1);
    SeWrite(C, GENI_RX_RFR_WATERMARK, (Depth > 2) ? Depth - 2 : 1);
    SeWrite(C, GENI_M_IRQ_EN, SeRead(C, GENI_M_IRQ_EN) | 0x7E | M_CMD_DONE);

    SeWrite(C, GENI_DMA_MODE_EN, SeRead(C, GENI_DMA_MODE_EN) & ~1UL);
    SeWrite(C, SPI_LOOPBACK, 0);
    SeWrite(C, SPI_DEMUX_SEL, 0);
    SeWrite(C, SPI_CPHA, 0);
    SeWrite(C, SPI_CPOL, 0);
    SeWrite(C, SPI_DEMUX_OUTPUT_INV, 0);
    SeWrite(C, SPI_TRANS_CFG, SeRead(C, SPI_TRANS_CFG) & ~2UL);
    GeniPacking8(C);
    SeWrite(C, SPI_WORD_LEN, 4);
    SeWrite(C, GENI_CLK_SEL, 0);
    SeWrite(C, GENI_SER_M_CLK_CFG, (SPI_DIV << 4) | 1);
}

// Full-duplex transfer of Len bytes (multiple of 4), streaming the FIFOs.
static NTSTATUS GeniXfer(PDEVICE_CONTEXT C, const UCHAR *Tx, UCHAR *Rx, ULONG Len)
{
    ULONG Words = Len / 4;
    ULONG Txw = 0, Rxw = 0;
    ULONG Spin;

    SeWrite(C, GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    SeWrite(C, SPI_TX_TRANS_LEN, Len);
    SeWrite(C, SPI_RX_TRANS_LEN, Len);
    SeWrite(C, GENI_M_CMD0, SPI_FULL_DUPLEX << 27);

    for (Spin = 0; Spin < 20000; Spin++) {            // ~20 ms worst case
        while (Txw < Words && (SeRead(C, GENI_TX_FIFO_STATUS) & 0x0FFFFFFF) < C->TxDepth) {
            ULONG W;
            RtlCopyMemory(&W, Tx + Txw * 4, 4);
            SeWrite(C, GENI_TX_FIFO, W);
            Txw++;
        }
        while (Rxw < Words && (SeRead(C, GENI_RX_FIFO_STATUS) & 0x01FFFFFF) != 0) {
            ULONG W = SeRead(C, GENI_RX_FIFO);
            RtlCopyMemory(Rx + Rxw * 4, &W, 4);
            Rxw++;
        }
        if (Rxw == Words && (SeRead(C, GENI_M_IRQ_STATUS) & M_CMD_DONE) != 0) {
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(1);
    }

    SeWrite(C, GENI_M_CMD_CTRL, 4);                   // cancel
    KeStallExecutionProcessor(2000);
    SeWrite(C, GENI_M_CMD_CTRL, 2);                   // abort
    KeStallExecutionProcessor(2000);
    return STATUS_IO_TIMEOUT;
}

static USHORT CrcKermit(const UCHAR *D, ULONG Len)
{
    USHORT Crc = 0xFFFF;
    ULONG I, J;

    for (I = 0; I < Len; I++) {
        Crc ^= D[I];
        for (J = 0; J < 8; J++) {
            Crc = (Crc & 1) ? (USHORT)((Crc >> 1) ^ 0x8408) : (USHORT)(Crc >> 1);
        }
    }
    return Crc;
}

// Read DataLen (<= FTS_DATA_LEN) bytes from an FTS register; always clocks FTS_XFER_LEN bytes.
static NTSTATUS FtsRead(PDEVICE_CONTEXT C, UCHAR Reg, UCHAR DataLen, UCHAR *Out)
{
    UCHAR Tx[FTS_XFER_LEN];
    UCHAR Rx[FTS_XFER_LEN];
    ULONG Total = 4 + 3 + DataLen + 2;
    NTSTATUS Status;
    USHORT CrcCalc, CrcRead;

    Total = (Total + 3) & ~3UL;
    RtlZeroMemory(Tx, sizeof(Tx));
    RtlZeroMemory(Rx, sizeof(Rx));
    Tx[0] = Reg;
    Tx[1] = FTS_READ_CMD;
    Tx[2] = 0;
    Tx[3] = DataLen;

    Status = GeniXfer(C, Tx, Rx, Total);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if ((Rx[3] & 0xA0) != 0) {
        return STATUS_DEVICE_NOT_READY;
    }
    CrcCalc = CrcKermit(&Rx[7], DataLen);
    CrcRead = (USHORT)((Rx[7 + DataLen + 1] << 8) | Rx[7 + DataLen]);
    if (CrcCalc != CrcRead) {
        return STATUS_CRC_ERROR;
    }
    RtlCopyMemory(Out, &Rx[7], DataLen);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------- file log (C:\Windows\Temp\munchfts.log)
static VOID LogF(const char *Fmt, ...)
{
    char Buf[256];
    size_t Len = 0;
    va_list Args;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\SystemRoot\\Temp\\munchfts.log");
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Io;
    HANDLE H;
    LARGE_INTEGER Off;
    LARGE_INTEGER Now;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    KeQueryTickCount(&Now);
    RtlStringCbPrintfA(Buf, sizeof(Buf), "[%lu] ", (ULONG)(Now.QuadPart * KeQueryTimeIncrement() / 10000));
    RtlStringCbLengthA(Buf, sizeof(Buf), &Len);
    va_start(Args, Fmt);
    RtlStringCbVPrintfA(Buf + Len, sizeof(Buf) - Len, Fmt, Args);
    va_end(Args);
    RtlStringCbCatA(Buf, sizeof(Buf), "\r\n");
    RtlStringCbLengthA(Buf, sizeof(Buf), &Len);

    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&H, FILE_APPEND_DATA | SYNCHRONIZE, &Oa, &Io, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return;
    }
    Off.QuadPart = -1;                                // FILE_WRITE_TO_END_OF_FILE
    Off.LowPart = FILE_WRITE_TO_END_OF_FILE;
    ZwWriteFile(H, NULL, NULL, NULL, &Io, Buf, (ULONG)Len, &Off, NULL);
    ZwClose(H);
}

// ---------------------------------------------------------------- touch decode
static VOID PollOnce(PDEVICE_CONTEXT C)
{
    UCHAR Data[FTS_DATA_LEN];
    UCHAR B[FTS_DATA_LEN + 1];                 // B[0] = register, so indices match the vendor driver
    HID_TOUCH_REPORT Rep;
    HID_XFER_PACKET Pkt;
    NTSTATUS Status;
    ULONG I, Count = 0, PointNum;
    BOOLEAN Touching;

    if (!C->Ready) {
        return;
    }

    Status = FtsRead(C, FTS_REG_TOUCH, FTS_DATA_LEN, Data);
    C->LastStatus = Status;
    if (!NT_SUCCESS(Status)) {
        C->Failures++;
        return;
    }
    RtlCopyMemory(C->LastData, Data, sizeof(C->LastData));

    B[0] = FTS_REG_TOUCH;
    RtlCopyMemory(&B[1], Data, FTS_DATA_LEN);
    PointNum = B[2] & 0x0F;
    if (PointNum == 0x0F && B[3] == 0xFF && B[4] == 0xFF && B[5] == 0xFF && B[6] == 0xFF) {
        C->Failures++;                          // chip in recovery state: nothing valid
        return;
    }
    if (PointNum > FTS_MAX_POINTS) {
        return;
    }

    RtlZeroMemory(&Rep, sizeof(Rep));
    Rep.ReportId = 1;
    for (I = 0; I < FTS_MAX_POINTS; I++) {
        ULONG Base = 6 * I;
        UCHAR Id = B[5 + Base] >> 4;
        UCHAR Flag = B[3 + Base] >> 6;          // 0 down, 1 up, 2 contact
        ULONG X, Y;

        if (Id >= FTS_MAX_ID) {
            break;
        }
        X = ((B[3 + Base] & 0x0F) << 11) + ((B[4 + Base] & 0xFF) << 3) + ((B[7 + Base] & 0xE0) >> 5);
        Y = ((B[5 + Base] & 0x0F) << 11) + ((B[6 + Base] & 0xFF) << 3) + ((B[7 + Base] & 0x1C) >> 2);
        if (X < C->MinX) { C->MinX = X; C->ExtDirty = TRUE; }
        if (X > C->MaxX) { C->MaxX = X; C->ExtDirty = TRUE; }
        if (Y < C->MinY) { C->MinY = Y; C->ExtDirty = TRUE; }
        if (Y > C->MaxY) { C->MaxY = Y; C->ExtDirty = TRUE; }
        if (X > 10800) X = 10800;
        if (Y > 24000) Y = 24000;
        Rep.Finger[Count].Flags = (Flag == 1) ? 0 : 3;
        Rep.Finger[Count].Id = Id;
        Rep.Finger[Count].X = (USHORT)X;
        Rep.Finger[Count].Y = (USHORT)Y;
        Count++;
    }
    Rep.Count = (UCHAR)Count;

    Touching = (Count != 0);
    if (!Touching && !C->WasTouching) {
        return;                                 // idle, nothing to report
    }
    if (!Touching) {
        // All contacts gone without an explicit "up": report one lifted contact.
        Rep.Finger[0].Flags = 0;
        Rep.Finger[0].Id = 0;
        Rep.Count = 1;
    }
    C->WasTouching = Touching;
    C->Frames++;
    if (C->Frames <= 30) {
        LogF("frame %lu count %lu id %u x %u y %u flags %u", C->Frames, Count, Rep.Finger[0].Id, Rep.Finger[0].X, Rep.Finger[0].Y, Rep.Finger[0].Flags);
    }

    Pkt.reportBuffer = (PUCHAR)&Rep;
    Pkt.reportBufferLen = sizeof(Rep);
    Pkt.reportId = 1;
    VhfReadReportSubmit(C->Vhf, &Pkt);
}

static VOID PollThread(PVOID Ctx)
{
    PDEVICE_CONTEXT C = (PDEVICE_CONTEXT)Ctx;
    LARGE_INTEGER Delay;

    Delay.QuadPart = -10000LL * POLL_MS;
    ExSetTimerResolution(10000, TRUE);                // 1 ms timer resolution
    LogF("poll thread started");
    C->MinX = C->MinY = 0xFFFFFFFF;
    C->MaxX = C->MaxY = 0;

    while (!C->Stop) {
        PollOnce(C);
        C->Polls++;
        if (C->Polls <= 4 || (C->Polls < 8000 && (C->Polls % 250) == 0) || (C->Polls % 5000) == 0) {
            LogF("poll %lu frames %lu fail %lu last %08x gpio39 %lu data %02x %02x %02x %02x %02x %02x %02x %02x",
                 C->Polls, C->Frames, C->Failures, C->LastStatus,
                 C->Gpio ? (READ_REGISTER_ULONG((PULONG)(C->Gpio + 4)) & 1) : 9UL,
                 C->LastData[0], C->LastData[1], C->LastData[2], C->LastData[3],
                 C->LastData[4], C->LastData[5], C->LastData[6], C->LastData[7]);
        }
        if (C->ExtDirty && (C->Polls % 60) == 0) {
            C->ExtDirty = FALSE;
            LogF("extent x %lu..%lu y %lu..%lu", C->MinX, C->MaxX, C->MinY, C->MaxY);
        }
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    ExSetTimerResolution(0, FALSE);
    LogF("poll thread stopped");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ---------------------------------------------------------------- VHF feature report
VOID EvtVhfGetFeature(PVOID Ctx, VHFOPERATIONHANDLE Op, PVOID OpCtx, PHID_XFER_PACKET Pkt)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(OpCtx);

    if (Pkt->reportId == 2 && Pkt->reportBufferLen >= 2) {
        Pkt->reportBuffer[0] = 2;
        Pkt->reportBuffer[1] = FTS_MAX_POINTS;
        Pkt->reportBufferLen = 2;
        VhfAsyncOperationComplete(Op, STATUS_SUCCESS);
    } else {
        VhfAsyncOperationComplete(Op, STATUS_INVALID_PARAMETER);
    }
}

// ---------------------------------------------------------------- PnP
NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT C = GetContext(Device);
    ULONG I, N = WdfCmResourceListGetCount(Translated);
    ULONG Mem = 0;

    UNREFERENCED_PARAMETER(Raw);

    for (I = 0; I < N; I++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR D = WdfCmResourceListGetDescriptor(Translated, I);
        if (D->Type != CmResourceTypeMemory) {
            continue;
        }
        if (Mem == 0) {
            C->Se = MmMapIoSpaceEx(D->u.Memory.Start, D->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
        } else if (Mem == 1) {
            C->Gpio = MmMapIoSpaceEx(D->u.Memory.Start, D->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
        }
        LogF("resource %lu start %llx len %lx", Mem, D->u.Memory.Start.QuadPart, D->u.Memory.Length);
        Mem++;
    }
    LogF("prepare: %lu resources, %lu memory, Se %p Gpio %p", N, Mem, C->Se, C->Gpio);
    if (C->Se == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    LogF("se4: fw %08x ifdis %08x clk %08x status %08x hw0 %08x", SeRead(C, GENI_FW_REVISION_RO),
         SeRead(C, GENI_IF_DISABLE_RO), SeRead(C, GENI_SER_M_CLK_CFG), SeRead(C, 0x040), SeRead(C, GENI_HW_PARAM_0));

    // Only continue if the engine really is the SPI firmware and the FIFO interface is on.
    if (((SeRead(C, GENI_FW_REVISION_RO) >> 8) & 0xFF) != SPI_PROTOCOL ||
        (SeRead(C, GENI_IF_DISABLE_RO) & 1) != 0) {
        LogF("se4 is not an SPI engine with FIFO interface");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    GeniInit(C);
    return STATUS_SUCCESS;
}

NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT C = GetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (C->Se != NULL) {
        MmUnmapIoSpace(C->Se, 0x4000);
        C->Se = NULL;
    }
    if (C->Gpio != NULL) {
        MmUnmapIoSpace(C->Gpio, 0x1000);
        C->Gpio = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS EvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Prev)
{
    PDEVICE_CONTEXT C = GetContext(Device);
    NTSTATUS Status;
    UCHAR Id1 = 0;

    UNREFERENCED_PARAMETER(Prev);

    // Chip ID as a sanity check (expected 0x56); log only, polling starts either way.
    Status = FtsRead(C, FTS_REG_CHIP_ID, 1, &Id1);
    KdPrint(("munchfts: chip id %02x status %08x\n", Id1, Status));
    LogF("D0 entry: chip id %02x status %08x", Id1, Status);

    Status = VhfStart(C->Vhf);
    LogF("VhfStart %08x", Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    C->Ready = TRUE;
    C->Stop = FALSE;
    {
        HANDLE H;
        Status = PsCreateSystemThread(&H, THREAD_ALL_ACCESS, NULL, NULL, NULL, PollThread, C);
        LogF("PsCreateSystemThread %08x", Status);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        ObReferenceObjectByHandle(H, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&C->Thread, NULL);
        ZwClose(H);
    }
    return STATUS_SUCCESS;
}

NTSTATUS EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    PDEVICE_CONTEXT C = GetContext(Device);

    UNREFERENCED_PARAMETER(Target);
    C->Ready = FALSE;
    C->Stop = TRUE;
    if (C->Thread != NULL) {
        KeWaitForSingleObject(C->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(C->Thread);
        C->Thread = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attr;
    VHF_CONFIG Vhf;
    WDFDEVICE Device;
    PDEVICE_CONTEXT C;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    Pnp.EvtDeviceD0Entry = EvtD0Entry;
    Pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, DEVICE_CONTEXT);
    Attr.ExecutionLevel = WdfExecutionLevelPassive;     // timer runs at PASSIVE_LEVEL (file logging)
    Status = WdfDeviceCreate(&Init, &Attr, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    C = GetContext(Device);

    VHF_CONFIG_INIT(&Vhf, WdfDeviceWdmGetDeviceObject(Device), sizeof(gReportDescriptor), (PUCHAR)gReportDescriptor);
    Vhf.VendorID = 0x2808;                      // FocalTech
    Vhf.ProductID = 0x3658;
    Vhf.VersionNumber = 0x0001;
    Vhf.EvtVhfAsyncOperationGetFeature = EvtVhfGetFeature;
    Status = VhfCreate(&Vhf, &C->Vhf);
    LogF("DeviceAdd: VhfCreate %08x", Status);
    return Status;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Cfg;

    LogF("DriverEntry");
    WDF_DRIVER_CONFIG_INIT(&Cfg, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Cfg, WDF_NO_HANDLE);
}
