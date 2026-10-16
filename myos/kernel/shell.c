/* kernel/shell.c - the interactive shell. Input comes from the PS/2 keyboard
   or the serial port, whichever has a key first. */
#include "kernel.h"

static char line[4096];            /* big enough for the largest write command */

static int input_getc(void)
{
    for (;;) {
        int c = kb_pop();
        if (c >= 0)
            return c;
        if (serial_rx_ready())
            return serial_rx();
        cpu_hlt();                 /* sleep until the next interrupt */
    }
}

static void read_line(void)
{
    u32 n = 0;
    for (;;) {
        int c = input_getc();
        if (c == '\r')
            c = '\n';
        if (c == '\n') {
            kputc('\n');
            break;
        }
        if (c == '\b' || c == 127) {
            if (n > 0) {
                n--;
                kputc('\b');
                kputc(' ');
                kputc('\b');
            }
            continue;
        }
        if (c >= ' ' && c < 127 && n < sizeof line - 1) {
            line[n++] = (char)c;
            kputc((char)c);
        }
    }
    while (n > 0 && line[n - 1] == ' ')
        n--;
    line[n] = 0;
}

static void cmd_help(void)
{
    kprintf("  ls               list files\n");
    kprintf("  cat FILE         print a file\n");
    kprintf("  write FILE TEXT  create or overwrite a file (8.3 name)\n");
    kprintf("  rm FILE          delete a file\n");
    kprintf("  df               free disk space\n");
    kprintf("  echo TEXT        print text\n");
    kprintf("  mem              heap usage\n");
    kprintf("  uptime           time since the timer started\n");
    kprintf("  clear, reboot, halt\n");
}

static void run(char *cmd)
{
    char *arg = cmd;
    while (*arg && *arg != ' ')
        arg++;
    if (*arg) {
        *arg++ = 0;
        while (*arg == ' ')
            arg++;
    }
    if (cmd[0] == 0)
        return;

    if (!strcmp(cmd, "help")) {
        cmd_help();
    } else if (!strcmp(cmd, "ls")) {
        fat_ls();
    } else if (!strcmp(cmd, "cat")) {
        if (!*arg)
            kprintf("usage: cat FILE\n");
        else if (fat_cat(arg) == 0)
            kputc('\n');
    } else if (!strcmp(cmd, "write")) {
        char *name = arg;
        while (*arg && *arg != ' ')
            arg++;
        if (*arg) {
            *arg++ = 0;
            while (*arg == ' ')
                arg++;
        }
        if (!*name)
            kprintf("usage: write FILE TEXT\n");
        else if (fat_write(name, arg, strlen(arg)) == 0)
            kprintf("wrote %s (%u bytes)\n", name, strlen(arg));
    } else if (!strcmp(cmd, "rm")) {
        if (!*arg)
            kprintf("usage: rm FILE\n");
        else if (fat_delete(arg) == 0)
            kprintf("deleted %s\n", arg);
    } else if (!strcmp(cmd, "df")) {
        fat_df();
    } else if (!strcmp(cmd, "echo")) {
        kprintf("%s\n", arg);
    } else if (!strcmp(cmd, "mem")) {
        u32 used, free_bytes, largest;
        heap_stats(&used, &free_bytes, &largest);
        kprintf("heap: %u bytes used, %u bytes free, largest free block %u\n",
                used, free_bytes, largest);
    } else if (!strcmp(cmd, "uptime")) {
        u32 ms = uptime_ms();
        kprintf("up %u.%02u s\n", ms / 1000, (ms % 1000) / 10);
    } else if (!strcmp(cmd, "clear")) {
        console_clear();
    } else if (!strcmp(cmd, "reboot")) {
        kprintf("rebooting\n");
        outb(0x64, 0xFE);          /* keyboard controller: pulse the reset line */
    } else if (!strcmp(cmd, "halt")) {
        kprintf("halted\n");
        outb(0xF4, 0x10);          /* QEMU's debug-exit port; ignored elsewhere */
        cpu_cli();
        for (;;)
            cpu_hlt();
    } else if (!strcmp(cmd, "crash")) {
        /* Deliberate divide-by-zero to exercise the exception path. Both operands
           are volatile so the compiler cannot fold the division away (it treats
           x/0 as undefined and would otherwise reduce 1/x to x==1). */
        volatile u32 one = 1, zero = 0;
        kprintf("%u\n", one / zero);
    } else {
        kprintf("unknown command: %s (type help)\n", cmd);
    }
}

void shell_run(void)
{
    kprintf("type 'help' for commands\n");
    for (;;) {
        kprintf("myos> ");
        read_line();
        run(line);
    }
}
