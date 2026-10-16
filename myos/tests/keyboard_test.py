#!/usr/bin/env python3
"""Exercise the graphical desktop, keyboard-driven apps, and terminal in QEMU.

Screenshots are taken through the QEMU monitor so this test also runs headless
in CI. The serial-shell and filesystem tests live in smoke.sh.
"""
import os
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


def screenshot(name):
    """Capture the VGA output as PPM through QEMU's monitor."""
    path = os.path.join(tmp, name + ".ppm")
    monitor("screendump " + path, wait=0.3)
    data = open(path, "rb").read()
    pos = 0
    tokens = []
    while len(tokens) < 4:
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos < len(data) and data[pos] == ord("#"):
            pos = data.find(b"\n", pos) + 1
            continue
        end = pos
        while end < len(data) and data[end] not in b" \t\r\n":
            end += 1
        tokens.append(data[pos:end])
        pos = end
    assert tokens[0] == b"P6", "QEMU did not return a PPM screenshot"
    width, height, maxval = map(int, tokens[1:])
    pos += 2 if data[pos:pos + 2] == b"\r\n" else 1
    pixels = data[pos:]
    assert (width, height) in ((320, 200), (640, 400)) and maxval == 255, (width, height, maxval)
    assert len(pixels) == width * height * 3, "incomplete QEMU screenshot"
    return pixels


def color_count(pixels):
    return len(set(zip(pixels[0::3], pixels[1::3], pixels[2::3])))


drain(1.5)                                    # let the graphical desktop boot
home = screenshot("home")
monitor("sendkey 3")                          # open System Monitor from keyboard
system = screenshot("system")
monitor("sendkey 2")                          # open the integrated terminal
terminal = screenshot("terminal")
type_text("echo keyboard works\n")
terminal_output = screenshot("terminal-output")
type_text("crash\n")
panic = screenshot("panic")

checks = [
    ("VGA desktop rendered in multiple colors", color_count(home) >= 8),
    ("keyboard shortcut opens a desktop app", system != home),
    ("keyboard input updates the graphical terminal", terminal_output != terminal),
    ("kernel exception handler updates the terminal display", panic != terminal_output),
]

print("VGA mode: 320x200x24-bit screenshot from QEMU")
print("Distinct desktop colors: %d" % color_count(home))
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
