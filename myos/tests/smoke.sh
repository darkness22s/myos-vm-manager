#!/bin/bash
# tests/smoke.sh - end-to-end test of MyOS in QEMU.
#   1. A file made on the host (5000 bytes = 3 clusters) is copied onto the
#      disk with mtools; the guest must read it back exactly.
#   2. The guest writes a 4000-byte file (2 clusters) through the shell, reads
#      it back, and lists and deletes files.
#   3. The host checks the disk with mtools and fsck.fat.
#   4. The machine is rebooted and the guest must still read the file.
cd "$(dirname "$0")/.." || exit 1

TMP=$(mktemp -d)
IMG="$TMP/disk.img"
LOG="$TMP/serial.log"
IN="$TMP/input.fifo"
cp build/os.img "$IMG"

# Test data: base64 of random bytes, so there are no spaces or control characters.
HOSTDATA=$(head -c 3750 /dev/urandom | base64 -w0)    # 5000 characters
GUESTDATA=$(head -c 3000 /dev/urandom | base64 -w0)   # 4000 characters
printf '%s' "$HOSTDATA" > "$TMP/host.txt"
mcopy -i "$IMG@@1M" "$TMP/host.txt" ::HOSTDATA.TXT

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
QEMU_ARGS=(-m 16 -display none -monitor none -serial stdio -no-reboot
           -hda "$IMG" -device isa-debug-exit,iobase=0xf4,iosize=0x04)

mkfifo "$IN"
start=$(now_ms)
qemu-system-i386 "${QEMU_ARGS[@]}" < "$IN" > "$LOG" 2>&1 &
QPID=$!
exec 3> "$IN"
( sleep 90; kill "$QPID" 2>/dev/null ) &
WATCHDOG=$!

fail_exit() { echo "FAIL: $1 (log: $LOG)"; kill "$QPID" 2>/dev/null; exit 1; }

wait_for() {                       # wait up to 20 s for a pattern in the log
    local t0; t0=$(date +%s)
    until grep -q "$1" "$LOG" 2>/dev/null; do
        [ $(( $(date +%s) - t0 )) -ge 20 ] && fail_exit "timed out waiting for '$1'"
        sleep 0.01
    done
}

prompts() { grep -c 'myos> ' "$LOG" 2>/dev/null; }
wait_prompt() {                    # wait until the shell has printed N prompts
    local want=$1 t0; t0=$(date +%s)
    while [ "$(prompts)" -lt "$want" ]; do
        [ $(( $(date +%s) - t0 )) -ge 20 ] && fail_exit "shell prompt #$want never appeared"
        sleep 0.02
    done
}

NEXT=1
run_cmd() {                        # type one line, wait for the next prompt
    printf '%s\r' "$1" >&3
    NEXT=$((NEXT + 1))
    wait_prompt "$NEXT"
}

wait_for 'Loading kernel'
t_loader=$(( $(now_ms) - start ))
wait_prompt 1
t_prompt=$(( $(now_ms) - start ))

run_cmd "echo hello from the serial port"
run_cmd "write A.TXT scratch file"
run_cmd "write B.TXT the kernel wrote this file"
run_cmd "cat B.TXT"
run_cmd "write B.TXT second version of the file"
run_cmd "cat B.TXT"
run_cmd "cat HOSTDATA.TXT"
run_cmd "write GUESTBIG.TXT $GUESTDATA"
run_cmd "cat GUESTBIG.TXT"
run_cmd "ls"
run_cmd "rm A.TXT"
run_cmd "ls"
run_cmd "df"
run_cmd "mem"
run_cmd "uptime"
printf 'halt\r' >&3
wait "$QPID"
qstatus=$?
exec 3>&-
kill "$WATCHDOG" 2>/dev/null

tr -d '\r' < "$LOG" > "$TMP/clean.log"
echo "Transcript of the kernel's serial output (long lines cut to 110 columns):"
sed -n '/MyOS 0.1/,$p' "$TMP/clean.log" | cut -c1-110 | sed 's/^/    | /'
echo

# Second boot on the same disk: the guest must still read GUESTBIG.TXT.
( sleep 2; printf 'cat GUESTBIG.TXT\r'; sleep 1; printf 'halt\r'; sleep 1 ) \
    | timeout 30 qemu-system-i386 "${QEMU_ARGS[@]}" > "$TMP/reboot.log" 2>&1
tr -d '\r' < "$TMP/reboot.log" > "$TMP/clean2.log"

PASS=0
FAIL=0
report() {
    if [ "$2" -eq 0 ]; then
        echo "  PASS  $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL  $1"; FAIL=$((FAIL + 1))
    fi
}

echo "Guest checks (shell output over serial):"
grep -qx 'hello from the serial port' "$TMP/clean.log";   report "typed command runs (echo)" $?
grep -qx 'the kernel wrote this file' "$TMP/clean.log";   report "cat prints a file written by the guest" $?
grep -qx 'second version of the file' "$TMP/clean.log";   report "overwrite replaces the file" $?
grep -qxF "$HOSTDATA" "$TMP/clean.log";                   report "reads a 5000-byte host-made file (3 clusters)" $?
grep -qxF "$GUESTDATA" "$TMP/clean.log";                  report "writes and reads back a 4000-byte file (2 clusters)" $?
grep -q  'HOSTDATA.TXT  5000 bytes' "$TMP/clean.log";     report "ls shows the host-made file with its size" $?
grep -q  'GUESTBIG.TXT  4000 bytes' "$TMP/clean.log";     report "ls shows the guest-made file with its size" $?
grep -qx 'deleted A.TXT' "$TMP/clean.log";                report "rm deletes a file" $?
grep -q  ' of [0-9]* bytes free' "$TMP/clean.log";        report "df reports free space" $?
grep -q  'heap: ' "$TMP/clean.log";                       report "mem reports the heap" $?
grep -qx 'up [0-9.]* s' "$TMP/clean.log";                 report "uptime reports the timer" $?
[ "$qstatus" -eq 33 ];                                    report "halt exits QEMU cleanly (status 33)" $?
grep -qxF "$GUESTDATA" "$TMP/clean2.log";                 report "file survives a reboot (read by the second boot)" $?

echo "Host checks (the same disk, read from outside the VM):"
dd if="$IMG" of="$TMP/volume.img" bs=512 skip=2048 status=none
fsck.fat -n "$TMP/volume.img" > "$TMP/fsck.txt" 2>&1
report "FAT tables consistent (fsck.fat)" $?
[ "$(mtype -i "$IMG@@1M" ::GUESTBIG.TXT 2>/dev/null)" = "$GUESTDATA" ]
report "host reads GUESTBIG.TXT byte-for-byte" $?
[ "$(mtype -i "$IMG@@1M" ::B.TXT 2>/dev/null)" = "second version of the file" ]
report "host reads the overwritten B.TXT" $?
[ "$(mtype -i "$IMG@@1M" ::HOSTDATA.TXT 2>/dev/null)" = "$HOSTDATA" ]
report "HOSTDATA.TXT intact on disk" $?
! mtype -i "$IMG@@1M" ::A.TXT >/dev/null 2>&1
report "A.TXT is gone from the disk" $?

echo
echo "Timing (wall clock on this machine, QEMU emulation):"
echo "    bootloader reached after ${t_loader} ms, shell prompt after ${t_prompt} ms"
echo "$PASS passed, $FAIL failed"
rm -rf "$TMP"
[ "$FAIL" -eq 0 ]
