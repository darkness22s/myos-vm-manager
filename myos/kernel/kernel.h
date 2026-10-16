/* kernel/kernel.h - shared types, port I/O, and prototypes for MyOS. */
#ifndef MYOS_KERNEL_H
#define MYOS_KERNEL_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            i32;

#define NULL ((void *)0)

static inline void outb(u16 port, u8 v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(u16 port, u16 v)
{
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline u16 inw(u16 port)
{
    u16 v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void io_wait(void) { outb(0x80, 0); }
static inline void cpu_sti(void) { __asm__ volatile ("sti"); }
static inline void cpu_cli(void) { __asm__ volatile ("cli"); }
static inline void cpu_hlt(void) { __asm__ volatile ("hlt"); }

/* console.c */
void console_clear(void);
void serial_init(void);
int  serial_rx_ready(void);
int  serial_rx(void);
void kputc(char c);
void kprintf(const char *fmt, ...);

/* interrupts.c */
void idt_init(void);
void pic_init(void);
void pit_init(u32 hz);
u32  uptime_ms(void);
int  kb_pop(void);

/* lib.c */
void *memcpy(void *dst, const void *src, u32 n);
void *memset(void *dst, int c, u32 n);
int   memcmp(const void *a, const void *b, u32 n);
u32   strlen(const char *s);
int   strcmp(const char *a, const char *b);
void  heap_init(void *start, u32 size);
void *kmalloc(u32 n);
void  kfree(void *p);
void  heap_stats(u32 *used, u32 *free_bytes, u32 *largest_free);

/* disk.c */
int  disk_init(void);
void fat_ls(void);
int  fat_cat(const char *name);
int  fat_write(const char *name, const char *data, u32 len);
int  fat_delete(const char *name);
void fat_df(void);

/* shell.c */
void shell_run(void);

#endif
