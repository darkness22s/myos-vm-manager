#!/bin/bash
# build.sh - builds MyOS into build/os.img, a 16 MiB BIOS-bootable disk image.
#   LBA 0       MBR (stage 1, partition table)
#   LBA 1-16    stage 2 bootloader
#   LBA 17..    kernel (flat binary, linked at 1 MiB)
#   LBA 2048..  FAT16 volume labelled MYOS (the filesystem)
set -euo pipefail
cd "$(dirname "$0")"

B=build
rm -rf "$B"
mkdir -p "$B"

IMG_SECTORS=32768              # 16 MiB
PART_START=2048                # volume starts at 1 MiB
PART_SECTORS=$((IMG_SECTORS - PART_START))
KERNEL_LBA=17

CFLAGS=(-m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra
        -fno-stack-protector -fno-stack-clash-protection -fcf-protection=none
        -fno-pic -fno-pie -fno-asynchronous-unwind-tables -fno-builtin
        -fno-tree-loop-distribute-patterns -nostdlib)

nasm -f elf32 kernel/start.asm -o "$B/start.o"
for f in console interrupts disk lib shell main; do
    gcc "${CFLAGS[@]}" -c "kernel/$f.c" -o "$B/$f.o"
done
ld -m elf_i386 -T kernel/linker.ld -o "$B/kernel.elf" \
   "$B/start.o" "$B/console.o" "$B/interrupts.o" "$B/disk.o" \
   "$B/lib.o" "$B/shell.o" "$B/main.o"
objcopy -O binary "$B/kernel.elf" "$B/kernel.bin"

KSIZE=$(stat -c %s "$B/kernel.bin")
KSECTORS=$(( (KSIZE + 511) / 512 ))
truncate -s $((KSECTORS * 512)) "$B/kernel.bin"
if [ $((KERNEL_LBA + KSECTORS)) -gt $PART_START ]; then
    echo "kernel does not fit before the filesystem"; exit 1
fi

nasm -f bin boot/mbr.asm -DPART_SECTORS=$PART_SECTORS -o "$B/mbr.bin"
nasm -f bin boot/stage2.asm -DKERNEL_SECTORS=$KSECTORS -DKERNEL_LBA=$KERNEL_LBA -o "$B/stage2.bin"
S2SIZE=$(stat -c %s "$B/stage2.bin")
if [ "$S2SIZE" -gt 8192 ]; then
    echo "stage 2 is $S2SIZE bytes; stage 1 loads only 8192"; exit 1
fi

dd if=/dev/zero of="$B/os.img" bs=512 count=$IMG_SECTORS status=none
mkfs.fat -F 16 -n MYOS -C "$B/fat.img" $((PART_SECTORS / 2)) > /dev/null
dd if="$B/fat.img"    of="$B/os.img" bs=512 seek=$PART_START conv=notrunc status=none
dd if="$B/mbr.bin"    of="$B/os.img" bs=512 seek=0 conv=notrunc status=none
dd if="$B/stage2.bin" of="$B/os.img" bs=512 seek=1 conv=notrunc status=none
dd if="$B/kernel.bin" of="$B/os.img" bs=512 seek=$KERNEL_LBA conv=notrunc status=none

echo "kernel: $KSIZE bytes ($KSECTORS sectors)   stage 2: $S2SIZE bytes"
echo "built $B/os.img"
