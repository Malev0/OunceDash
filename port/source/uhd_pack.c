// RAM cache of the -uhd files in Resources' subfolders (mainly icons/).
//
// PC GD keeps its icons in one folder of ~7200 files. On the SD card's FAT
// filesystem, opening a file by name scans that folder, about 18 ms per
// file, and a page of the icon menu opens ~70 of them on the game thread.
// So the files are loaded once and served from memory:
//   - first launch: a thread reads each file and writes them all to one pack
//     file, cache/uhd-pack.bin; the game waits for it on a progress screen
//     (loadscreen.c), ~30 s
//   - later launches: a low-priority thread reads the pack in one go (well
//     under a second) while the game starts
// The pack records the list of names it holds; a different set of -uhd
// files (another Resources folder) rebuilds it. Delete the cache folder to
// force a rebuild after replacing files with same-named ones.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "port.h"

#define PACK_DIR   GD_ROOT "/cache"
#define PACK_PATH  PACK_DIR "/uhd-pack.bin"
#define PACK_MAGIC 0x4b50444fu /* "ODPK" */
#define PACK_VERSION 1

typedef struct {
    char *name;               // lower-case path relative to Resources
    const uint8_t *data;
    uint32_t size;
    volatile int ready;       // set (release) once data/size are valid
} PackEntry;

typedef struct {
    uint32_t magic, version, count, reserved;
    uint64_t names_hash, data_size;
} PackHeader;

typedef struct { uint32_t name_len, size; uint64_t offset; } PackRecord;

static PackEntry *g_entries;
static size_t g_count;
static uint32_t *g_table;      // open addressing: index + 1, 0 = empty
static size_t g_table_size;
static uint64_t g_names_hash;
static char g_root[256];
static Thread g_thread;
static bool g_thread_started;
static volatile bool g_stop;
static volatile bool g_building, g_writing, g_finished;
static volatile size_t g_done;

bool uhd_pack_building(void) { return g_building && !g_finished; }
bool uhd_pack_finished(void) { return g_finished; }
void uhd_pack_progress(size_t *done, size_t *total, bool *writing)
{
    *done = g_done;
    *total = g_count;
    *writing = g_writing;
}

static uint32_t hash_lower(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c += 32;
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const PackEntry *)a)->name, ((const PackEntry *)b)->name);
}

static PackEntry *find(const char *rel)
{
    if (!g_table_size) return NULL;
    size_t j = hash_lower(rel) & (g_table_size - 1);
    while (g_table[j]) {
        PackEntry *e = &g_entries[g_table[j] - 1];
        if (!strcasecmp(e->name, rel)) return e;
        j = (j + 1) & (g_table_size - 1);
    }
    return NULL;
}

bool uhd_pack_get(const char *rel, const void **data, uint32_t *size)
{
    PackEntry *e = find(rel);
    if (!e || !__atomic_load_n(&e->ready, __ATOMIC_ACQUIRE)) return false;
    *data = e->data;
    *size = e->size;
    return true;
}

static void publish(PackEntry *e, const uint8_t *data, uint32_t size)
{
    e->data = data;
    e->size = size;
    __atomic_store_n(&e->ready, 1, __ATOMIC_RELEASE);
}

static bool read_whole(const char *path, uint8_t **out, size_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) { close(fd); return false; }
    size_t len = (size_t)st.st_size;
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) { close(fd); return false; }
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    if (got != len) { free(buf); return false; }
    *out = buf;
    *out_size = len;
    return true;
}

static bool load_pack(void)
{
    uint8_t *buf;
    size_t len;
    if (!read_whole(PACK_PATH, &buf, &len)) return false;
    const PackHeader *h = (const PackHeader *)buf;
    size_t recs = sizeof(PackHeader) + (size_t)g_count * sizeof(PackRecord);
    if (len < sizeof(PackHeader) || h->magic != PACK_MAGIC || h->version != PACK_VERSION ||
        h->count != g_count || h->names_hash != g_names_hash || len < recs) {
        free(buf);
        return false;
    }
    const PackRecord *r = (const PackRecord *)(buf + sizeof(PackHeader));
    size_t names_total = 0;
    for (size_t i = 0; i < g_count; i++) names_total += r[i].name_len;
    const uint8_t *names = buf + recs;
    const uint8_t *data = names + names_total;
    if (recs + names_total + h->data_size != len) { free(buf); return false; }
    // records are in the same (sorted) order as g_entries; check each name
    const uint8_t *np = names;
    for (size_t i = 0; i < g_count; i++) {
        if (strlen(g_entries[i].name) != r[i].name_len ||
            memcmp(g_entries[i].name, np, r[i].name_len) ||
            r[i].offset + r[i].size > h->data_size) {
            free(buf);
            return false;
        }
        np += r[i].name_len;
    }
    for (size_t i = 0; i < g_count; i++) publish(&g_entries[i], data + r[i].offset, r[i].size);
    return true; // buf stays allocated: the entries point into it
}

static void write_pack(void)
{
    mkdir(PACK_DIR, 0777);
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", PACK_PATH);
    FILE *f = fopen(tmp, "wb");
    if (!f) { LOG("uhd pack: cannot write %s\n", tmp); return; }
    PackHeader h = { PACK_MAGIC, PACK_VERSION, (uint32_t)g_count, 0, g_names_hash, 0 };
    for (size_t i = 0; i < g_count; i++) h.data_size += g_entries[i].size;
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    uint64_t off = 0;
    for (size_t i = 0; ok && i < g_count; i++) {
        PackRecord r = { (uint32_t)strlen(g_entries[i].name), g_entries[i].size, off };
        ok = fwrite(&r, sizeof(r), 1, f) == 1;
        off += g_entries[i].size;
    }
    for (size_t i = 0; ok && i < g_count; i++)
        ok = fwrite(g_entries[i].name, 1, strlen(g_entries[i].name), f) == strlen(g_entries[i].name);
    for (size_t i = 0; ok && i < g_count && !g_stop; i++)
        ok = !g_entries[i].size || fwrite(g_entries[i].data, g_entries[i].size, 1, f) == 1;
    ok = fclose(f) == 0 && ok && !g_stop;
    if (ok) {
        remove(PACK_PATH);
        ok = rename(tmp, PACK_PATH) == 0;
    }
    if (!ok) remove(tmp);
    LOG("uhd pack: %s %s (%lu MB)\n", ok ? "wrote" : "could not write", PACK_PATH,
        (unsigned long)(h.data_size >> 20));
}

// Whether the pack on the SD card matches this set of files (header only).
static bool pack_matches(void)
{
    FILE *f = fopen(PACK_PATH, "rb");
    if (!f) return false;
    PackHeader h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == PACK_MAGIC && h.version == PACK_VERSION &&
              h.count == g_count && h.names_hash == g_names_hash;
    fclose(f);
    return ok;
}

static void pack_thread(void *arg)
{
    (void)arg;
    uint64_t t = time_ns();
    if (!g_building) {
        if (load_pack()) {
            LOG("uhd pack: %zu files loaded from %s in %lu ms\n", g_count, PACK_PATH,
                (unsigned long)((time_ns() - t) / 1000000));
            g_finished = true;
            return;
        }
        LOG("uhd pack: %s is unreadable, reading the files instead\n", PACK_PATH);
    }
    LOG("uhd pack: building (%zu files, first launch or changed Resources)\n", g_count);
    size_t failed = 0;
    for (size_t i = 0; i < g_count && !g_stop; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", g_root, g_entries[i].name);
        uint8_t *buf;
        size_t len;
        if (read_whole(path, &buf, &len)) publish(&g_entries[i], buf, (uint32_t)len);
        else { publish(&g_entries[i], NULL, 0); failed++; }
        g_done = i + 1;
    }
    if (g_stop) return;
    LOG("uhd pack: %zu files read in %lu s (%zu failed)\n", g_count,
        (unsigned long)((time_ns() - t) / 1000000000ULL), failed);
    g_writing = true;
    if (!failed) write_pack();
    g_finished = true;
}

// names: relative paths (any case) of the files to cache.
void uhd_pack_start(const char *root, char **names, size_t count)
{
    if (!count) return;
    snprintf(g_root, sizeof(g_root), "%s", root);
    g_entries = calloc(count, sizeof(PackEntry));
    if (!g_entries) return;
    for (size_t i = 0; i < count; i++) {
        g_entries[i].name = strdup(names[i]);
        for (char *c = g_entries[i].name; c && *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    }
    g_count = count;
    qsort(g_entries, g_count, sizeof(PackEntry), cmp_entry);
    // FNV-1a over the sorted names: identifies this set of files
    g_names_hash = 1469598103934665603ULL;
    for (size_t i = 0; i < g_count; i++) {
        for (const char *c = g_entries[i].name; *c; c++)
            g_names_hash = (g_names_hash ^ (uint8_t)*c) * 1099511628211ULL;
        g_names_hash = (g_names_hash ^ 0xff) * 1099511628211ULL;
    }
    g_table_size = 1;
    while (g_table_size < g_count * 2) g_table_size <<= 1;
    g_table = calloc(g_table_size, sizeof(uint32_t));
    if (!g_table) { g_table_size = 0; return; }
    for (size_t i = 0; i < g_count; i++) {
        size_t j = hash_lower(g_entries[i].name) & (g_table_size - 1);
        while (g_table[j]) j = (j + 1) & (g_table_size - 1);
        g_table[j] = (uint32_t)i + 1;
    }
    // Building: the game waits for it, so it runs at the game's priority.
    // Loading: in the background, below the game (0x2c).
    g_building = !pack_matches();
    u64 mask = cores_worker_mask();
    int core = -2;
    for (int c = 63; c >= 0; c--) if ((mask >> c) & 1) { core = c; break; }
    if (R_SUCCEEDED(threadCreate(&g_thread, pack_thread, NULL, NULL, 0x10000,
                                 g_building ? 0x2c : 0x3b, core))) {
        if (mask) svcSetThreadCoreMask(g_thread.handle, core, mask);
        g_thread_started = R_SUCCEEDED(threadStart(&g_thread));
    }
    if (!g_thread_started) g_finished = true; // nothing to wait for
    LOG("uhd pack: %zu files to cache (%s), thread %s\n", g_count, g_building ? "building" : "loading",
        g_thread_started ? "started" : "FAILED");
}

void uhd_pack_stop(void)
{
    if (!g_thread_started) return;
    g_stop = true;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    g_thread_started = false;
}
