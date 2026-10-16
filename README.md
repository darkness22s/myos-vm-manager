# MyOS workbench

This ZIP contains:

- `vm-manager/`: a web page for running several QEMU virtual machines. You can create, start, stop, and delete VMs, open each VM's screen, type into it, and leave it again.
- `disks/myos.img`: a ready-to-boot MyOS disk image. It is the default disk for new VMs.
- `myos/`: the MyOS source code (bootloader, low-memory graphical desktop, kernel shell, and tests). Rebuild it with `bash build.sh`.

## 1. Install QEMU

You need QEMU (the emulator) and Python 3.9 or newer. Most systems already have Python.

- **Ubuntu / Debian:** `sudo apt install qemu-system-x86`
- **macOS:** `brew install qemu`
- **Windows:** install QEMU for Windows from https://www.qemu.org/download/ and add its folder to your PATH. Details are in "Running on Windows" below.

Check that it works: `qemu-system-i386 --version`

## 2. Start the manager

    cd vm-manager
    python3 server.py

Open http://127.0.0.1:8080 in your browser. If that port is busy, start on another one: `VM_MANAGER_PORT=8090 python3 server.py`.

## 3. Create and run VMs

1. Click **+ New VM**. Give the VM a name, choose its memory and architecture, and check the disk image. It is filled in with `disks/myos.img`.
2. Click **Create VM**. The disk is copied, so each VM has its own disk. Repeat this for as many VMs as you like.
3. Click **Start** on a VM's card, then **Open screen**. MyOS starts on its graphical desktop (16 MiB RAM by default). In the screen, press **1** for Files, **2** for Terminal, **3** for System Monitor, or **4** for Settings. Use `exit` in Terminal to return to the desktop.

## Getting out of a VM

You can always leave, and nothing traps your keyboard or mouse:

- Click **← Back to VMs**, or press **Ctrl+]** anywhere in the screen view.
- Keys go to the VM only while the screen has a green border. Click anywhere outside the screen to stop typing.
- **Power off** asks the guest to shut down, and stops the VM after 4 seconds if the guest does not. **Force stop** stops it immediately.

Inside MyOS, `halt` closes the VM by itself, and `reboot` restarts it.

## Try MyOS

Open Terminal with **2**, then run:

    help
    write NOTE.TXT hello from my VM
    cat NOTE.TXT
    ls

Press `exit` to return to the desktop. Each VM has its own disk, so files written in one VM do not appear in another.

## Troubleshooting

- **"qemu-system-i386 was not found":** install QEMU (step 1), or point the manager at it: `QEMU_I386=/full/path/to/qemu-system-i386 python3 server.py`.
- **Blank or black screen:** wait a second after Start. If it persists, read `vm-manager/data/vms/<id>/qemu.log`.
- **Keys do nothing:** click the screen so its border turns green, then use the displayed app hotkeys (1–4).
- **Serial panel says disabled:** this is expected; the guest uses the graphical VGA screen and keyboard.
- **VMs still run after you close the page:** this is by design. Stop them from the manager, or run `pkill qemu-system-i386` (Linux and macOS).
- **Delete is only available** for stopped VMs.

## Running on Windows

The manager is written to run on Windows, but I have not been able to test it there. If something fails, please send me the error from the terminal and the file `vm-manager\data\vms\<id>\qemu.log`.

1. Install QEMU for Windows from https://www.qemu.org/download/ (the Windows section). Note the folder that holds `qemu-system-i386.exe`, usually `C:\Program Files\qemu`.
2. Either add that folder to your PATH, or tell the manager where the program is. In Command Prompt: `set "QEMU_I386=C:\Program Files\qemu\qemu-system-i386.exe"`. In PowerShell: `$env:QEMU_I386 = "C:\Program Files\qemu\qemu-system-i386.exe"`.
3. Open a terminal in the `vm-manager` folder and run `py server.py`. The `python3` command used above is often missing on Windows; use `python server.py` if `py` is not found.
4. Open http://127.0.0.1:8080 in your browser.

On Windows, each VM starts detached from the manager's window, so closing that window or pressing Ctrl+C there should leave your VMs running. To stop everything, use Force stop in the page, or end the `qemu-system-i386.exe` processes in Task Manager.

If Ctrl+] does nothing on your keyboard, use the Back button. On some European keyboard layouts, the `]` key needs AltGr.

## Notes

- Everything runs locally. The manager accepts connections only from this computer, and each VM's monitor listens on 127.0.0.1 as well. Anyone logged in to this computer can control your VMs.
- The screen refreshes about twice a second using screenshots. Typed keys assume a US keyboard layout.
- Tested on Ubuntu 24.04 with QEMU 8.2. macOS and Windows are not tested yet. See "Running on Windows" below.

## Rebuilding MyOS

    cd myos
    bash build.sh            # creates build/os.img

The GitHub Actions workflow builds the image, runs the QEMU tests, and uploads `os.img` as a downloadable artifact. To use a locally built image in the manager, copy `build/os.img` over `disks/myos.img`.
