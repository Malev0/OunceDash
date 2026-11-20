// Minimal read-only ZIP reader for the user's APK (stored + deflate).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "port.h"

struct ZipArchive {
    FILE *f;
    Mutex lock;
    // The same file opened through the filesystem service: positional reads
    // need no lock or seek, so the game's loader threads read in parallel.
    FsFile fs;
    bool has_fs;
    ZipEntry *entries;
    size_t count;
    // open-addressing hash of entry names
    uint32_t *table;
    size_t table_size;
};

ZipArchive *g_apk;

static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

size_t zip_pread(ZipArchive *z, void *buf, size_t size, uint64_t offset)
{
    if (z->has_fs) {
        u64 got = 0;
        if (R_SUCCEEDED(fsFileRead(&z->fs, (s64)offset, buf, size, FsReadOption_None, &got)))
            return (size_t)got;
    }
    mutexLock(&z->lock);
    size_t got = 0;
    if (fseeko(z->f, (off_t)offset, SEEK_SET) == 0)
        got = fread(buf, 1, size, z->f);
    mutexUnlock(&z->lock);
    return got;
}

ZipArchive *zip_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    setvbuf(f, NULL, _IOFBF, 64 * 1024);

    ZipArchive *z = calloc(1, sizeof(*z));
    z->f = f;
    mutexInit(&z->lock);
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (sd && !strncmp(path, "sdmc:/", 6)) {
        char fspath[FS_MAX_PATH];
        snprintf(fspath, sizeof(fspath), "%s", path + 5);
        z->has_fs = R_SUCCEEDED(fsFsOpenFile(sd, fspath, FsOpenMode_Read, &z->fs));
    }

    fseeko(f, 0, SEEK_END);
    uint64_t file_size = (uint64_t)ftello(f);
    size_t tail = file_size < 0x10000 + 22 ? (size_t)file_size : 0x10000 + 22;
    uint8_t *buf = malloc(tail);
    zip_pread(z, buf, tail, file_size - tail);

    // locate End Of Central Directory
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--) {
        if (rd32(buf + i) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) {
        free(buf); fclose(f); free(z);
        return NULL;
    }
    uint64_t cd_count = rd16(buf + eocd + 10);
    uint64_t cd_size = rd32(buf + eocd + 12);
    uint64_t cd_offset = rd32(buf + eocd + 16);

    // ZIP64 locator directly before the EOCD
    if (eocd >= 20 && rd32(buf + eocd - 20) == 0x07064b50) {
        uint64_t z64 = rd64(buf + eocd - 20 + 8);
        uint8_t rec[56];
        if (zip_pread(z, rec, sizeof(rec), z64) == sizeof(rec) && rd32(rec) == 0x06064b50) {
            cd_count = rd64(rec + 32);
            cd_size = rd64(rec + 40);
            cd_offset = rd64(rec + 48);
        }
    }
    free(buf);

    uint8_t *cd = malloc(cd_size);
    if (zip_pread(z, cd, cd_size, cd_offset) != cd_size) {
        free(cd); fclose(f); free(z);
        return NULL;
    }

    z->entries = calloc(cd_count, sizeof(ZipEntry));
    size_t n = 0;
    const uint8_t *p = cd, *end = cd + cd_size;
    while (p + 46 <= end && rd32(p) == 0x02014b50 && n < cd_count) {
        uint16_t method = rd16(p + 10);
        uint64_t csize = rd32(p + 20), usize = rd32(p + 24);
        uint16_t nlen = rd16(p + 28), xlen = rd16(p + 30), clen = rd16(p + 32);
        uint64_t loff = rd32(p + 42);
        // zip64 extra field
        const uint8_t *x = p + 46 + nlen, *xend = x + xlen;
        while (x + 4 <= xend) {
            uint16_t id = rd16(x), sz = rd16(x + 2);
            if (id == 1) {
                const uint8_t *q = x + 4;
                if (usize == 0xffffffff) { usize = rd64(q); q += 8; }
                if (csize == 0xffffffff) { csize = rd64(q); q += 8; }
                if (loff == 0xffffffff) { loff = rd64(q); q += 8; }
            }
            x += 4 + sz;
        }
        ZipEntry *e = &z->entries[n++];
        e->name = malloc(nlen + 1);
        memcpy(e->name, p + 46, nlen);
        e->name[nlen] = 0;
        e->method = method;
        e->comp_size = (uint32_t)csize;
        e->size = (uint32_t)usize;
        e->local_offset = loff;
        p += 46 + nlen + xlen + clen;
    }
    free(cd);
    z->count = n;

    z->table_size = 1;
    while (z->table_size < n * 2 + 1) z->table_size <<= 1;
    z->table = malloc(z->table_size * sizeof(uint32_t));
    memset(z->table, 0xff, z->table_size * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++) {
        size_t h = name_hash(z->entries[i].name) & (z->table_size - 1);
        while (z->table[h] != 0xffffffff) h = (h + 1) & (z->table_size - 1);
        z->table[h] = (uint32_t)i;
    }
    return z;
}

const ZipEntry *zip_find(ZipArchive *z, const char *name)
{
    if (!z) return NULL;
    while (name[0] == '/') name++;
    size_t h = name_hash(name) & (z->table_size - 1);
    while (z->table[h] != 0xffffffff) {
        ZipEntry *e = &z->entries[z->table[h]];
        if (strcmp(e->name, name) == 0)
            return e;
        h = (h + 1) & (z->table_size - 1);
    }
    return NULL;
}

size_t zip_count(ZipArchive *z) { return z ? z->count : 0; }
const ZipEntry *zip_entry_at(ZipArchive *z, size_t i) { return &z->entries[i]; }

uint64_t zip_data_offset(ZipArchive *z, const ZipEntry *ce)
{
    ZipEntry *e = (ZipEntry *)ce;
    if (e->data_offset)
        return e->data_offset;
    uint8_t lh[30];
    if (zip_pread(z, lh, 30, e->local_offset) != 30 || rd32(lh) != 0x04034b50)
        return 0;
    e->data_offset = e->local_offset + 30 + rd16(lh + 26) + rd16(lh + 28);
    return e->data_offset;
}

void *zip_read(ZipArchive *z, const ZipEntry *e, size_t *out_size)
{
    uint64_t off = zip_data_offset(z, e);
    if (!off)
        return NULL;
    uint8_t *out = malloc((size_t)e->size + 1);
    if (!out)
        return NULL;
    if (e->method == 0) {
        if (zip_pread(z, out, e->size, off) != e->size) { free(out); return NULL; }
    } else if (e->method == 8) {
        uint8_t *in = malloc(e->comp_size ? e->comp_size : 1);
        if (zip_pread(z, in, e->comp_size, off) != e->comp_size) { free(in); free(out); return NULL; }
        z_stream s = {0};
        inflateInit2(&s, -MAX_WBITS);
        s.next_in = in; s.avail_in = e->comp_size;
        s.next_out = out; s.avail_out = e->size;
        int rc = inflate(&s, Z_FINISH);
        inflateEnd(&s);
        free(in);
        if (rc != Z_STREAM_END) { free(out); return NULL; }
    } else {
        free(out);
        return NULL;
    }
    out[e->size] = 0;
    if (out_size) *out_size = e->size;
    return out;
}
