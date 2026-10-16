# MyOS 0.1

A small operating system written from scratch. It does not use Linux, GRUB, or
any existing OS. It has its own two-stage BIOS bootloader, a 32-bit x86 kernel,
a keyboard and serial shell, and a FAT16 filesystem that reads and writes real
files on disk. The disk image is standard FAT16, so you can read and write its
files from your normal PC with mtools or any FAT-aware system.

## What works (verified in QEMU)

- Bootloader: MBR, then stage 2 (A20, BIOS disk reads), then 32-bit protected mode, then the kernel.
- Kernel: IDT with 48 vectors, remapped PIC, 100 Hz PIT timer, and an exception handler that prints a panic with the faulting address instead of silently rebooting.
- Keyboard: PS/2 driver (scan code set 1, US layout). Lowercase typing is tested; the shift key is implemented but not tested.
- Output: VGA text mode with scrolling and cursor, plus COM1 serial in and out.
- Memory: 1 MiB first-fit heap with block merging, and a 16 KiB stack.
- Disk: ATA PIO driver on the primary IDE channel.
- Filesystem: FAT16 volume with 8.3 names in the root directory. Supports list, read, create, overwrite, delete, and free-space. Overwrites are ordered so that a failure midway should leave the old file intact (reasoned about, not tested).
- Shell commands: help, ls, cat, write, rm, df, echo, mem, uptime, clear, reboot, halt, and crash (a deliberate divide-by-zero to test the exception handler).

## Test results

- `tests/smoke.sh`: 18 of 18 pass. Covers the serial shell, a 3-cluster file made on the host, a 2-cluster file written by the guest, overwrite, delete, a reboot that reads the file back, and host checks with mtools and fsck.fat.
- `tests/keyboard_test.py`: 3 of 3 pass. Keys are typed through QEMU's emulated PS/2 keyboard, the screen is read back from VGA memory, and a divide-by-zero produces a panic message.
- Boots and works with 4 MiB of RAM.
- Boot to the shell prompt takes about 0.13 to 0.15 s of wall-clock time in QEMU, including QEMU's own start-up.

## Size

- Kernel code: 11,290 bytes. Stage 2: 358 bytes.
- About 2,000 lines of source, including tests and the build script.

## Build and run

Requirements: gcc with 32-bit support, nasm, binutils, mtools, dosfstools, QEMU (i386), and python3. These are build tools only, not runtime dependencies.

    ./build.sh                                  # produces build/os.img (16 MiB)
    qemu-system-i386 -m 16 -hda build/os.img    # opens a window; type at the keyboard
    bash tests/smoke.sh                         # end-to-end test
    python3 tests/keyboard_test.py              # keyboard and exception test

To put it on a USB stick for a BIOS PC (this erases the stick, so check the device name twice):

    sudo dd if=myos.img of=/dev/sdX bs=4M conv=fsync

## How it boots

1. The BIOS loads the 512-byte MBR at 0x7C00. It reads stage 2 (16 sectors) to 0x7E00 and jumps there.
2. Stage 2 enables A20, reads the kernel to 0x10000 with BIOS disk calls, switches to 32-bit protected mode with a flat GDT, copies the kernel to 0x100000 (1 MiB), and jumps to it.
3. The kernel clears BSS, sets up the console, heap, IDT, PIC, and timer, enables interrupts, mounts the FAT16 volume if its label is MYOS, and starts the shell.

Disk layout: LBA 0 holds the MBR, LBA 1 to 16 hold stage 2, LBA 17 onward holds the kernel, and LBA 2048 onward holds the FAT16 volume.

## Limitations (please read)

- Tested only in QEMU. I have not run it on a real PC, so boot on real hardware is unverified.
- BIOS boot only. There is no UEFI support.
- The disk driver uses the legacy IDE interface. From a USB stick on a real PC, the kernel will boot but will not find its filesystem and runs without storage. Supporting USB storage needs a USB host controller driver (xHCI, EHCI) and the USB mass-storage protocol. This is the largest step toward real hardware.
- The keyboard driver is PS/2 only. Many modern PCs have only USB keyboards, which work only if the firmware emulates PS/2 ("legacy USB support").
- There is no paging, no processes, and no user mode. Everything runs in kernel mode as one thread.
- Filesystem: FAT16 only, root directory only, 8.3 names, and at most 4096 bytes per file written from the shell.
- Some failure paths are reasoned about but not tested: root directory full, and disk full during a write.
- "Boots in a second" on real hardware depends on firmware. A PC's firmware usually takes several seconds before any bootloader runs. The part this project controls takes milliseconds.

## Source layout

    boot/mbr.asm            stage 1: MBR, loads stage 2, partition table
    boot/stage2.asm         stage 2: A20, loads the kernel, enters protected mode
    kernel/start.asm        entry point, BSS clearing, 48 interrupt stubs
    kernel/main.c           kmain: setup, mount, start the shell
    kernel/interrupts.c     IDT, PIC, PIT timer, keyboard, exception panic
    kernel/console.c        VGA text, COM1 serial, kprintf
    kernel/disk.c           ATA PIO driver, FAT16 filesystem
    kernel/lib.c            string functions, heap allocator
    kernel/shell.c          line editor and commands
    kernel/linker.ld        links the kernel at 1 MiB
    build.sh                builds build/os.img
    tests/smoke.sh          end-to-end test over serial, plus host checks
    tests/keyboard_test.py  keyboard, VGA, and exception test via the QEMU monitor
