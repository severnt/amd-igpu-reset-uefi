# amd-igpu-reset-uefi

UEFI reset for AMD iGPU VFIO passthrough: reset the Granite Ridge iGPU (1002:13C0) from the VM's UEFI.

**Status: experimental.** Tested on an AMD Ryzen 9 9950X3D iGPU. Not tested on other hardware. Use at your own risk.

A UEFI driver, loaded from a PCI option ROM inside the VM, that runs before the AMD GOP driver
on every VM boot (including reboots started inside Windows and boots after a forced stop).

## What it does

- Reads the PSP mailbox. If it shows a ring left by a previous driver (`0x8001xxxx` or
  `0x8002xxxx`), it runs the sequence validated from the host
  (`igpu-mode2-reset.py --disallow-gfxoff --psp-destroy --mode2`):
  GetSmuVersion -> DisallowGfxOff -> PSP destroy rings -> MODE2 reset.
- Fresh host boot (`0x80000000`) or already reset (`0x80030000`): does nothing.
- Set `ALWAYS_RESET` to 1 in `AmdIgpuResetDxe.c` to reset on every boot instead.

## Host setup

Kernel command line used on the tested system:

    vfio_pci.ids=1002:13c0,1002:1640 vfio_pci.disable_vga=1 vfio_iommu_type1.allow_unsafe_interrupts=1 iommu=pt

- Not sure how much of this is required. Some of it may be unnecessary; remove options one at a time to find out.
- `1002:13c0` is the iGPU and `1002:1640` is its audio function on the tested system. Check yours with `lspci -nn`.

Module options, e.g. in `/etc/modprobe.d/vfio.conf`:

    options vfio_iommu_type1 allow_unsafe_interrupts=1
    softdep drm pre: vfio-pci

    blacklist amdgpu

- As above, not sure all of these are needed.
- `blacklist amdgpu` keeps the host from binding the iGPU, which means the host has no amdgpu driver for any AMD GPU.
- Rebuild the initramfs after editing (distro-specific, e.g. `update-initramfs -u`, `mkinitcpio -P` or `dracut -f`).

## VBIOS and GOP ROM

- Both ROMs come from the AMD iGPU on your own CPU. This repo does not ship them.
- How to obtain them (extract from the host's `/sys/firmware/acpi/tables/VFCT`, or use the prebuilt files that guide links to): see
  [isc30/ryzen-gpu-passthrough-proxmox](https://github.com/isc30/ryzen-gpu-passthrough-proxmox).
  That guide notes its prebuilt `AMDGopDriver.rom` might not be compatible with all hardware; if it fails, extract your own.
- Use the VBIOS (e.g. `vbios_13C0.dat`) on the GPU function and the GOP ROM (`AMDGopDriver_*.rom`) concatenated after `AmdIgpuReset.rom` on the audio function, as below.

## Install

1. Put the reset image in front of your GOP ROM (`AmdIgpuReset.rom` is built without the
   "last image" flag, so plain concatenation gives a valid two-image ROM):

       cat AmdIgpuReset.rom AMDGopDriver_9950x3d.rom > AmdIgpuReset+GOP.rom

2. Load `AmdIgpuReset+GOP.rom` on the device that loads the GOP ROM today
   (typically the iGPU's audio function; adjust to your setup). libvirt:

       <hostdev mode='subsystem' type='pci' managed='yes'>
         <source><address domain='0x0000' bus='0x10' slot='0x00' function='0x1'/></source>
         <rom file='/path/to/AmdIgpuReset+GOP.rom'/>
       </hostdev>

   Plain QEMU: `-device vfio-pci,host=0000:10:00.1,romfile=/path/to/AmdIgpuReset+GOP.rom`

3. Leave the VBIOS romfile on the GPU function (10:00.0) as it is.
   RadeonResetBugFix stays uninstalled; no host hook is needed.

## Full libvirt example (tested system)

Both iGPU functions are passed through. The GPU function gets the VBIOS, the audio function gets
the combined reset + GOP ROM:

    <hostdev mode='subsystem' type='pci' managed='yes'>
      <driver name='vfio'/>
      <source>
        <address domain='0x0000' bus='0x10' slot='0x00' function='0x0'/>
      </source>
      <rom file='/var/lib/libvirt/images/vbios_13C0.dat'/>
      <address type='pci' domain='0x0000' bus='0x01' slot='0x00' function='0x0' multifunction='on'/>
    </hostdev>
    <hostdev mode='subsystem' type='pci' managed='yes'>
      <driver name='vfio'/>
      <source>
        <address domain='0x0000' bus='0x10' slot='0x00' function='0x1'/>
      </source>
      <rom file='/var/lib/libvirt/images/AMDGopDriver_13C0+reset.rom'/>
      <address type='pci' domain='0x0000' bus='0x01' slot='0x00' function='0x1'/>
    </hostdev>

`<features>` used:

    <features>
      <acpi/>
      <apic/>
      <hyperv mode='custom'>
        <relaxed state='on'/>
        <vapic state='on'/>
        <spinlocks state='on' retries='8191'/>
        <vpindex state='on'/>
        <runtime state='on'/>
        <synic state='on'/>
        <stimer state='on'/>
        <vendor_id state='on' value='notavm'/>
        <tlbflush state='on'>
          <extended state='on'/>
        </tlbflush>
        <ipi state='on'/>
        <avic state='on'/>
      </hyperv>
      <kvm>
        <hidden state='on'/>
      </kvm>
      <vmport state='off'/>
      <smm state='on'>
        <tseg unit='MiB'>48</tseg>
      </smm>
      <ioapic driver='kvm'/>
      <msrs unknown='ignore'/>
    </features>

### With Looking Glass

Add to the `<qemu:commandline>` block (needs the `xmlns:qemu` namespace on `<domain>`, see below).
The first pair enlarges the 64-bit PCI MMIO window in OVMF; the rest adds the shared memory device
(here backed by `/dev/kvmfr0`, 64 MiB):

    <qemu:arg value='-fw_cfg'/>
    <qemu:arg value='opt/ovmf/X-PciMmio64Mb,string=65536'/>
    <qemu:arg value='-device'/>
    <qemu:arg value='{&apos;driver&apos;:&apos;ivshmem-plain&apos;,&apos;id&apos;:&apos;shmem0&apos;,&apos;memdev&apos;:&apos;looking-glass&apos;}'/>
    <qemu:arg value='-object'/>
    <qemu:arg value='{&apos;qom-type&apos;:&apos;memory-backend-file&apos;,&apos;id&apos;:&apos;looking-glass&apos;,&apos;mem-path&apos;:&apos;/dev/kvmfr0&apos;,&apos;size&apos;:67108864,&apos;share&apos;:true}'/>

## See what it did (optional)

The driver writes to QEMU's debug console (I/O port 0x402), the same channel OVMF uses
(`OvmfPkg/README`). libvirt: add `xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'`
to `<domain>`, then:

    <qemu:commandline>
      <qemu:arg value='-debugcon'/>
      <qemu:arg value='file:/var/log/libvirt/qemu/win-ovmf-debug.log'/>
      <qemu:arg value='-global'/>
      <qemu:arg value='isa-debugcon.iobase=0x402'/>
    </qemu:commandline>

Expected lines:

- every boot: a list of `PciIo BAR n` lines, one marked `<- registers`
- fresh host boot: `AmdIgpuReset: no PSP ring left behind, no reset needed`
- after a Windows session: `PSP destroy rings: done`, `MODE2 reset acknowledged`, and an
  `after:` line with `CP_STAT 0x00000000 RLC_CNTL 0x00000000`

## Test order

1. Fresh host boot, start the VM: expect "no reset needed", iGPU works.
2. Reboot Windows from inside: expect the reset lines, iGPU works.
3. Shut Windows down, start the VM again: same.
4. `virsh destroy`, start the VM again: same.

## Roll back

Point the romfile back at the original `AMDGopDriver_9950x3d.rom`.

## Repository layout

- `AmdIgpuReset.rom`: prebuilt option ROM (no "last image" flag)
- `src/AmdIgpuResetPkg/`: edk2 package (driver source, `.inf`, `.dsc`, `.dec`)
- `src/build.sh`: build script

## Build from source

    src/build.sh /path/to/edk2 [AMDGopDriver_9950x3d.rom]

The ROM is written to the repo root. Needs edk2 with the submodules listed in `build.sh`, plus gcc, make, nasm, uuid-dev, python3.
The prebuilt `AmdIgpuReset.rom` came from edk2 master 34b75da (2026-10-02), GCC 13, RELEASE.
sha256: `32c704bbc3ebd8b7b37c7f7b27a0d80daedfa73cd58e86dd8f33d88bd7100d97`

## What has been verified (emulation and unit tests)

- Builds with edk2 master and GCC 13 (`-Werror`).
- QEMU 8.2 + Ubuntu OVMF 2024.02: the driver loads from the option ROM and the AMD GOP driver
  loads from the second image (`drivers` lists it at ROM offset 0x4200). Same result with
  Ubuntu's Secure Boot build (`OVMF_CODE_4M.secboot.fd`, `OVMF_VARS_4M.ms.fd`,
  SecureBootEnable on).
- Host unit test of the driver source against a mocked iGPU: skip on a clean PSP mailbox,
  command order, bus master off during MODE2, config/attribute restore, late enumeration,
  bounded waits when the SMU never answers.
- Register offsets are identical to `igpu-mode2-reset.py`.
- v0.2: the register BAR is located at runtime. edk2 numbers BARs logically (a 64-bit BAR
  uses one index for two config registers, `PciParseBar()`), so PCI BAR5 is not PciIo index 5
  on this iGPU. v0.1 used index 5 and read all-ones. Checked in QEMU against the AHCI
  controller's BAR at config 0x24, and in the unit test with the iGPU's BAR layout.

Real hardware: tested on a 9950X3D (see Status). Details of what was run and observed are not recorded here.

## Caveats

- If Windows uses BitLocker, keep the recovery key at hand: the new ROM changes what is
  measured at boot.
- Bad register access on this iGPU can hang the host. Keep SSH access while testing.
- Upstream OVMF trusts option ROMs (`PcdOptionRomImageVerificationPolicy|0x00` in
  `OvmfPkgX64.dsc`). Other OVMF builds might not.

## License

See `LICENSE` (MIT). Source files carry an `SPDX-License-Identifier: MIT` header.
