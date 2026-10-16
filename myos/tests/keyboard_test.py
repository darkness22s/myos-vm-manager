#!/usr/bin/env python3
"""tests/keyboard_test.py - drives the PS/2 keyboard path end to end.

Keys are sent through the QEMU monitor (sendkey), so they reach the kernel
exactly like physical key presses: IRQ1, scan code translation, line editing,
and the shell. The VGA text buffer is read back with the monitor's xp command
to check what is on screen. Finally a deliberate divide-by-zero checks that
the exception handler prints a panic instead of silently rebooting.
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
tmp = tempfile.mkdtemp(prefix="myos-kbd-")
disk = os.path.join(tmp, "disk.img")
sock_path = os.path.join(tmp, "monitor.sock")
shutil.copy(os.path.join(ROOT, "build", "os.img"), disk)

qemu = subprocess.Popen(
    ["qemu-system-i386", "-m", "16", "-display", "none", "-serial", "none",
     "-monitor", "unix:%s,server,nowait" % sock_path, "-hda", disk,
     "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

t0 = time.time()
while not os.path.exists(sock_path) and time.time() - t0 < 5:
    time.sleep(0.02)
mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
mon.connect(sock_path)
mon.settimeout(0.1)


def drain(wait):
    time.sleep(wait)
    data = b""
    while True:
        try:
            chunk = mon.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    return data.decode(errors="replace")


def monitor(cmd, wait=0.1):
    mon.sendall((cmd + "\n").encode())
    return drain(wait)


# QEMU key names for the characters this test types.
KEY_NAMES = {" ": "spc", "\n": "ret", "-": "minus", ".": "dot"}


def type_text(text):
    for ch in text:
        monitor("sendkey " + KEY_NAMES.get(ch, ch), wait=0.08)


def read_screen():
    """Returns the 25 text rows currently shown in VGA memory."""
    out = monitor("xp /4000xb 0xb8000", wait=0.5)
    values = []
    for line in out.splitlines():
        m = re.search(r"(?:^|\s)[0-9a-fA-F]{8,16}:(.*)$", line)
        if m:
            values += [int(h, 16) for h in re.findall(r"(?:0x)?([0-9a-fA-F]{2})\b", m.group(1))]
    chars = values[0::2][:80 * 25]            # every other byte is an attribute
    rows = []
    for r in range(25):
        row = chars[r * 80:(r + 1) * 80]
        rows.append("".join(chr(c) if 32 <= c < 127 else " " for c in row).rstrip())
    return rows


drain(1.5)                                    # let the kernel boot
type_text("echo keyboard works\n")
time.sleep(0.3)
type_text("crash\n")
time.sleep(0.8)
rows = read_screen()

checks = [
    ("keyboard input reaches the shell", "myos> echo keyboard works" in rows),
    ("echo prints the typed text",       "keyboard works" in rows),
    ("divide-by-zero prints a panic",    any("KERNEL PANIC: divide error (vector 0)" in r for r in rows)),
]

print("Screen as read from VGA memory:")
for r in rows:
    if r:
        print("    | " + r)
print()
passed = 0
for desc, ok in checks:
    print("  %s  %s" % ("PASS" if ok else "FAIL", desc))
    passed += ok

monitor("quit", wait=0.3)
try:
    qemu.wait(timeout=5)
except subprocess.TimeoutExpired:
    qemu.kill()
shutil.rmtree(tmp, ignore_errors=True)

print("%d passed, %d failed" % (passed, len(checks) - passed))
sys.exit(0 if passed == len(checks) else 1)
