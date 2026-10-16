/* kernel/main.c - kernel entry, called from kernel/start.asm once the stack
   and BSS are ready. */
#include "kernel.h"

extern char _end[];                /* first byte past the kernel image (linker.ld) */

void kmain(void)
{
    console_clear();
    serial_init();
    kprintf("MyOS 0.1 (32-bit x86 kernel)\n");

    u32 heap = ((u32)_end + 15u) & ~15u;
    heap_init((void *)heap, 1024u * 1024u);

    idt_init();
    pic_init();
    pit_init(100);
    cpu_sti();

    if (disk_init() == 0)
        kprintf("disk: MyOS FAT16 volume mounted\n");
    else
        kprintf("disk: no MyOS volume found, running without storage\n");

    /* The manager guest boots into the graphical desktop when no COM1 backend
       is attached. Serial-backed invocations keep the shell for CI and recovery. */
    if (serial_is_up())
        shell_run();
    else
        desktop_run();
}
