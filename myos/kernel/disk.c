/* kernel/disk.c - ATA PIO driver and a FAT16 filesystem (root directory only). */
#include "kernel.h"

/* ---- ATA PIO, primary IDE channel, master drive ---- */
#define ATA_DATA   0x1F0
#define ATA_SECCNT 0x1F2
#define ATA_LBA0   0x1F3
#define ATA_LBA1   0x1F4
#define ATA_LBA2   0x1F5
#define ATA_DRIVE  0x1F6
#define ATA_STATUS 0x1F7
#define ATA_CMD    0x1F7

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

#define SECTOR        512u
#define ENTRY_DELETED 0xE5
#define ATTR_LFN      0x0F
#define ATTR_VOLUME   0x08
#define ATTR_ARCHIVE  0x20

static u32 part_lba, fat_lba, root_lba, data_lba;
static u32 fat_sectors, root_sectors, root_entries, num_fats;
static u32 spc, nclusters;
static u8 *fat;                    /* the FAT, kept in RAM */
static u8 *fat_dirty;              /* one flag per FAT sector */
static int mounted;

static u32 rd32(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static int ata_wait_idle(void)
{
    for (u32 i = 0; i < 2000000u; i++)
        if (!(inb(ATA_STATUS) & ST_BSY))
            return 0;
    return -1;
}

static int ata_wait_data(void)
{
    for (u32 i = 0; i < 2000000u; i++) {
        u8 s = inb(ATA_STATUS);
        if (s & (ST_ERR | ST_DF))
            return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ))
            return 0;
    }
    return -1;
}

static int ata_begin(u32 lba, u8 cmd)
{
    if (ata_wait_idle())
        return -1;
    outb(ATA_DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F)));   /* LBA mode, master */
    outb(ATA_SECCNT, 1);
    outb(ATA_LBA0, (u8)lba);
    outb(ATA_LBA1, (u8)(lba >> 8));
    outb(ATA_LBA2, (u8)(lba >> 16));
    outb(ATA_CMD, cmd);
    return 0;
}

static int ata_read(u32 lba, void *buf)
{
    if (ata_begin(lba, 0x20) || ata_wait_data())
        return -1;
    u16 *p = buf;
    for (int i = 0; i < 256; i++)
        p[i] = inw(ATA_DATA);
    return 0;
}

static int ata_write(u32 lba, const void *buf)
{
    if (ata_begin(lba, 0x30) || ata_wait_data())
        return -1;
    const u16 *p = buf;
    for (int i = 0; i < 256; i++)
        outw(ATA_DATA, p[i]);
    outb(ATA_CMD, 0xE7);           /* flush the drive's write cache */
    if (ata_wait_idle())
        return -1;
    return (inb(ATA_STATUS) & (ST_ERR | ST_DF)) ? -1 : 0;
}

/* ---- mount ---- */

static int valid_83(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '~';
}

int disk_init(void)
{
    u8 sec[SECTOR];
    mounted = 0;

    if (ata_read(0, sec))
        return -1;
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return -1;
    const u8 *pe = sec + 0x1BE;                 /* first partition entry */
    if (pe[4] != 0x04 && pe[4] != 0x06 && pe[4] != 0x0E)
        return -1;
    part_lba = rd32(pe + 8);

    if (ata_read(part_lba, sec))
        return -1;
    if (rd16(sec + 0x0B) != 512 || sec[0x0D] == 0)
        return -1;
    spc = sec[0x0D];
    u32 reserved = rd16(sec + 0x0E);
    num_fats = sec[0x10];
    root_entries = rd16(sec + 0x11);
    fat_sectors = rd16(sec + 0x16);
    u32 total = rd16(sec + 0x13);
    if (!total)
        total = rd32(sec + 0x20);
    if (num_fats == 0 || fat_sectors == 0)
        return -1;
    if (memcmp(sec + 0x36, "FAT16", 5) != 0)
        return -1;
    /* Only touch a volume that carries our label, so a stranger's disk is
       never written to by accident. */
    if (memcmp(sec + 0x2B, "MYOS       ", 11) != 0)
        return -1;

    fat_lba = part_lba + reserved;
    root_lba = fat_lba + num_fats * fat_sectors;
    root_sectors = (root_entries * 32 + SECTOR - 1) / SECTOR;
    data_lba = root_lba + root_sectors;
    nclusters = (total - (data_lba - part_lba)) / spc;

    fat = kmalloc(fat_sectors * SECTOR);
    fat_dirty = kmalloc(fat_sectors);
    if (!fat || !fat_dirty)
        return -1;
    for (u32 s = 0; s < fat_sectors; s++)
        if (ata_read(fat_lba + s, fat + s * SECTOR))
            return -1;
    memset(fat_dirty, 0, fat_sectors);
    mounted = 1;
    return 0;
}

static int need_mount(void)
{
    if (!mounted)
        kprintf("no filesystem mounted\n");
    return mounted ? 0 : -1;
}

/* ---- FAT table (kept in RAM, written back on flush) ---- */

static int cluster_ok(u32 c)
{
    return c >= 2 && c < nclusters + 2;
}

static u32 cluster_lba(u32 c)
{
    return data_lba + (c - 2) * spc;
}

static u16 fat_get(u32 c)
{
    if (c * 2 + 1 >= fat_sectors * SECTOR)
        return 0xFFFF;
    return rd16(fat + c * 2);
}

static void fat_set(u32 c, u16 v)
{
    if (c * 2 + 1 >= fat_sectors * SECTOR)
        return;
    fat[c * 2] = (u8)v;
    fat[c * 2 + 1] = (u8)(v >> 8);
    fat_dirty[(c * 2) / SECTOR] = 1;
}

/* Writes every dirty FAT sector to all FAT copies. */
static int fat_flush(void)
{
    for (u32 s = 0; s < fat_sectors; s++) {
        if (!fat_dirty[s])
            continue;
        for (u32 copy = 0; copy < num_fats; copy++)
            if (ata_write(fat_lba + copy * fat_sectors + s, fat + s * SECTOR))
                return -1;
        fat_dirty[s] = 0;
    }
    return 0;
}

static void free_chain(u32 c)
{
    u32 hops = 0;
    while (cluster_ok(c) && hops++ < nclusters) {
        u16 next = fat_get(c);
        fat_set(c, 0);
        c = next;
    }
}

static int alloc_clusters(u32 count, u16 *chain)
{
    u32 got = 0;
    for (u32 c = 2; c < nclusters + 2 && got < count; c++)
        if (fat_get(c) == 0)
            chain[got++] = (u16)c;
    if (got < count)
        return -1;
    for (u32 i = 0; i < count; i++)
        fat_set(chain[i], (i + 1 < count) ? chain[i + 1] : 0xFFFF);
    return 0;
}

/* ---- names and the root directory ---- */

/* "name.ext" -> 11-byte FAT directory form, upper case, space padded. */
static int name_to_83(const char *in, char out[11])
{
    memset(out, ' ', 11);
    u32 n = 0;
    while (*in && *in != '.') {
        char c = *in++;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if (!valid_83(c) || n >= 8)
            return -1;
        out[n++] = c;
    }
    if (n == 0)
        return -1;
    if (*in == '.') {
        in++;
        u32 e = 8;
        while (*in) {
            char c = *in++;
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            if (!valid_83(c) || e >= 11)
                return -1;
            out[e++] = c;
        }
    }
    return 0;
}

static void name_from_83(const u8 *e, char *out)
{
    u32 n = 0;
    for (u32 i = 0; i < 8 && e[i] != ' '; i++)
        out[n++] = (char)e[i];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (u32 i = 8; i < 11 && e[i] != ' '; i++)
            out[n++] = (char)e[i];
    }
    out[n] = 0;
}

static int is_file_entry(const u8 *e)
{
    return e[0] != 0x00 && e[0] != ENTRY_DELETED &&
           e[11] != ATTR_LFN && !(e[11] & ATTR_VOLUME);
}

/* Returns 0 and fills the outputs if name83 exists, 1 if not, -1 on error. */
static int root_find(const char *name83, u32 *sector, u32 *slot, u8 *out)
{
    u8 buf[SECTOR];
    for (u32 s = 0; s < root_sectors; s++) {
        if (ata_read(root_lba + s, buf))
            return -1;
        for (u32 i = 0; i < 16; i++) {
            if (s * 16 + i >= root_entries || buf[i * 32] == 0x00)
                return 1;
            const u8 *e = buf + i * 32;
            if (!is_file_entry(e))
                continue;
            if (memcmp(e, name83, 11) == 0) {
                if (sector)
                    *sector = root_lba + s;
                if (slot)
                    *slot = i;
                if (out)
                    memcpy(out, e, 32);
                return 0;
            }
        }
    }
    return 1;
}

/* Finds an unused directory slot. Returns 0 on success, 1 if full, -1 on error. */
static int find_free_slot(u32 *sector, u32 *slot)
{
    u8 buf[SECTOR];
    for (u32 s = 0; s < root_sectors; s++) {
        if (ata_read(root_lba + s, buf))
            return -1;
        for (u32 i = 0; i < 16; i++) {
            if (s * 16 + i >= root_entries)
                return 1;
            u8 first = buf[i * 32];
            if (first == 0x00 || first == ENTRY_DELETED) {
                *sector = root_lba + s;
                *slot = i;
                return 0;
            }
        }
    }
    return 1;
}

/* Removes name83 from the directory and frees its clusters in RAM.
   Returns 0 if removed, 1 if it did not exist, -1 on error. */
static int dir_remove(const char *name83)
{
    u32 sector, slot;
    u8 ent[32];
    int r = root_find(name83, &sector, &slot, ent);
    if (r != 0)
        return r;
    free_chain(rd16(ent + 26));
    u8 buf[SECTOR];
    if (ata_read(sector, buf))
        return -1;
    buf[slot * 32] = ENTRY_DELETED;
    return ata_write(sector, buf) ? -1 : 0;
}

/* ---- public API ---- */

void fat_ls(void)
{
    if (need_mount())
        return;
    u8 buf[SECTOR];
    u32 files = 0, bytes = 0;
    for (u32 s = 0; s < root_sectors; s++) {
        if (ata_read(root_lba + s, buf)) {
            kprintf("disk read error\n");
            return;
        }
        for (u32 i = 0; i < 16; i++) {
            if (s * 16 + i >= root_entries || buf[i * 32] == 0x00)
                goto summary;
            const u8 *e = buf + i * 32;
            if (!is_file_entry(e))
                continue;
            char name[16];
            name_from_83(e, name);
            u32 size = rd32(e + 28);
            kprintf("  %s  %u bytes\n", name, size);
            files++;
            bytes += size;
        }
    }
summary:
    kprintf("%u file(s), %u bytes\n", files, bytes);
}

/* Return the zero-based root-directory file at index. */
int fat_entry_get(u32 index, char *name, u32 *size)
{
    if (need_mount())
        return -1;
    u8 buf[SECTOR];
    u32 seen = 0;
    for (u32 s = 0; s < root_sectors; s++) {
        if (ata_read(root_lba + s, buf))
            return -1;
        for (u32 i = 0; i < 16; i++) {
            if (s * 16 + i >= root_entries || buf[i * 32] == 0x00)
                return 0;
            const u8 *e = buf + i * 32;
            if (!is_file_entry(e))
                continue;
            if (seen++ == index) {
                name_from_83(e, name);
                if (size)
                    *size = rd32(e + 28);
                return 1;
            }
        }
    }
    return 0;
}

int fat_cat(const char *name)
{
    if (need_mount())
        return -1;
    char n83[11];
    if (name_to_83(name, n83)) {
        kprintf("bad name (use 8.3 names such as NOTE.TXT)\n");
        return -1;
    }
    u8 ent[32];
    int r = root_find(n83, 0, 0, ent);
    if (r < 0) {
        kprintf("disk read error\n");
        return -1;
    }
    if (r > 0) {
        kprintf("no such file: %s\n", name);
        return -1;
    }
    u32 left = rd32(ent + 28);
    u32 cl = rd16(ent + 26);
    u8 buf[SECTOR];
    u32 hops = 0;
    while (left && cluster_ok(cl) && hops++ < nclusters) {
        for (u32 s = 0; s < spc && left; s++) {
            if (ata_read(cluster_lba(cl) + s, buf)) {
                kprintf("\ndisk read error\n");
                return -1;
            }
            for (u32 i = 0; i < SECTOR && left; i++, left--)
                kputc((char)buf[i]);
        }
        cl = fat_get(cl);
    }
    return 0;
}

/* Creates or overwrites a file (at most 4096 bytes). The update is ordered so
   that a failure or a power cut at any step leaves either the old file or the
   new one intact: the new clusters are allocated while the old ones stay
   reserved, the directory entry is switched only once the data is on disk,
   and the old chain is released last. */
int fat_write(const char *name, const char *data, u32 len)
{
    if (need_mount())
        return -1;
    char n83[11];
    if (name_to_83(name, n83)) {
        kprintf("bad name (use 8.3 names such as NOTE.TXT)\n");
        return -1;
    }
    if (len > 4096) {
        kprintf("too large: 4096 bytes at most\n");
        return -1;
    }
    u32 csize = SECTOR * spc;
    u32 need = (len + csize - 1) / csize;
    u16 chain[16];

    /* 1. Is there an old copy, and where does its directory entry live? */
    u32 dsec = 0, dslot = 0;
    u8 old[32];
    int r = root_find(n83, &dsec, &dslot, old);
    if (r < 0) {
        kprintf("disk error\n");
        return -1;
    }
    int exists = (r == 0);
    u16 old_chain = exists ? rd16(old + 26) : 0;

    /* 2. Where does the new entry go? Checked before anything changes. */
    if (!exists) {
        r = find_free_slot(&dsec, &dslot);
        if (r < 0) {
            kprintf("disk error\n");
            return -1;
        }
        if (r > 0) {
            kprintf("root directory is full\n");
            return -1;
        }
    }

    /* 3. Allocate and write the new clusters; the old ones stay reserved. */
    if (need && alloc_clusters(need, chain)) {
        kprintf("disk is full\n");
        return -1;
    }
    u8 buf[SECTOR];
    for (u32 k = 0; k * SECTOR < len; k++) {
        u32 off = k * SECTOR;
        u32 cnt = (len - off < SECTOR) ? (len - off) : SECTOR;
        memset(buf, 0, SECTOR);
        memcpy(buf, data + off, cnt);
        if (ata_write(cluster_lba(chain[k / spc]) + (k % spc), buf)) {
            if (need)
                free_chain(chain[0]);
            fat_flush();
            kprintf("disk write error (old file untouched)\n");
            return -1;
        }
    }
    if (fat_flush()) {
        kprintf("disk write error (FAT)\n");
        return -1;
    }

    /* 4. Point the directory entry at the new chain. */
    if (ata_read(dsec, buf)) {
        kprintf("disk read error\n");
        return -1;
    }
    u8 *e = buf + dslot * 32;
    memset(e, 0, 32);
    memcpy(e, n83, 11);
    e[11] = ATTR_ARCHIVE;
    if (need) {
        e[26] = (u8)chain[0];
        e[27] = (u8)(chain[0] >> 8);
    }
    e[28] = (u8)len;
    e[29] = (u8)(len >> 8);
    e[30] = (u8)(len >> 16);
    e[31] = (u8)(len >> 24);
    if (ata_write(dsec, buf)) {
        if (need)
            free_chain(chain[0]);
        fat_flush();
        kprintf("disk write error (directory, old file untouched)\n");
        return -1;
    }

    /* 5. The old chain is no longer referenced: release it and persist. */
    if (exists)
        free_chain(old_chain);
    if (fat_flush()) {
        kprintf("disk write error (FAT)\n");
        return -1;
    }
    return 0;
}

int fat_delete(const char *name)
{
    if (need_mount())
        return -1;
    char n83[11];
    if (name_to_83(name, n83)) {
        kprintf("bad name\n");
        return -1;
    }
    int r = dir_remove(n83);
    if (r > 0) {
        kprintf("no such file: %s\n", name);
        return -1;
    }
    if (r < 0 || fat_flush()) {
        kprintf("disk error\n");
        return -1;
    }
    return 0;
}

void fat_df(void)
{
    if (need_mount())
        return;
    u32 free_clusters = 0;
    for (u32 c = 2; c < nclusters + 2; c++)
        if (fat_get(c) == 0)
            free_clusters++;
    kprintf("%u of %u bytes free\n",
            free_clusters * spc * SECTOR, nclusters * spc * SECTOR);
}
