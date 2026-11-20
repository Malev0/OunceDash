// High-resolution (-uhd) textures from the user's own PC copy of the game.
//
// The Android APK only ships -hd (2x) textures, and its cocos2d caps the
// content scale at 2, so at 1080p and above every sprite is upscaled and
// blurry. The game's file lookup still knows the -uhd suffix (it tries it
// first when the content scale is 4). If the user copies the Resources
// folder of their PC version to sdmc:/switch/OunceDash/Resources, the
// loader
//   - raises the content scale for Medium/High texture quality to 4, and
//   - answers the game's requests for assets/...-uhd.* from that folder
//     (CCFileUtilsAndroid::isFileExist / getFileData, through its vtable).
// Everything else still comes from the APK; anything without a -uhd file
// falls back to -hd through the game's own lookup.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "port.h"

#define UHD_DIR GD_ROOT "/Resources"

// GD 2.2.145 arm64
#define VT_CCFileUtilsAndroid "_ZTVN7cocos2d18CCFileUtilsAndroidE"

static bool g_uhd_available;

// A libc++ std::string returned by value (through x8, like any 24-byte struct).
typedef struct { uint8_t b[24]; } cxx_string;
static cxx_string (*g_fullPathForFilename)(void *self, const char *name, bool skip_suffix);
static unsigned char *(*g_orig_doGetFileData)(void *self, const char *name, const char *mode,
                                              unsigned long *size, bool async);
static bool (*g_orig_isFileExist)(void *self, const void *path);
static int g_served;

// Every -uhd file under Resources/, as lower-case paths relative to it
// ("icons/player_01-uhd.png"), listed once at startup. The game checks for
// -uhd files hundreds of times when it opens a page of icons; asking the SD
// card each time costs milliseconds per check (FAT scans the directory), so
// the checks are answered from this set. Case-insensitive, like FAT.
static char **g_names;
static size_t g_names_cap, g_names_count;

static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c += 32;
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static void names_add(const char *rel)
{
    if (g_names_count * 2 >= g_names_cap) {
        size_t cap = g_names_cap ? g_names_cap * 2 : 4096;
        char **t = calloc(cap, sizeof(char *));
        if (!t) return;
        for (size_t i = 0; i < g_names_cap; i++) {
            if (!g_names[i]) continue;
            size_t j = name_hash(g_names[i]) & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = g_names[i];
        }
        free(g_names);
        g_names = t;
        g_names_cap = cap;
    }
    char *copy = strdup(rel);
    if (!copy) return;
    for (char *c = copy; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    size_t j = name_hash(copy) & (g_names_cap - 1);
    while (g_names[j]) {
        if (!strcmp(g_names[j], copy)) { free(copy); return; }
        j = (j + 1) & (g_names_cap - 1);
    }
    g_names[j] = copy;
    g_names_count++;
}

static bool names_has(const char *rel)
{
    if (!g_names_cap) return false;
    size_t j = name_hash(rel) & (g_names_cap - 1);
    while (g_names[j]) {
        if (!strcasecmp(g_names[j], rel)) return true;
        j = (j + 1) & (g_names_cap - 1);
    }
    return false;
}

static void names_scan(const char *dir, const char *rel, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[512], subrel[512];
        snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
        snprintf(subrel, sizeof(subrel), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            is_dir = stat(sub, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir) {
            if (depth < 4) names_scan(sub, subrel, depth + 1);
        } else if (strstr(e->d_name, "-uhd.")) {
            names_add(subrel);
        }
    }
    closedir(d);
}

bool uhd_detect(void)
{
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    LOG("memory: %lu MB used of %lu MB\n", (unsigned long)(used >> 20), (unsigned long)(total >> 20));
    if (!g_config.uhd) {
        LOG("uhd: disabled in config.ini\n");
        return false;
    }
    struct stat st;
    if (stat(UHD_DIR, &st) != 0) {
        LOG("uhd: %s not found, using the APK's -hd textures\n", UHD_DIR);
        return false;
    }
    uint64_t t = time_ns();
    names_scan(UHD_DIR, "", 0);
    g_uhd_available = g_names_count > 0;
    LOG("uhd: %zu -uhd files in %s (listed in %lu ms)\n", g_names_count, UHD_DIR,
        (unsigned long)((time_ns() - t) / 1000000));
    // Files in subfolders (icons/: thousands per folder, slow to open on FAT)
    // are cached in RAM by uhd_pack.c.
    if (g_uhd_available) {
        char **sub = malloc(g_names_count * sizeof(char *));
        size_t n = 0;
        for (size_t i = 0; sub && i < g_names_cap; i++)
            if (g_names[i] && strchr(g_names[i], '/')) sub[n++] = g_names[i];
        if (sub) uhd_pack_start(UHD_DIR, sub, n);
        free(sub);
    }
    return g_uhd_available;
}

// libc++ std::string (__ndk1, default layout): short strings keep size<<1 in
// the first byte and the text after it; long ones set bit 0 and keep the
// pointer at +16.
static const char *std_string_cstr(const void *s)
{
    const uint8_t *p = s;
    if (p[0] & 1) return *(const char *const *)(p + 16);
    return (const char *)(p + 1);
}

// "assets/GJ_GameSheet03-uhd.png" -> "sdmc:/.../Resources/GJ_GameSheet03-uhd.png"
static bool overlay_path(const char *path, char *out, size_t n)
{
    if (!path || !strstr(path, "-uhd.")) return false;
    const char *name = path;
    if (!strncmp(name, "assets/", 7)) name += 7;
    else if (name[0] == '/') return false;
    if (!names_has(name)) return false;
    snprintf(out, n, UHD_DIR "/%s", name);
    return true;
}

static bool hook_isFileExist(void *self, const void *path)
{
    char p[512];
    if (overlay_path(std_string_cstr(path), p, sizeof(p))) return true; // no SD access
    return g_orig_isFileExist(self, path);
}

static unsigned char *read_overlay(const char *name, const char *p, unsigned long *size)
{
    // from the RAM cache when it has the file
    const void *cached;
    uint32_t cached_size;
    if (uhd_pack_get(p + strlen(UHD_DIR) + 1, &cached, &cached_size) && cached) {
        unsigned char *copy = malloc(cached_size ? cached_size : 1);
        if (copy) {
            memcpy(copy, cached, cached_size);
            if (size) *size = cached_size;
            ++g_served;
            return copy;
        }
    }
    // One read() of the whole file goes straight to the filesystem, without
    // stdio's small buffer in between (these are multi-MB sprite sheets).
    int fd = open(p, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    long len = fstat(fd, &st) == 0 ? (long)st.st_size : -1;
    if (len < 0) { close(fd); return NULL; }
    // the game frees this with delete[], which is free() in its libc++
    unsigned char *buf = malloc(len > 0 ? len : 1);
    if (!buf) {
        close(fd);
        u64 used = 0, total = 0;
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        LOG("uhd: OUT OF MEMORY reading %s (%ld bytes; used %lu of %lu MB)\n", name, len,
            (unsigned long)(used >> 20), (unsigned long)(total >> 20));
        return NULL;
    }
    size_t got = 0;
    while (got < (size_t)len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    if (size) *size = got;
    static uint64_t bytes;
    bytes += got;
    if (++g_served % 50 == 0)
        LOG("uhd: %d files served (%lu MB)\n", g_served, (unsigned long)(bytes >> 20));
    return buf;
}

// Replaces the tail call to doGetFileData in both getFileData (sync) and
// getFileDataForAsync (the loading screen's background texture loads).
static unsigned char *hook_doGetFileData_inner(void *self, const char *name, const char *mode,
                                               unsigned long *size, bool async);

static unsigned char *hook_doGetFileData(void *self, const char *name, const char *mode,
                                         unsigned long *size, bool async)
{
    uint64_t t = time_ns();
    unsigned long sz = 0;
    unsigned char *r = hook_doGetFileData_inner(self, name, mode, size ? size : &sz, async);
    perf_add_file(time_ns() - t, r ? (size ? *size : sz) : 0);
    return r;
}

static unsigned char *hook_doGetFileData_inner(void *self, const char *name, const char *mode,
                                               unsigned long *size, bool async)
{
    char p[512];
    if (overlay_path(name, p, sizeof(p))) {
        unsigned char *buf = read_overlay(name, p, size);
        if (buf) return buf;
    }
    // Bare names ("goldFont.fnt") are resolved inside doGetFileData, through
    // our isFileExist, to "assets/goldFont-uhd.fnt", which the original then
    // looks for in the APK. Resolve them the same way here first.
    if (name && name[0] != '/' && !strstr(name, "-uhd.") && g_fullPathForFilename) {
        cxx_string full = g_fullPathForFilename(self, name, false);
        const char *fp = std_string_cstr(&full);
        bool served = false;
        unsigned char *buf = NULL;
        if (overlay_path(fp, p, sizeof(p))) {
            buf = read_overlay(fp, p, size);
            served = buf != NULL;
        }
        if (full.b[0] & 1) free(*(void **)(full.b + 16)); // long string storage
        if (served) return buf;
    }
    unsigned char *r = g_orig_doGetFileData(self, name, mode, size, async);
    static int missing;
    if (!r && missing < 50) { // only failures are worth a (flushed) log line
        missing++;
        LOG("file: %s%s -> NOT FOUND\n", name ? name : "(null)", async ? " [async]" : "");
    }
    return r;
}

static const so_call_hook g_hooks[] = {
    // CCFileUtilsAndroid::getFileData: mov w4,#0 ; b doGetFileData
    { "_ZN7cocos2d18CCFileUtilsAndroid11getFileDataEPKcS2_Pm",
      "_ZN7cocos2d18CCFileUtilsAndroid13doGetFileDataEPKcS2_Pmb", (void *)hook_doGetFileData },
    // CCFileUtilsAndroid::getFileDataForAsync: mov w4,#1 ; b doGetFileData
    { "_ZN7cocos2d18CCFileUtilsAndroid19getFileDataForAsyncEPKcS2_Pm",
      "_ZN7cocos2d18CCFileUtilsAndroid13doGetFileDataEPKcS2_Pmb", (void *)hook_doGetFileData },
};

void uhd_prepare_hooks(void)
{
    if (g_uhd_available) so_set_call_hooks(g_hooks, sizeof(g_hooks) / sizeof(g_hooks[0]));
}

void uhd_install(so_module *gd)
{
    if (!g_uhd_available) return;
    g_orig_doGetFileData = (__typeof__(g_orig_doGetFileData))so_symbol(gd,
        "_ZN7cocos2d18CCFileUtilsAndroid13doGetFileDataEPKcS2_Pmb");
    uintptr_t vt = so_symbol(gd, VT_CCFileUtilsAndroid);
    if (!vt || !g_orig_doGetFileData) { LOG("uhd: file functions not found\n"); g_uhd_available = false; return; }
    // Find isFileExist's slot by its value rather than assuming its index.
    uintptr_t want = so_symbol(gd, "_ZN7cocos2d18CCFileUtilsAndroid11isFileExistERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE");
    uintptr_t *slot_exist = NULL;
    for (int i = 2; i < 80 && want; i++)
        if (((uintptr_t *)vt)[i] == want) { slot_exist = &((uintptr_t *)vt)[i]; break; }
    if (!slot_exist) {
        LOG("uhd: isFileExist not found in the CCFileUtilsAndroid vtable, not hooking\n");
        g_uhd_available = false;
        return;
    }
    g_orig_isFileExist = (__typeof__(g_orig_isFileExist))*slot_exist;
    *slot_exist = (uintptr_t)hook_isFileExist;
    g_fullPathForFilename = (__typeof__(g_fullPathForFilename))so_symbol(gd,
        "_ZN7cocos2d11CCFileUtils19fullPathForFilenameEPKcb");
    LOG("uhd: file hooks installed\n");
}
