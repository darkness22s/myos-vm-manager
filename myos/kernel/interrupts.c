/* kernel/interrupts.c - IDT, PIC, PIT timer, PS/2 keyboard, and exception panic. */
#include "kernel.h"

struct idt_entry {
    u16 lo;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 hi;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

/* Stack frame built by isr_common in kernel/start.asm. */
struct regs {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags;
};

extern u32 isr_stub_table[48];
void isr_dispatch(struct regs *r);

static struct idt_entry idt[256];
static volatile u32 ticks;           /* PIT ticks since the timer started */

static const char *exc_names[32] = {
    [0]  = "divide error",
    [1]  = "debug",
    [2]  = "non-maskable interrupt",
    [3]  = "breakpoint",
    [4]  = "overflow",
    [5]  = "bound range exceeded",
    [6]  = "invalid opcode",
    [7]  = "device not available",
    [8]  = "double fault",
    [10] = "invalid TSS",
    [11] = "segment not present",
    [12] = "stack fault",
    [13] = "general protection fault",
    [14] = "page fault",
    [16] = "x87 floating-point error",
    [17] = "alignment check",
    [18] = "machine check",
    [19] = "SIMD floating-point error",
};

static void panic(const struct regs *r)
{
    const char *name = "unknown exception";
    if (r->int_no < 32 && exc_names[r->int_no])
        name = exc_names[r->int_no];
    cpu_cli();
    kprintf("\n*** KERNEL PANIC: %s (vector %u)\n", name, r->int_no);
    kprintf("error=%x eip=%x cs=%x eflags=%x eax=%x\n",
            r->err_code, r->eip, r->cs, r->eflags, r->eax);
    for (;;)
        cpu_hlt();
}

/* Remap IRQ0-15 to vectors 32-47 (the BIOS default collides with CPU
   exceptions) and unmask only the timer (IRQ0) and keyboard (IRQ1). */
void pic_init(void)
{
    outb(0x20, 0x11); io_wait();   /* ICW1: start initialisation */
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();   /* ICW2: master vectors start at 32 */
    outb(0xA1, 0x28); io_wait();   /* ICW2: slave vectors start at 40 */
    outb(0x21, 0x04); io_wait();   /* ICW3: slave is on IRQ2 */
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();   /* ICW4: 8086 mode */
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFC);              /* unmask IRQ0 and IRQ1 */
    outb(0xA1, 0xFF);
}

/* PIT channel 0 as a periodic interrupt source (base clock 1.193182 MHz). */
void pit_init(u32 hz)
{
    u32 div = 1193182u / hz;
    outb(0x43, 0x36);              /* channel 0, rate generator, 16-bit */
    outb(0x40, (u8)div);
    outb(0x40, (u8)(div >> 8));
}

u32 uptime_ms(void)
{
    return ticks * 10u;            /* 100 Hz */
}

static void idt_set(int n, u32 handler)
{
    idt[n].lo = (u16)handler;
    idt[n].hi = (u16)(handler >> 16);
    idt[n].sel = 0x08;             /* kernel code segment from the GDT */
    idt[n].zero = 0;
    idt[n].flags = 0x8E;           /* present, ring 0, 32-bit interrupt gate */
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++)
        idt_set(i, isr_stub_table[i]);
    struct idt_ptr p;
    p.limit = (u16)(sizeof idt - 1);
    p.base = (u32)idt;
    __asm__ volatile ("lidt %0" : : "m"(p));
}

/* PS/2 keyboard, scan code set 1, US layout. Letters are shifted by case. */
static const char sc_plain[128] = {
    [0x01] = 27,   [0x02] = '1',  [0x03] = '2',  [0x04] = '3',  [0x05] = '4',
    [0x06] = '5',  [0x07] = '6',  [0x08] = '7',  [0x09] = '8',  [0x0A] = '9',
    [0x0B] = '0',  [0x0C] = '-',  [0x0D] = '=',  [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e',  [0x13] = 'r',  [0x14] = 't',
    [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',  [0x18] = 'o',  [0x19] = 'p',
    [0x1A] = '[',  [0x1B] = ']',  [0x1C] = '\n',
    [0x1E] = 'a',  [0x1F] = 's',  [0x20] = 'd',  [0x21] = 'f',  [0x22] = 'g',
    [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',  [0x26] = 'l',  [0x27] = ';',
    [0x28] = '\'', [0x29] = '`',  [0x2B] = '\\',
    [0x2C] = 'z',  [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',  [0x30] = 'b',
    [0x31] = 'n',  [0x32] = 'm',  [0x33] = ',',  [0x34] = '.',  [0x35] = '/',
    [0x37] = '*',  [0x39] = ' ',
};

static const char sc_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x1A] = '{', [0x1B] = '}', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2B] = '|', [0x33] = '<', [0x34] = '>',
    [0x35] = '?', [0x37] = '*', [0x39] = ' ',
};

static volatile int shift_down;
static char kbuf[128];
static volatile u32 kb_head, kb_tail;

static void kb_put(char c)
{
    u32 next = (kb_head + 1) % sizeof kbuf;
    if (next != kb_tail) {         /* drop the key if the buffer is full */
        kbuf[kb_head] = c;
        kb_head = next;
    }
}

int kb_pop(void)
{
    int c = -1;
    cpu_cli();
    if (kb_tail != kb_head) {
        c = (u8)kbuf[kb_tail];
        kb_tail = (kb_tail + 1) % sizeof kbuf;
    }
    cpu_sti();
    return c;
}

static void keyboard_irq(void)
{
    u8 sc = inb(0x60);
    if (sc == 0x2A || sc == 0x36) { shift_down = 1; return; }   /* shift down */
    if (sc == 0xAA || sc == 0xB6) { shift_down = 0; return; }   /* shift up */
    if (sc & 0x80)                 return;                      /* other key-ups */
    char c = (shift_down && sc_shift[sc]) ? sc_shift[sc] : sc_plain[sc];
    if (shift_down && c >= 'a' && c <= 'z')
        c = (char)(c - 32);
    if (c)
        kb_put(c);
}

/* Called by isr_common for every interrupt and exception. */
void isr_dispatch(struct regs *r)
{
    if (r->int_no < 32)
        panic(r);
    if (r->int_no >= 32 && r->int_no < 48) {
        u32 irq = r->int_no - 32;
        if (irq == 0)
            ticks++;
        else if (irq == 1)
            keyboard_irq();
        if (irq >= 8)
            outb(0xA0, 0x20);      /* end-of-interrupt to the slave PIC */
        outb(0x20, 0x20);          /* end-of-interrupt to the master PIC */
    }
}
