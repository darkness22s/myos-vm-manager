#!/usr/bin/env python3
"""VM Manager: a local web page for running several QEMU virtual machines.

Standard library only. Start it with `python3 server.py`, then open
http://127.0.0.1:8080 in a browser. Running VMs keep going if this server stops.
"""
import json
import os
import re
import secrets
import shutil
import socket
import struct
import subprocess
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
VMS_DIR = DATA / "vms"
CONFIG = DATA / "vms.json"
DEFAULT_DISK = HERE.parent / "disks" / "myos.img"
PORT = int(os.environ.get("VM_MANAGER_PORT", "8080"))
ARCHES = {"i386": "qemu-system-i386", "x86_64": "qemu-system-x86_64"}
KEY_NAME = re.compile(r"[a-z0-9_-]{1,24}")
POSIX = os.name == "posix"
# On Windows, a VM started from a console shares that console's Ctrl+C and window-close events.
# Detaching it keeps the VM running when the manager's window is closed or interrupted.
DETACHED = (subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP) if os.name == "nt" else 0

CONFIG_LOCK = threading.RLock()
GUARD = threading.Lock()
VM_LOCKS, MON_LOCKS = {}, {}   # per VM: one operation at a time, one monitor connection at a time
PROCS = {}                     # VM id -> Popen, for QEMU processes this server started


class ApiError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def per_vm_lock(table, vm_id):
    with GUARD:
        return table.setdefault(vm_id, threading.Lock())


# ---- saved VM definitions (data/vms.json) ----

def load_vms():
    with CONFIG_LOCK:
        return json.loads(CONFIG.read_text()) if CONFIG.exists() else []


def put_vm(vm, remove=False):
    with CONFIG_LOCK:
        vms = load_vms()
        ids = [v["id"] for v in vms]
        if remove:
            vms = [v for v in vms if v["id"] != vm["id"]]
        elif vm["id"] in ids:
            vms[ids.index(vm["id"])] = vm
        else:
            vms.append(vm)
        DATA.mkdir(parents=True, exist_ok=True)
        tmp = CONFIG.with_suffix(".tmp")
        tmp.write_text(json.dumps(vms, indent=2))
        tmp.replace(CONFIG)


def get_vm(vm_id):
    for vm in load_vms():
        if vm["id"] == vm_id:
            return vm
    raise ApiError(404, "no such VM")


def vm_info(vm):
    return {"id": vm["id"], "name": vm["name"], "memory": vm["memory"],
            "arch": vm["arch"], "running": is_running(vm)}


# ---- QEMU processes and their monitors ----

def qemu_binary(arch):
    return os.environ.get("QEMU_" + arch.upper()) or shutil.which(ARCHES[arch])


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def is_running(vm):
    proc = PROCS.get(vm["id"])
    if proc and proc.poll() is not None:
        PROCS.pop(vm["id"], None)              # reap a QEMU that exited by itself
    lock = per_vm_lock(MON_LOCKS, vm["id"])
    if not lock.acquire(blocking=False):       # a monitor command is in progress: the VM is up
        return True
    try:
        with socket.create_connection(("127.0.0.1", vm["port"]), timeout=0.3):
            return True
    except OSError:
        return False
    finally:
        lock.release()


def read_prompt(sock):
    data = b""
    while not data.endswith(b"(qemu) "):
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk
    return data.decode(errors="replace")


def hmp(vm, *commands):
    """Runs QEMU monitor commands on one VM and returns each reply as text."""
    with per_vm_lock(MON_LOCKS, vm["id"]):
        replies = []
        with socket.create_connection(("127.0.0.1", vm["port"]), timeout=5) as s:
            read_prompt(s)                     # the monitor banner and first prompt
            for command in commands:
                s.sendall(command.encode() + b"\n")
                replies.append(read_prompt(s))
    return replies


def start_vm(vm):
    if is_running(vm):
        return
    binary = qemu_binary(vm["arch"])
    if not binary:
        raise ApiError(500, f"{ARCHES[vm['arch']]} was not found. Install QEMU (see README.md).")
    folder = VMS_DIR / vm["id"]
    with open(folder / "qemu.log", "w") as log:
        proc = subprocess.Popen(
            [binary, "-name", vm["name"], "-m", str(vm["memory"]),
             "-hda", str(folder / "disk.img"), "-display", "none",
             "-monitor", f"tcp:127.0.0.1:{vm['port']},server,nowait",
             "-serial", f"file:{folder / 'serial.log'}",
             "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
            start_new_session=POSIX, creationflags=DETACHED)           # own session: closing this page never kills the VM
    PROCS[vm["id"]] = proc
    vm["pid"] = proc.pid
    put_vm(vm)
    for _ in range(100):                       # wait up to 5 seconds for the monitor to listen
        if is_running(vm):
            return
        if proc.poll() is not None:
            break
        time.sleep(0.05)
    PROCS.pop(vm["id"], None)
    output = (folder / "qemu.log").read_text(errors="replace")[-800:].strip()
    raise ApiError(500, "QEMU did not start. " + (output or "It printed nothing; see qemu.log."))


def stop_vm(vm, force=False):
    if not is_running(vm):
        return
    if not force:
        try:
            hmp(vm, "system_powerdown")        # ACPI power button; guests that ignore it keep running
        except OSError:
            pass
        deadline = time.time() + 4
        while is_running(vm) and time.time() < deadline:
            time.sleep(0.2)
    if is_running(vm):
        try:
            hmp(vm, "quit")
        except OSError:
            pass
    deadline = time.time() + 3
    while is_running(vm) and time.time() < deadline:
        time.sleep(0.1)
    if is_running(vm):                         # last resort
        proc = PROCS.get(vm["id"])
        if proc:
            proc.kill()
        elif POSIX and vm.get("pid"):
            os.kill(vm["pid"], 9)
        elif vm.get("pid"):                    # Windows, after the page server was restarted
            subprocess.run(["taskkill", "/PID", str(vm["pid"]), "/F"], capture_output=True)
    proc = PROCS.pop(vm["id"], None)
    if proc:
        proc.wait(timeout=5)


def delete_vm(vm):
    stop_vm(vm, force=True)
    put_vm(vm, remove=True)
    shutil.rmtree(VMS_DIR / vm["id"], ignore_errors=True)


def create_vm(body):
    name = str(body.get("name", "")).strip()
    if not re.fullmatch(r"[A-Za-z0-9 _.-]{1,32}", name):
        raise ApiError(400, "Name: 1 to 32 letters, digits, spaces, or _ . -")
    try:
        memory = int(body.get("memory", 16))
    except (TypeError, ValueError):
        raise ApiError(400, "Memory must be a whole number of MB")
    if not 8 <= memory <= 4096:
        raise ApiError(400, "Memory must be between 8 and 4096 MB")
    arch = body.get("arch", "i386")
    if arch not in ARCHES:
        raise ApiError(400, "Architecture must be i386 or x86_64")
    text = str(body.get("source", "")).strip()
    source = Path(text).expanduser() if text else Path(".")
    if not source.is_absolute():
        source = HERE / source
    if not text or not source.is_file():
        raise ApiError(400, f"Disk image not found: {text or '(nothing entered)'}")
    vm = {"id": secrets.token_hex(4), "name": name, "memory": memory, "arch": arch,
          "port": free_port(), "pid": None}
    folder = VMS_DIR / vm["id"]
    folder.mkdir(parents=True)
    shutil.copyfile(source, folder / "disk.img")   # each VM gets its own copy of the disk
    put_vm(vm)
    return vm


# ---- screens, keys and the serial log ----

def ppm_to_png(data):
    """Converts the binary PPM (P6) screendump that QEMU writes into a PNG file."""
    fields, i = [], 0
    while len(fields) < 4:                     # magic, width, height, max value
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    if fields[0] != b"P6":
        raise ApiError(500, "unexpected screenshot format")
    width, height = int(fields[1]), int(fields[2])
    pixels = data[i + 1:i + 1 + width * height * 3]
    stride = width * 3
    rows = b"".join(b"\0" + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 1))
            + chunk(b"IEND", b""))


def screenshot_png(vm):
    frame = VMS_DIR / vm["id"] / "screen.ppm"
    hmp(vm, f'screendump "{frame.as_posix()}"')
    if not frame.exists():
        raise ApiError(500, "QEMU did not write a screenshot")
    return ppm_to_png(frame.read_bytes())


def send_keys(vm, keys):
    if not isinstance(keys, list):
        raise ApiError(400, "keys must be a list")
    for key in keys:
        if not isinstance(key, str) or not KEY_NAME.fullmatch(key):
            raise ApiError(400, f"Unsupported key name: {key!r}")
    if keys:
        hmp(vm, *[f"sendkey {key}" for key in keys])


def serial_text(vm, limit=20000):
    path = VMS_DIR / vm["id"] / "serial.log"
    data = path.read_bytes()[-limit:] if path.exists() else b""
    return data.decode(errors="replace").replace("\r", "")


def server_info():
    return {"qemu": {arch: qemu_binary(arch) is not None for arch in ARCHES},
            "default_disk": str(DEFAULT_DISK) if DEFAULT_DISK.is_file() else ""}


# ---- HTTP ----

class Handler(BaseHTTPRequestHandler):
    server_version = "VMManager/0.1"

    def log_message(self, *args):              # keep the terminal quiet
        pass

    def send_bytes(self, status, body, ctype):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, status, payload):
        self.send_bytes(status, json.dumps(payload).encode(), "application/json")

    def read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(length)) if length else {}

    def handle_request(self, method):
        if self.headers.get("Host", "").rsplit(":", 1)[0] not in ("127.0.0.1", "localhost"):
            return self.send_json(403, {"error": f"Open this page at http://127.0.0.1:{PORT}"})
        path = self.path.split("?", 1)[0]
        try:
            if method == "GET" and path == "/":
                return self.send_bytes(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
            if method == "GET" and path == "/api/info":
                return self.send_json(200, server_info())
            if method == "GET" and path == "/api/vms":
                return self.send_json(200, [vm_info(vm) for vm in load_vms()])
            if method == "POST" and path == "/api/vms":
                return self.send_json(201, vm_info(create_vm(self.read_json())))
            match = re.fullmatch(r"/api/vms/([0-9a-f]{8})(?:/(start|stop|keys|screen\.png|serial))?", path)
            if not match:
                raise ApiError(404, "not found")
            vm_id, action = match.groups()
            with per_vm_lock(VM_LOCKS, vm_id):
                vm = get_vm(vm_id)
                if method == "DELETE" and action is None:
                    delete_vm(vm)
                    return self.send_json(200, {"ok": True})
                if method == "GET" and action == "screen.png":
                    return self.send_bytes(200, screenshot_png(vm), "image/png")
                if method == "GET" and action == "serial":
                    return self.send_bytes(200, serial_text(vm).encode(), "text/plain; charset=utf-8")
                if method == "POST" and action == "start":
                    start_vm(vm)
                elif method == "POST" and action == "stop":
                    stop_vm(vm, force=bool(self.read_json().get("force")))
                elif method == "POST" and action == "keys":
                    send_keys(vm, self.read_json().get("keys"))
                else:
                    raise ApiError(404, "not found")
            return self.send_json(200, vm_info(get_vm(vm_id)))
        except ApiError as err:
            self.send_json(err.status, {"error": str(err)})
        except OSError as err:
            self.send_json(409, {"error": f"The VM is not running or not responding ({err})"})
        except Exception as err:               # report the problem in the page and keep serving
            self.send_json(500, {"error": f"{type(err).__name__}: {err}"})

    def do_GET(self):
        self.handle_request("GET")

    def do_POST(self):
        self.handle_request("POST")

    def do_DELETE(self):
        self.handle_request("DELETE")


def main():
    VMS_DIR.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"VM Manager: http://127.0.0.1:{PORT}")
    print("Ctrl+C stops this page server. Running VMs keep running.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("Page server stopped. Any running VMs are still running.")


if __name__ == "__main__":
    main()
