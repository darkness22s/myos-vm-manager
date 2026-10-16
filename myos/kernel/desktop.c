/* kernel/desktop.c - compact mode-13h desktop, file browser, and shell window. */
#include "kernel.h"

#define FB ((volatile u8 *)0xA0000)
#define W 320
#define H 200
#define TASK_Y 182
#define BLACK 0
#define NAVY 1
#define BLUE 9
#define CYAN 11
#define WHITE 15
#define GREY 8
#define LIGHT 7
#define GREEN 10
#define RED 12
#define YELLOW 14

enum app { APP_DESKTOP, APP_FILES, APP_SYSTEM, APP_SETTINGS, APP_TERMINAL };
static enum app current = APP_DESKTOP;
static int terminal_active;
static int mx = 160, my = 90, left_down;
static u8 packet[3];
static u32 packet_n;
static u32 last_clock;
static int accent = CYAN;
static int cursor_x, cursor_y;

static void rect(int x, int y, int w, int h, u8 color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++) {
        volatile u8 *p = FB + row * W + x;
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

static void text(int x, int y, const char *s, u8 color)
{
    /* The BIOS 8x8 ROM font remains mapped at F000:FA6E in protected mode. */
    volatile const u8 *font = (volatile const u8 *)0xFFA6E;
    while (*s && x <= W - 8) {
        u8 ch = (u8)*s++;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        for (int row = 0; row < 8; row++) {
            u8 bits = font[ch * 8 + row];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col))
                    FB[(y + row) * W + x + col] = color;
        }
        x += 8;
    }
}

static void center_text(int x, int y, int width, const char *s, u8 color)
{
    int n = (int)strlen(s);
    text(x + (width - n * 8) / 2, y, s, color);
}

static void button(int x, int y, int w, const char *label, int selected)
{
    rect(x, y, w, 20, selected ? accent : 8);
    rect(x + 1, y + 1, w - 2, 18, selected ? BLUE : 7);
    center_text(x + 1, y + 6, w - 2, label, WHITE);
}

static void window_frame(const char *title)
{
    rect(28, 17, 264, 158, BLACK);
    rect(30, 19, 260, 154, 7);
    rect(31, 20, 258, 18, accent);
    text(37, 25, title, WHITE);
    rect(279, 22, 8, 12, RED);
    text(280, 24, "X", WHITE);
    rect(33, 40, 256, 130, 15);
}

static void taskbar(void)
{
    rect(0, TASK_Y, W, 18, 8);
    rect(0, TASK_Y, W, 1, 15);
    button(2, TASK_Y + 2, 42, "MYOS", current == APP_DESKTOP);
    button(47, TASK_Y + 2, 49, "TERM", current == APP_TERMINAL);
    button(99, TASK_Y + 2, 48, "FILES", current == APP_FILES);
    button(150, TASK_Y + 2, 48, "INFO", current == APP_SYSTEM);
    button(201, TASK_Y + 2, 51, "SETUP", current == APP_SETTINGS);
    u32 secs = uptime_ms() / 1000;
    char clock[9] = { (char)('0' + (secs / 36000) % 10),
        (char)('0' + (secs / 3600) % 10), ':',
        (char)('0' + (secs / 600) % 6), (char)('0' + (secs / 60) % 10), ':',
        (char)('0' + (secs / 10) % 6), (char)('0' + secs % 10), 0 };
    text(260, TASK_Y + 6, clock, WHITE);
}

static void desktop_draw(void)
{
    rect(0, 0, W, TASK_Y, NAVY);
    rect(0, 0, W, 17, 1);
    text(8, 5, "MYOS DESKTOP", WHITE);
    text(222, 5, "32-BIT  |  16 MB", LIGHT);

    /* Native desktop shortcuts: each icon opens the corresponding working app. */
    rect(22, 34, 34, 30, 14); rect(25, 37, 28, 24, YELLOW);
    rect(28, 40, 9, 2, WHITE); rect(28, 45, 22, 2, WHITE);
    rect(28, 50, 18, 2, WHITE);
    text(12, 67, "FILES", WHITE);

    rect(86, 34, 34, 30, 8); rect(89, 37, 28, 24, BLACK);
    text(93, 44, ">_", GREEN); text(81, 67, "TERMINAL", WHITE);

    rect(150, 34, 34, 30, 6); rect(153, 37, 28, 24, 15);
    rect(158, 42, 18, 4, accent); rect(158, 49, 14, 3, 8);
    text(143, 67, "SYSTEM INFO", WHITE);

    rect(214, 34, 34, 30, 5); rect(217, 37, 28, 24, 15);
    rect(222, 43, 18, 3, accent); rect(222, 50, 12, 3, 8);
    text(211, 67, "SETTINGS", WHITE);

    rect(12, 103, 296, 61, 1);
    rect(13, 104, 294, 59, 9);
    text(22, 113, "WELCOME TO MYOS", WHITE);
    text(22, 128, "A SMALL, REAL x86 OPERATING SYSTEM", 14);
    text(22, 145, "HOTKEYS: 1 FILES  2 TERMINAL  3 INFO  4 SETTINGS", 11);
    taskbar();
}

static void files_draw(void)
{
    window_frame("FILE BROWSER  -  A:/");
    text(42, 47, "NAME", GREY);
    text(226, 47, "SIZE", GREY);
    rect(40, 58, 238, 1, 8);
    char name[16];
    for (u32 i = 0; i < 12; i++) {
        u32 size = 0;
        int r = fat_entry_get(i, name, &size);
        if (r <= 0) {
            if (r < 0) text(43, 67, "DISK NOT AVAILABLE", RED);
            else if (i == 0) text(43, 67, "NO FILES YET", GREY);
            break;
        }
        char nbuf[12];
        u32 j = 0;
        while (name[j] && j < 11) { nbuf[j] = name[j]; j++; }
        nbuf[j] = 0;
        text(44, 64 + (int)i * 8, nbuf, i % 2 ? 8 : 1);
        char digits[11];
        int d = 0;
        do { digits[d++] = (char)('0' + size % 10); size /= 10; } while (size && d < 10);
        for (int k = 0; k < d / 2; k++) { char t = digits[k]; digits[k] = digits[d-1-k]; digits[d-1-k] = t; }
        digits[d] = 0;
        text(230, 64 + (int)i * 8, digits, 8);
    }
    text(42, 155, "FAT16 ROOT  |  CLICK TASKBAR TO SWITCH APPS", GREY);
    taskbar();
}

static void number_text(int x, int y, u32 value, u8 color)
{
    char digits[11];
    int n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value && n < 10);
    for (int i = 0; i < n / 2; i++) {
        char t = digits[i]; digits[i] = digits[n - 1 - i]; digits[n - 1 - i] = t;
    }
    digits[n] = 0;
    text(x, y, digits, color);
}

static void system_draw(void)
{
    window_frame("SYSTEM MONITOR");
    u32 used, free_bytes, largest;
    heap_stats(&used, &free_bytes, &largest);
    text(44, 49, "MYOS KERNEL", accent);
    text(44, 64, "ARCHITECTURE: 32-BIT x86", 1);
    text(44, 78, "MEMORY: 16 MB MINIMUM", 1);
    text(44, 92, "KERNEL HEAP", accent);
    text(44, 105, "USED:", 8); number_text(92, 105, used, 1);
    text(44, 118, "FREE BYTES:", 8); number_text(140, 118, free_bytes, 1);
    text(44, 133, "UPTIME (SECONDS):", 8);
    number_text(180, 133, uptime_ms() / 1000, 1);
    (void)largest;
    taskbar();
}

static void settings_draw(void)
{
    window_frame("DESKTOP SETTINGS");
    text(44, 51, "ACCENT COLOR", 1);
    button(44, 67, 44, "CYAN", accent == CYAN);
    button(94, 67, 44, "GREEN", accent == GREEN);
    button(144, 67, 44, "GOLD", accent == YELLOW);
    text(44, 104, "CLICK A COLOR TO APPLY IT", GREY);
    text(44, 124, "LOW-RES VGA MODE 13H", 8);
    text(44, 139, "FRAMEBUFFER USE: 64 KB", 8);
    taskbar();
}

static void draw(void)
{
    if (current == APP_DESKTOP) desktop_draw();
    else if (current == APP_FILES) files_draw();
    else if (current == APP_SYSTEM) system_draw();
    else if (current == APP_SETTINGS) settings_draw();
}

static void open_app(enum app app)
{
    current = app;
    terminal_active = app == APP_TERMINAL;
    if (terminal_active) {
        rect(0, 0, W, TASK_Y, BLACK);
        text(8, 5, "MYOS TERMINAL  -  TYPE EXIT TO RETURN TO DESKTOP", GREEN);
        rect(0, 17, W, TASK_Y - 17, BLACK);
        cursor_x = 0; cursor_y = 22;
        taskbar();
        shell_run();
        terminal_active = 0;
        current = APP_DESKTOP;
    }
    draw();
}

static void mouse_init(void)
{
    /* Enable the PS/2 auxiliary port and switch the mouse to streaming mode. */
    for (u32 i = 0; i < 100000 && (inb(0x64) & 2); i++) { }
    outb(0x64, 0xA8);
    for (u32 i = 0; i < 100000 && (inb(0x64) & 2); i++) { }
    outb(0x64, 0xD4); outb(0x60, 0xF4);
    for (u32 i = 0; i < 100000; i++) {
        if (inb(0x64) & 1) { (void)inb(0x60); break; }
    }
}

static int mouse_poll(int *pressed)
{
    int moved = 0;
    *pressed = 0;
    while ((inb(0x64) & 1) && (inb(0x64) & 0x20)) {
        u8 b = inb(0x60);
        packet[packet_n++] = b;
        if (packet_n == 3) {
            packet_n = 0;
            int dx = (signed char)packet[1];
            int dy = (signed char)packet[2];
            int nx = mx + dx;
            int ny = my - dy;
            if (nx < 0) nx = 0;
            if (nx >= W) nx = W - 1;
            if (ny < 0) ny = 0;
            if (ny >= H) ny = H - 1;
            if (nx != mx || ny != my) { mx = nx; my = ny; moved = 1; }
            if ((packet[0] & 1) && !left_down) *pressed = 1;
            left_down = packet[0] & 1;
        }
    }
    return moved;
}

static void pointer_draw(void)
{
    /* Small high-contrast pointer, redrawn after the retained desktop view. */
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j <= i / 2; j++) {
            int x = mx + j, y = my + i;
            if (x < W && y < H) FB[y * W + x] = WHITE;
        }
    }
}

static void click(int x, int y)
{
    if (y >= TASK_Y) {
        if (x < 45) { current = APP_DESKTOP; draw(); }
        else if (x < 98) open_app(APP_TERMINAL);
        else if (x < 149) open_app(APP_FILES);
        else if (x < 200) open_app(APP_SYSTEM);
        else if (x < 254) open_app(APP_SETTINGS);
        return;
    }
    if (current == APP_DESKTOP && y >= 30 && y < 81) {
        if (x < 70) open_app(APP_FILES);
        else if (x < 135) open_app(APP_TERMINAL);
        else if (x < 200) open_app(APP_SYSTEM);
        else open_app(APP_SETTINGS);
    } else if (current == APP_SETTINGS && y >= 65 && y < 90) {
        if (x >= 44 && x < 90) accent = CYAN;
        else if (x >= 94 && x < 140) accent = GREEN;
        else if (x >= 144 && x < 190) accent = YELLOW;
        draw();
    } else if (current != APP_DESKTOP && x >= 275 && y < 38) {
        current = APP_DESKTOP; draw();
    }
}

int desktop_console_active(void)
{
    return terminal_active;
}

void desktop_console_clear(void)
{
    rect(0, 20, W, TASK_Y - 20, BLACK);
    cursor_x = 0;
    cursor_y = 22;
}

void desktop_console_putc(char c)
{
    if (c == '\r') return;
    if (c == '\n') { cursor_x = 0; cursor_y += 8; }
    else if (c == '\b') { if (cursor_x >= 8) cursor_x -= 8; rect(cursor_x, cursor_y, 8, 8, BLACK); }
    else {
        char s[2] = { c, 0 };
        if (cursor_x >= W - 8) { cursor_x = 0; cursor_y += 8; }
        if (cursor_y >= TASK_Y - 8) {
            for (int y = 22; y < TASK_Y - 8; y++)
                for (int x = 0; x < W; x++) FB[y * W + x] = FB[(y + 8) * W + x];
            rect(0, TASK_Y - 8, W, 8, BLACK);
            cursor_y = TASK_Y - 16;
        }
        text(cursor_x, cursor_y, s, WHITE);
        cursor_x += 8;
    }
    if (c == '\n') {
        if (cursor_y >= TASK_Y - 8) { cursor_y = 22; rect(0, 20, W, TASK_Y - 20, BLACK); }
        else rect(0, cursor_y, W, 8, BLACK);
    }
}

void desktop_run(void)
{
    mouse_init();
    current = APP_DESKTOP;
    desktop_draw();
    for (;;) {
        int pressed;
        int moved = mouse_poll(&pressed);
        if (pressed) click(mx, my);
        if (moved || pressed) {
            draw();
            pointer_draw();
        }
        int c = kb_pop();
        if (c >= 0) {
            if (c == '1') open_app(APP_FILES);
            else if (c == '2') open_app(APP_TERMINAL);
            else if (c == '3') open_app(APP_SYSTEM);
            else if (c == '4') open_app(APP_SETTINGS);
            else if (c == 27) { current = APP_DESKTOP; terminal_active = 0; draw(); }
        }
        u32 now = uptime_ms();
        if (now - last_clock >= 1000) { last_clock = now; draw(); pointer_draw(); }
        cpu_hlt();
    }
}
