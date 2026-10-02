/** @file
  AmdIgpuResetDxe: reset the AMD Granite Ridge iGPU (PCI 1002:13C0) at VM boot,
  before the AMD GOP driver and the guest OS initialize it.

  Load it from a PCI option ROM image placed ahead of the AMD GOP driver image.

  Reset sequence, validated from the host with
  `igpu-mode2-reset.py --disallow-gfxoff --psp-destroy --mode2`, copied from Linux amdgpu:
    1. SMU GetSmuVersion                    mailbox sanity check
    2. SMU DisallowGfxOff                   amdgpu_device_fini_hw() ungates power gating;
                                            gfx_v10_0_set_powergating_state() sends it for GC 10.3.6
    3. PSP GFX_CTRL_CMD_ID_DESTROY_RINGS    psp_v13_0_ring_stop()
    4. SMU GfxDeviceDriverReset(MODE2)      nv_asic_mode2_reset(), smu_v13_0_5_mode2_reset()
  It only runs when the PSP mailbox still shows a ring created by a previous driver
  (fresh host boot: 0x80000000, after a reset: 0x80030000, left dirty: 0x80010000).

  Log: QEMU debugcon on I/O port 0x402, if present
  (-debugcon file:debug.log -global isa-debugcon.iobase=0x402).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/Pci.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PciIo.h>

//
// Set to 1 to reset on every boot, even when the PSP mailbox looks clean.
//
#define ALWAYS_RESET  0

#define IGPU_ID       0x13C01002u     // DeviceId << 16 | VendorId
#define REG_BAR_CFG   0x24            // config offset of the MMIO register BAR (PCI BAR5)

//
// Granite Ridge IP-discovery segment bases (dword units), from icex/macos-raphael-igpu.
//
#define NBIO_SEG0  0x0u
#define NBIO_SEG2  0xD20u
#define MP_SEG0    0x16000u
#define GC_SEG0    0x1260u
#define GC_SEG1    0xA000u
#define BAR5_OFF(Seg, Dw)  (((Seg) + (Dw)) * 4u)

#define PCIE_INDEX2  BAR5_OFF (NBIO_SEG0, 0x0E)    // regBIF_BX0_PCIE_INDEX2       nbio_7_2_0_offset.h
#define PCIE_DATA2   BAR5_OFF (NBIO_SEG0, 0x0F)    // regBIF_BX0_PCIE_DATA2
#define MEMSIZE      BAR5_OFF (NBIO_SEG2, 0xC3)    // regRCC_DEV0_EPF0_0_RCC_CONFIG_MEMSIZE
#define PSP_C2P_64   BAR5_OFF (MP_SEG0, 0x80)      // regMP0_SMN_C2PMSG_64          mp_13_0_2_offset.h
#define GRBM_STATUS  BAR5_OFF (GC_SEG0, 0xDA4)     // mmGRBM_STATUS                 gc_10_3_0_offset.h
#define CP_STAT      BAR5_OFF (GC_SEG0, 0xF40)     // mmCP_STAT
#define RLC_CNTL     BAR5_OFF (GC_SEG1, 0x4C00)    // mmRLC_CNTL

#define SMN_MP1(Dw)  (((Dw) + 0xB00000u / 4 + MP_SEG0) * 4u)  // smu_v13_0_5_ppt.c defines
#define SMU_MSG      SMN_MP1 (0xBEE142u)          // mmMP1_C2PMSG_2   (0x03B10508)
#define SMU_RESP     SMN_MP1 (0xBEE261u)          // mmMP1_C2PMSG_33  (0x03B10984)
#define SMU_ARG      SMN_MP1 (0xBEE262u)          // mmMP1_C2PMSG_34  (0x03B10988)

#define MSG_GET_SMU_VERSION          2u           // smu_v13_0_5_ppsmc.h
#define MSG_GFX_DEVICE_DRIVER_RESET  10u
#define MSG_DISALLOW_GFXOFF          23u
#define MODE2_RESET                  2u

#define PSP_DESTROY_RINGS  0x00030000u            // psp_gfx_if.h
#define PSP_CMD_MASK       0x000F0000u
#define PSP_RESP_MASK      0x8000FFFFu            // MBOX_TOS_RESP_MASK  amdgpu_psp.h
#define PSP_RESP_DONE      0x80000000u            // MBOX_TOS_RESP_FLAG

#define DEBUGCON_PORT   0x402
#define DEBUGCON_MAGIC  0xE9                      // OvmfPkg DebugIoPortQemu.c

STATIC BOOLEAN              mDebugCon;
STATIC BOOLEAN              mDone;
STATIC EFI_PCI_IO_PROTOCOL  *mPciIo;
STATIC UINT8                mRegBar;    // PciIo BarIndex of the register BAR, see FindRegisterBar()
STATIC BOOLEAN              mReadErrorLogged;
STATIC EFI_EVENT            mEvent;
STATIC VOID                 *mRegistration;

STATIC
VOID
EFIAPI
Log (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Buffer[192];
  VA_LIST  Marker;
  UINTN    Length;
  UINTN    Index;

  if (!mDebugCon) {
    return;
  }

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  for (Index = 0; Index < Length; Index++) {
    IoWrite8 (DEBUGCON_PORT, (UINT8)Buffer[Index]);
  }
}

STATIC
UINT32
Rd (
  IN UINT32  Offset
  )
{
  UINT32      Value;
  EFI_STATUS  Status;

  Status = mPciIo->Mem.Read (mPciIo, EfiPciIoWidthUint32, mRegBar, Offset, 1, &Value);
  if (EFI_ERROR (Status)) {
    if (!mReadErrorLogged) {
      mReadErrorLogged = TRUE;
      Log ("AmdIgpuReset:   read of BAR index %u offset 0x%x failed: %r\n", mRegBar, Offset, Status);
    }

    return MAX_UINT32;
  }

  return Value;
}

STATIC
VOID
Wr (
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  mPciIo->Mem.Write (mPciIo, EfiPciIoWidthUint32, mRegBar, Offset, 1, &Value);
}

STATIC
UINT32
SmnRd (
  IN UINT32  Address
  )
{
  // amdgpu_device_indirect_rreg()
  Wr (PCIE_INDEX2, Address);
  Rd (PCIE_INDEX2);
  return Rd (PCIE_DATA2);
}

STATIC
VOID
SmnWr (
  IN UINT32  Address,
  IN UINT32  Value
  )
{
  // amdgpu_device_indirect_wreg()
  Wr (PCIE_INDEX2, Address);
  Rd (PCIE_INDEX2);
  Wr (PCIE_DATA2, Value);
  Rd (PCIE_DATA2);
}

STATIC
UINT32
SmuPoll (
  IN UINTN  TimeoutMs
  )
{
  UINT32  Response;

  for ( ; ;) {
    Response = SmnRd (SMU_RESP);
    if ((Response != 0) || (TimeoutMs == 0)) {
      return Response;
    }

    TimeoutMs--;
    gBS->Stall (1000);
  }
}

/**
  smu_msg_v1_send_msg(): pre-poll, clear response, write argument, write message, post-poll.
  amdgpu does not send when the previous response is 0 (busy) or an unknown value, except
  for the first message after init, where it skips the pre-poll.
**/
STATIC
BOOLEAN
SmuSend (
  IN  UINT32   Message,
  IN  UINT32   Argument,
  IN  UINTN    TimeoutMs,
  IN  BOOLEAN  First,
  OUT UINT32   *OutArgument OPTIONAL
  )
{
  UINT32  Pre;
  UINT32  Response;

  Pre = SmuPoll (2000);
  if (!(First && (Pre == 0)) && (Pre != 1) && ((Pre < 0xFC) || (Pre > 0xFF))) {
    Log ("AmdIgpuReset:   SMU mailbox not ready (0x%x), message %u not sent\n", Pre, Message);
    return FALSE;
  }

  SmnWr (SMU_RESP, 0);
  SmnWr (SMU_ARG, Argument);
  SmnWr (SMU_MSG, Message);
  Response = SmuPoll (TimeoutMs);
  if (OutArgument != NULL) {
    *OutArgument = SmnRd (SMU_ARG);
  }

  Log ("AmdIgpuReset:   SMU message %u arg %u: before 0x%x, response 0x%x\n", Message, Argument, Pre, Response);
  return Response == 1;
}

STATIC
VOID
LogState (
  IN CONST CHAR8  *When
  )
{
  Log (
    "AmdIgpuReset: %a: PSP 0x%08x MEMSIZE 0x%08x GRBM_STATUS 0x%08x CP_STAT 0x%08x RLC_CNTL 0x%08x\n",
    When,
    Rd (PSP_C2P_64),
    Rd (MEMSIZE),
    Rd (GRBM_STATUS),
    Rd (CP_STAT),
    Rd (RLC_CNTL)
    );
}

STATIC
BOOLEAN
PspDestroyRings (
  VOID
  )
{
  UINT32  Value;
  UINTN   Ms;

  // psp_v13_0_ring_stop(): write the command, wait 20 ms, poll for the response flag
  Wr (PSP_C2P_64, PSP_DESTROY_RINGS);
  gBS->Stall (20 * 1000);
  for (Ms = 0; Ms < 2000; Ms++) {
    Value = Rd (PSP_C2P_64);
    if ((Value & PSP_RESP_MASK) == PSP_RESP_DONE) {
      Log ("AmdIgpuReset:   PSP destroy rings: done (0x%x)\n", Value);
      return TRUE;
    }

    gBS->Stall (1000);
  }

  Log ("AmdIgpuReset:   PSP destroy rings: no response (0x%x)\n", Value);
  return FALSE;
}

STATIC
BOOLEAN
Mode2Reset (
  VOID
  )
{
  UINT32   Saved[64];
  UINT32   Now[64];
  UINT16   Command;
  UINT16   NoMaster;
  UINTN    Index;
  UINTN    Ms;
  BOOLEAN  Ok;

  // nv_asic_mode2_reset(): clear bus master, cache config space, reset,
  // restore config space, wait until NBIO reports a memory size again
  mPciIo->Pci.Read (mPciIo, EfiPciIoWidthUint16, PCI_COMMAND_OFFSET, 1, &Command);
  NoMaster = Command & (UINT16) ~EFI_PCI_COMMAND_BUS_MASTER;
  mPciIo->Pci.Write (mPciIo, EfiPciIoWidthUint16, PCI_COMMAND_OFFSET, 1, &NoMaster);
  mPciIo->Pci.Read (mPciIo, EfiPciIoWidthUint32, 0, 64, Saved);

  Ok = SmuSend (MSG_GFX_DEVICE_DRIVER_RESET, MODE2_RESET, 20000, FALSE, NULL);

  mPciIo->Pci.Read (mPciIo, EfiPciIoWidthUint32, 0, 64, Now);
  for (Index = 0; Index < 64; Index++) {
    if ((Index != PCI_COMMAND_OFFSET / 4) && (Saved[Index] != Now[Index])) {
      Log ("AmdIgpuReset:   config 0x%02x changed 0x%08x -> 0x%08x, restoring\n", (UINT32)(Index * 4), Saved[Index], Now[Index]);
      mPciIo->Pci.Write (mPciIo, EfiPciIoWidthUint32, (UINT32)(Index * 4), 1, &Saved[Index]);
    }
  }

  mPciIo->Pci.Write (mPciIo, EfiPciIoWidthUint16, PCI_COMMAND_OFFSET, 1, &Command);
  for (Ms = 0; (Rd (MEMSIZE) == MAX_UINT32) && (Ms < 2000); Ms++) {
    gBS->Stall (1000);
  }

  return Ok;
}

/**
  PciBusDxe numbers BARs logically: a 64-bit BAR uses two config registers but one BarIndex
  (PciEnumeratorSupport.c, PciParseBar()). BAR0 and BAR2 of this iGPU are 64-bit, so the
  register BAR at config offset 0x24 is not BarIndex 5. Find it by matching its address.
**/
STATIC
BOOLEAN
FindRegisterBar (
  VOID
  )
{
  UINT32                             Bar;
  UINT8                              Index;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Desc;
  BOOLEAN                            Match;

  mPciIo->Pci.Read (mPciIo, EfiPciIoWidthUint32, REG_BAR_CFG, 1, &Bar);
  if ((Bar & 0x7) != 0) {
    Log ("AmdIgpuReset: config 0x24 = 0x%08x is not a 32-bit memory BAR\n", Bar);
    return FALSE;
  }

  for (Index = 0; Index < PCI_MAX_BAR; Index++) {
    if (EFI_ERROR (mPciIo->GetBarAttributes (mPciIo, Index, NULL, (VOID **)&Desc))) {
      continue;
    }

    Match = (Desc->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) &&
            (Desc->AddrSpaceGranularity == 32) &&
            (Desc->AddrRangeMin == (Bar & ~0xFu)) &&
            (Desc->AddrLen > PSP_C2P_64 + 4);
    Log (
      "AmdIgpuReset:   PciIo BAR %u: type %u, %lu-bit, base 0x%lx, length 0x%lx%a\n",
      Index,
      Desc->ResType,
      Desc->AddrSpaceGranularity,
      Desc->AddrRangeMin,
      Desc->AddrLen,
      Match ? "  <- registers" : ""
      );
    gBS->FreePool (Desc);
    if (Match) {
      mRegBar = Index;
      return TRUE;
    }
  }

  Log ("AmdIgpuReset: no PciIo BAR matches config 0x24 = 0x%08x\n", Bar);
  return FALSE;
}

STATIC
VOID
ResetIgpu (
  VOID
  )
{
  UINT64  Attributes;
  UINT32  Psp;
  UINT32  Command;
  UINT32  Version;
  UINTN   Ms;

  if (EFI_ERROR (mPciIo->Attributes (mPciIo, EfiPciIoAttributeOperationGet, 0, &Attributes))) {
    Attributes = 0;
  }

  mPciIo->Attributes (mPciIo, EfiPciIoAttributeOperationEnable, EFI_PCI_IO_ATTRIBUTE_MEMORY, NULL);

  if (!FindRegisterBar ()) {
    goto Done;
  }

  LogState ("before");
  if (Rd (MEMSIZE) == MAX_UINT32) {
    Log ("AmdIgpuReset: BAR5 reads all-ones, device not responding; leaving it alone\n");
    goto Done;
  }

  Psp     = Rd (PSP_C2P_64);
  Command = Psp & PSP_CMD_MASK;
  if (!ALWAYS_RESET && (Command != 0x00010000) && (Command != 0x00020000)) {
    Log ("AmdIgpuReset: no PSP ring left behind, no reset needed\n");
    goto Done;
  }

  if (!SmuSend (MSG_GET_SMU_VERSION, 0, 2000, TRUE, &Version)) {
    Log ("AmdIgpuReset: SMU mailbox not trusted; nothing reset\n");
    goto Done;
  }

  Log ("AmdIgpuReset:   SMU firmware %u.%u.%u\n", (Version >> 16) & 0xFF, (Version >> 8) & 0xFF, Version & 0xFF);

  SmuSend (MSG_DISALLOW_GFXOFF, 0, 2000, FALSE, NULL);
  for (Ms = 0; (Rd (GRBM_STATUS) == MAX_UINT32) && (Ms < 1000); Ms++) {
    gBS->Stall (1000);
  }

  PspDestroyRings ();

  if (Mode2Reset ()) {
    Log ("AmdIgpuReset: MODE2 reset acknowledged\n");
  } else {
    Log ("AmdIgpuReset: MODE2 reset NOT acknowledged\n");
  }

  LogState ("after");

Done:
  mPciIo->Attributes (mPciIo, EfiPciIoAttributeOperationSet, Attributes, NULL);
}

STATIC
VOID
TryDevice (
  IN EFI_PCI_IO_PROTOCOL  *PciIo
  )
{
  UINT32  Id;

  if (mDone || EFI_ERROR (PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, 0, 1, &Id)) || (Id != IGPU_ID)) {
    return;
  }

  mDone  = TRUE;
  mPciIo = PciIo;
  Log ("AmdIgpuReset: found 1002:13c0\n");
  ResetIgpu ();
}

STATIC
VOID
EFIAPI
OnPciIo (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_HANDLE           Handle;
  UINTN                Size;
  EFI_PCI_IO_PROTOCOL  *PciIo;

  while (!mDone) {
    Size = sizeof (Handle);
    if (EFI_ERROR (gBS->LocateHandle (ByRegisterNotify, NULL, mRegistration, &Size, &Handle))) {
      break;
    }

    if (!EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiPciIoProtocolGuid, (VOID **)&PciIo))) {
      TryDevice (PciIo);
    }
  }

  if (mDone) {
    gBS->CloseEvent (Event);
  }
}

EFI_STATUS
EFIAPI
AmdIgpuResetEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE           *Handles;
  UINTN                Count;
  UINTN                Index;
  EFI_PCI_IO_PROTOCOL  *PciIo;

  mDebugCon = (IoRead8 (DEBUGCON_PORT) == DEBUGCON_MAGIC);
  Log ("AmdIgpuReset: loaded\n");

  //
  // The iGPU's PciIo normally exists already: PciBusDxe installs it before dispatching
  // option ROMs, and the GPU function is enumerated before its audio function.
  //
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles))) {
    for (Index = 0; (Index < Count) && !mDone; Index++) {
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPciIoProtocolGuid, (VOID **)&PciIo))) {
        TryDevice (PciIo);
      }
    }

    gBS->FreePool (Handles);
  }

  //
  // Otherwise wait for it to appear.
  //
  if (!mDone) {
    Log ("AmdIgpuReset: 1002:13c0 not enumerated yet, waiting for it\n");
    if (!EFI_ERROR (gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnPciIo, NULL, &mEvent))) {
      gBS->RegisterProtocolNotify (&gEfiPciIoProtocolGuid, mEvent, &mRegistration);
    }
  }

  return EFI_SUCCESS;
}
