# MyOS 0.1

A small operating system written from scratch. It does not use Linux, GRUB, or
any existing OS. It has its own two-stage BIOS bootloader, a 32-bit x86 kernel,
a native VGA graphics desktop, keyboard and PS/2 mouse input, a built-in shell,
and a FAT16 filesystem that reads and writes real files on disk.

## Desktop

The BIOS boot path selects 320x200 256-color VGA mode. MyOS draws a desktop,
icons, windows, and a taskbar directly to the VGA framebuffer; it uses about
64 KiB of video memory and boots with the VM Manager's existing memory configuration.

- **Files** lists real files in the FAT16 root directory.
- **Terminal** runs the existing shell (`help`, `ls`, `cat`, `write`, `rm`, `df`, `mem`, `uptime`, and more). Type `exit` to return to the desktop.
- **System Monitor** reports live heap use and uptime.
- **Settings** changes the desktop accent color.
- Click icons/taskbar buttons with a PS/2 mouse, or press **1** Files, **2** Terminal, **3** System Monitor, **4** Settings. **Esc** returns to the desktop.

The VM Manager remains a host-side app. It boots MyOS in QEMU and shows the guest's graphical screen; it is not an executable inside the guest OS. The manager disables the guest serial console so MyOS boots to the desktop. A serial-backed QEMU launch still enters the shell for recovery and automated tests.

## What works (verified in QEMU)

- Bootloader: MBR, then stage 2 (A20, BIOS disk reads), then 32-bit protected mode, then the kernel.
- Kernel: IDT with 48 vectors, remapped PIC, 100 Hz PIT timer, and an exception handler that reports the faulting address.
- Display and input: VGA mode 13h framebuffer desktop, PS/2 keyboard and polled PS/2 mouse.
- Memory: 1 MiB first-fit heap with block merging, and a 16 KiB stack.
- Disk: ATA PIO driver on the primary IDE channel.
- Filesystem: FAT16 volume with 8.3 names in the root directory. Supports list, read, create, overwrite, delete, and free-space. Overwrites are ordered so that a failure midway should leave the old file intact (reasoned about, not tested).
- Shell commands: help, ls, cat, write, rm, df, echo, mem, uptime, clear, reboot, halt, crash, and exit.

## Test results

- `tests/smoke.sh` exercises the serial shell, FAT16 reads and writes, reboot persistence, and host-side FAT checks.
- `tests/keyboard_test.py` captures the rendered desktop and checks keyboard app shortcuts, terminal input, and a divide-by-zero panic through QEMU's monitor.
- The GitHub Actions workflow builds the disk image, runs both QEMU suites, and uploads `os.img` as an artifact.

## Size

- Kernel code: 11,290 bytes. Stage 2: 358 bytes.
- About 2,000 lines of source, including tests and the build script.

## Build and run

Requirements: gcc with 32-bit support, nasm, binutils, mtools, dosfstools, QEMU (i386), and python3. These are build tools only, not runtime dependencies.

    bash build.sh                                      # produces build/os.img (16 MiB)
    qemu-system-i386 -m 16 -serial none -hda build/os.img  # graphical desktop
    bash tests/smoke.sh                                # serial shell and FAT tests
    python3 tests/keyboard_test.py                     # graphical apps and terminal

To put it on a USB stick for a BIOS PC (this erases the stick, so check the device name twice):

    sudo dd if=myos.img of=/dev/sdX bs=4M conv=fsync

## How it boots

1. The BIOS loads the 512-byte MBR at 0x7C00. It reads stage 2 (16 sectors) to 0x7E00 and jumps there.
2. Stage 2 enables A20, reads the kernel to 0x10000 with BIOS disk calls, selects VGA mode 13h, switches to 32-bit protected mode with a flat GDT, copies the kernel to 0x100000, and jumps to it.
3. The kernel clears BSS, sets up the console, heap, IDT, PIC, and timer, enables interrupts, mounts FAT16 when present, then starts the desktop (or the shell when a serial port is attached).

Disk layout: LBA 0 holds the MBR, LBA 1 to 16 hold stage 2, LBA 17 onward holds the kernel, and LBA 2048 onward holds the FAT16 volume.

## Limitations (please read)

- Tested only in QEMU. I have not run it on a real PC, so boot on real hardware is unverified.
- BIOS boot only. There is no UEFI support.
- The disk driver uses the legacy IDE interface. From a USB stick on a real PC, the kernel will boot but will not find its filesystem and runs without storage. Supporting USB storage needs a USB host controller driver (xHCI, EHCI) and the USB mass-storage protocol. This is the largest step toward real hardware.
- The desktop is intentionally low resolution (320x200, 256 colors) to keep graphics memory use minimal; it is not a high-resolution composited desktop.
- The keyboard and mouse are PS/2 only. Many modern PCs have only USB input, which works only if firmware emulates PS/2 (legacy USB support).
- There is no paging, no processes, and no user mode. Desktop apps are built-in kernel components and run one at a time; the VM Manager is a host-side application.
- Filesystem: FAT16 only, root directory only, 8.3 names, and at most 4096 bytes per file written from the shell.
- Some failure paths are reasoned about but not tested: root directory full, and disk full during a write.
- "Boots in a second" on real hardware depends on firmware. A PC's firmware usually takes several seconds before any bootloader runs. The part this project controls takes milliseconds.

## Source layout

    boot/mbr.asm            stage 1: MBR, loads stage 2, partition table
    boot/stage2.asm         stage 2: A20, loads the kernel, enters protected mode
    kernel/start.asm        entry point, BSS clearing, 48 interrupt stubs
    kernel/main.c           kmain: setup, mount, and launch desktop or serial shell
    kernel/desktop.c        mode-13h desktop, app windows, PS/2 mouse polling
    kernel/interrupts.c     IDT, PIC, PIT timer, keyboard, exception panic
    kernel/console.c        VGA/desktop output, COM1 serial, kprintf
    kernel/disk.c           ATA PIO driver, FAT16 filesystem
    kernel/lib.c            string functions, heap allocator
    kernel/shell.c          line editor and commands
    kernel/linker.ld        links the kernel at 1 MiB
    build.sh                builds build/os.img
    tests/smoke.sh          end-to-end test over serial, plus host checks
    tests/keyboard_test.py  keyboard, VGA, and exception test via the QEMU monitor
