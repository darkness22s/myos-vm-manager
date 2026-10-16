/* kernel/console.c - VGA text output, COM1 serial port, and kprintf. */
#include "kernel.h"

#define VGA_MEM  ((volatile u16 *)0xB8000)
#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_ATTR 0x07                 /* light grey on black */
#define COM1     0x3F8

static int cur_col, cur_row;
static int serial_up;

static u16 blank_cell(void)
{
    return (u16)(' ' | (VGA_ATTR << 8));
}

static void vga_move_cursor(void)
{
    u16 pos = (u16)(cur_row * VGA_COLS + cur_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)(pos >> 8));
}

static void vga_scroll(void)
{
    for (int r = 1; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            VGA_MEM[(r - 1) * VGA_COLS + c] = VGA_MEM[r * VGA_COLS + c];
    for (int c = 0; c < VGA_COLS; c++)
        VGA_MEM[(VGA_ROWS - 1) * VGA_COLS + c] = blank_cell();
    cur_row = VGA_ROWS - 1;
}

void console_clear(void)
{
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_MEM[i] = blank_cell();
    cur_col = 0;
    cur_row = 0;
    vga_move_cursor();
}

static void vga_putc(char c)
{
    if (c == '\n') {
        cur_col = 0;
        cur_row++;
    } else if (c == '\r') {
        cur_col = 0;
    } else if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            VGA_MEM[cur_row * VGA_COLS + cur_col] = blank_cell();
        }
    } else if (c == '\t') {
        int n = 4 - (cur_col % 4);
        while (n-- > 0)
            vga_putc(' ');
        return;
    } else {
        VGA_MEM[cur_row * VGA_COLS + cur_col] = (u16)((u8)c | (VGA_ATTR << 8));
        if (++cur_col == VGA_COLS) {
            cur_col = 0;
            cur_row++;
        }
    }
    if (cur_row >= VGA_ROWS)
        vga_scroll();
    vga_move_cursor();
}

void serial_init(void)
{
    outb(COM1 + 1, 0x00);         /* no UART interrupts; we poll instead */
    outb(COM1 + 3, 0x80);         /* DLAB on: set the baud divisor */
    outb(COM1 + 0, 0x03);         /* divisor 3 = 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);         /* 8 data bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);         /* enable and clear the FIFOs */
    outb(COM1 + 4, 0x0B);         /* DTR, RTS, OUT2 */
    serial_up = (inb(COM1 + 5) != 0xFF);   /* 0xFF means no UART is there */
}

static void serial_tx(char c)
{
    if (!serial_up)
        return;
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
        ;
    outb(COM1, (u8)c);
}

int serial_rx_ready(void)
{
    return serial_up && (inb(COM1 + 5) & 0x01);
}

int serial_rx(void)
{
    return inb(COM1);
}

void kputc(char c)
{
    vga_putc(c);
    if (c == '\n')
        serial_tx('\r');
    serial_tx(c);
}

static void put_unsigned(u32 v, u32 base, int width, char pad)
{
    static const char digits[] = "0123456789abcdef";
    char buf[32];
    int n = 0;
    do {
        buf[n++] = digits[v % base];
        v /= base;
    } while (v);
    while (n < width)
        buf[n++] = pad;
    while (n > 0)
        kputc(buf[--n]);
}

/* Supports %c %s %d %u %x %% with an optional 0 flag and width. */
void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            kputc(*fmt++);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        char spec = *fmt;
        if (!spec)
            break;
        fmt++;
        switch (spec) {
        case 'c':
            kputc((char)__builtin_va_arg(ap, int));
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s)
                kputc(*s++);
            break;
        }
        case 'd': {
            i32 v = __builtin_va_arg(ap, i32);
            if (v < 0) {
                kputc('-');
                put_unsigned((u32)0 - (u32)v, 10, width, pad);
            } else {
                put_unsigned((u32)v, 10, width, pad);
            }
            break;
        }
        case 'u':
            put_unsigned(__builtin_va_arg(ap, u32), 10, width, pad);
            break;
        case 'x':
            put_unsigned(__builtin_va_arg(ap, u32), 16, width, pad);
            break;
        case '%':
            kputc('%');
            break;
        default:
            kputc('%');
            kputc(spec);
            break;
        }
    }
    __builtin_va_end(ap);
}
