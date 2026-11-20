// Loads Android arm64 shared objects on Switch 2.
//
// The ELF image is laid out in heap pages, wrapped into an NRO0 header and
// handed to kaiHEN's ldr:ro, which maps it with real code permissions:
//
//   [0, text)        all non-writable PT_LOADs  -> RX   (text + rodata)
//   [text, data)     the gap before the RW load -> R    (NRO "ro", padding)
//   [data, end)      the writable PT_LOAD(s)    -> RW   (relro, got, data, bss)
//
// Every relocation of these libraries targets the RW part, so they are
// applied after the mapping, at the final address.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"
#include <kaihen.h>

typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize,
    e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz,
    p_memsz, p_align; } Elf64_Phdr;
typedef struct { int64_t d_tag; uint64_t d_val; } Elf64_Dyn;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
    uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PF_W 2
#define DT_NULL 0
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_INIT 12
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_GNU_HASH 0x6ffffef5

#define R_AARCH64_ABS64 257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027

#define PAGE 0x1000ULL
#define ALIGN_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))
#define ALIGN_DOWN(x) ((x) & ~(PAGE - 1))

struct so_module {
    char name[64];
    uintptr_t base;
    size_t size;
    Elf64_Phdr *phdr; // heap copy (the original is under the NRO header)
    int phnum;
    Elf64_Sym *dynsym;
    const char *dynstr;
    uint32_t *hash;
    uint32_t *gnu_hash;
    size_t nsyms;
    Elf64_Rela *rela; size_t rela_count;
    Elf64_Rela *jmprel; size_t jmprel_count;
    uintptr_t init;
    uintptr_t *init_array; size_t init_count;
    so_module *next;
};

static so_module *g_modules;
static bool g_ro_ready;
static Service g_ro;

static const so_call_hook *g_pending_hooks;
static size_t g_pending_hook_count;

void so_set_call_hooks(const so_call_hook *hooks, size_t count)
{
    g_pending_hooks = hooks;
    g_pending_hook_count = count;
}

// bl (0x94...) or b (0x14...): the opcode of the original is kept.
static uint32_t enc_branch(uint32_t opcode, uint64_t pc, uint64_t target)
{
    return opcode | (uint32_t)(((int64_t)(target - pc) >> 2) & 0x3ffffff);
}

static bool dec_bl(uint32_t ins, uint64_t pc, uint64_t *target)
{
    uint32_t op = ins & 0xfc000000u;
    if (op != 0x94000000u && op != 0x14000000u) return false;
    int64_t imm = ins & 0x3ffffff;
    if (imm & 0x2000000) imm -= 0x4000000;
    *target = pc + imm * 4;
    return true;
}

// Linker-style hash functions.
static uint32_t elf_hash(const char *name)
{
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        if ((g = h & 0xf0000000)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static uint32_t gnu_hash(const char *name)
{
    uint32_t h = 5381;
    while (*name) h = h * 33 + (uint8_t)*name++;
    return h;
}

static size_t gnu_hash_symcount(const uint32_t *gh)
{
    uint32_t nbuckets = gh[0], symoffset = gh[1], bloom_size = gh[2];
    const uint64_t *bloom = (const uint64_t *)(gh + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
    const uint32_t *chain = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++)
        if (buckets[i] > last) last = buckets[i];
    if (last < symoffset) return symoffset;
    while (!(chain[last - symoffset] & 1)) last++;
    return last + 1;
}

static Elf64_Sym *lookup_sym(so_module *m, const char *name)
{
    if (m->gnu_hash) {
        const uint32_t *gh = m->gnu_hash;
        uint32_t nbuckets = gh[0], symoffset = gh[1], bloom_size = gh[2];
        const uint64_t *bloom = (const uint64_t *)(gh + 4);
        const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
        const uint32_t *chain = buckets + nbuckets;
        uint32_t h = gnu_hash(name);
        uint32_t idx = buckets[h % nbuckets];
        if (idx < symoffset) return NULL;
        for (;;) {
            uint32_t ch = chain[idx - symoffset];
            if ((h | 1) == (ch | 1)) {
                Elf64_Sym *s = &m->dynsym[idx];
                if (strcmp(m->dynstr + s->st_name, name) == 0)
                    return s;
            }
            if (ch & 1) break;
            idx++;
        }
        return NULL;
    }
    if (m->hash) {
        uint32_t nbucket = m->hash[0];
        const uint32_t *bucket = m->hash + 2, *chain = bucket + nbucket;
        for (uint32_t i = bucket[elf_hash(name) % nbucket]; i; i = chain[i]) {
            Elf64_Sym *s = &m->dynsym[i];
            if (strcmp(m->dynstr + s->st_name, name) == 0)
                return s;
        }
    }
    return NULL;
}

uintptr_t so_symbol(so_module *m, const char *name)
{
    Elf64_Sym *s = lookup_sym(m, name);
    if (!s || s->st_shndx == 0)
        return 0;
    return m->base + s->st_value;
}

uintptr_t so_base(so_module *m) { return m->base; }
const char *so_name(so_module *m) { return m->name; }

so_module *so_find(const char *name)
{
    for (so_module *m = g_modules; m; m = m->next)
        if (strcmp(m->name, name) == 0)
            return m;
    return NULL;
}

// Reads a dynamic entry pointer through the unrelocated image.
static void parse_dynamic(so_module *m, uint8_t *img, Elf64_Dyn *dyn, uintptr_t base)
{
    size_t relasz = 0, pltrelsz = 0, init_arraysz = 0;
    uint64_t rela = 0, jmprel = 0, init_array = 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
        case DT_HASH: m->hash = (uint32_t *)(base + dyn->d_val); break;
        case DT_GNU_HASH: m->gnu_hash = (uint32_t *)(base + dyn->d_val); break;
        case DT_STRTAB: m->dynstr = (const char *)(base + dyn->d_val); break;
        case DT_SYMTAB: m->dynsym = (Elf64_Sym *)(base + dyn->d_val); break;
        case DT_RELA: rela = dyn->d_val; break;
        case DT_RELASZ: relasz = dyn->d_val; break;
        case DT_JMPREL: jmprel = dyn->d_val; break;
        case DT_PLTRELSZ: pltrelsz = dyn->d_val; break;
        case DT_INIT: m->init = dyn->d_val; break;
        case DT_INIT_ARRAY: init_array = dyn->d_val; break;
        case DT_INIT_ARRAYSZ: init_arraysz = dyn->d_val; break;
        }
    }
    (void)img;
    m->rela = rela ? (Elf64_Rela *)(base + rela) : NULL;
    m->rela_count = relasz / sizeof(Elf64_Rela);
    m->jmprel = jmprel ? (Elf64_Rela *)(base + jmprel) : NULL;
    m->jmprel_count = pltrelsz / sizeof(Elf64_Rela);
    m->init_array = init_array ? (uintptr_t *)(base + init_array) : NULL;
    m->init_count = init_arraysz / sizeof(uintptr_t);
    if (m->init) m->init += base;
}

static void make_module_id(uint8_t out[32], const char *name, const void *elf, size_t size)
{
    // Unique per library: FNV over the name and a sample of the file.
    uint64_t h[4] = {0xcbf29ce484222325ULL, 0x84222325cbf29ce4ULL, 0x100000001b3ULL, 0x1b3ULL};
    for (int k = 0; k < 4; k++) {
        for (const char *p = name; *p; p++) { h[k] ^= (uint8_t)*p; h[k] *= 0x100000001b3ULL; }
        const uint8_t *b = elf;
        for (size_t i = k; i < size; i += 4093) { h[k] ^= b[i]; h[k] *= 0x100000001b3ULL; }
    }
    memcpy(out, h, 32);
}

so_module *so_load(const char *name, const void *elf, size_t elf_size,
                   const so_patch *patches, size_t patch_count)
{
    const Elf64_Ehdr *eh = elf;
    if (elf_size < sizeof(*eh) || memcmp(eh->e_ident, "\x7f" "ELF", 4) || eh->e_machine != 183) {
        LOG("so_load(%s): not an arm64 ELF\n", name);
        return NULL;
    }
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)elf + eh->e_phoff);

    uint64_t text_end = 0, data_start = UINT64_MAX, data_end = 0;
    uint64_t dyn_vaddr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) dyn_vaddr = ph[i].p_vaddr;
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t end = ph[i].p_vaddr + ph[i].p_memsz;
        if (ph[i].p_flags & PF_W) {
            if (ALIGN_DOWN(ph[i].p_vaddr) < data_start) data_start = ALIGN_DOWN(ph[i].p_vaddr);
            if (end > data_end) data_end = end;
        } else if (end > text_end) {
            text_end = end;
        }
    }
    uint64_t text_size = ALIGN_UP(text_end);
    uint64_t data_used_end = data_end;
    data_end = ALIGN_UP(data_end);
    if (data_start == UINT64_MAX || data_start < text_size) {
        LOG("so_load(%s): unsupported segment layout\n", name);
        return NULL;
    }
    uint64_t ro_size = data_start - text_size;
    uint64_t image_size = data_end;
    if (ro_size == 0) {
        LOG("so_load(%s): no gap between text and data for the NRO ro segment\n", name);
        return NULL;
    }

    uint8_t *img = aligned_alloc(PAGE, image_size);
    if (!img) {
        LOG("so_load(%s): out of memory (%lu bytes)\n", name, (unsigned long)image_size);
        return NULL;
    }
    memset(img, 0, image_size);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        memcpy(img + ph[i].p_vaddr, (const uint8_t *)elf + ph[i].p_offset, ph[i].p_filesz);
    }

    so_module *m = calloc(1, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->size = image_size;
    m->phnum = eh->e_phnum;
    m->phdr = malloc(sizeof(Elf64_Phdr) * eh->e_phnum);
    memcpy(m->phdr, ph, sizeof(Elf64_Phdr) * eh->e_phnum);

    const so_call_hook *hooks = g_pending_hooks;
    size_t hook_count = g_pending_hook_count;
    g_pending_hooks = NULL;
    g_pending_hook_count = 0;
    uint64_t hook_slots[16];
    size_t hooks_ok = 0;

    // Code patches, applied while the image is still writable.
    if (patch_count || hook_count) {
        parse_dynamic(m, img, (Elf64_Dyn *)(img + dyn_vaddr), (uintptr_t)img);
        m->base = (uintptr_t)img;

        // Call-site hooks: trampolines go in the zero padding after the code,
        // their target pointers in the padding after the data.
        uint64_t tramp = (text_end + 15) & ~15ULL;
        uint64_t slot = (data_used_end + 7) & ~7ULL;
        for (size_t i = 0; i < hook_count && i < 16; i++) {
            const so_call_hook *h = &hooks[i];
            Elf64_Sym *ts = lookup_sym(m, h->target);
            Elf64_Sym *cs = lookup_sym(m, h->caller);
            uint64_t callsite = 0;
            if (ts && ts->st_value && cs && cs->st_value) {
                // the first call within the caller (at most 256 instructions)
                uint64_t len = cs->st_size ? cs->st_size : 1024;
                if (len > 1024) len = 1024;
                for (uint64_t o = 0; o + 4 <= len; o += 4) {
                    uint64_t pc = cs->st_value + o, cur;
                    if (dec_bl(*(uint32_t *)(img + pc), pc, &cur) && cur == ts->st_value) {
                        callsite = pc;
                        break;
                    }
                }
            }
            if (!callsite) {
                LOG("so_load(%s): hook skipped (no call from %s to %s)\n", name, h->caller, h->target);
                hook_slots[i] = 0;
                continue;
            }
            uint32_t ins = *(uint32_t *)(img + callsite);
            if (tramp + 12 > text_size || slot + 8 > data_end) {
                LOG("so_load(%s): no room for hook %zu\n", name, i);
                hook_slots[i] = 0;
                continue;
            }
            // adrp x16, slot ; ldr x16, [x16, #lo12(slot)] ; br x16
            int64_t pages = (int64_t)(ALIGN_DOWN(slot) - ALIGN_DOWN(tramp)) >> 12;
            uint32_t immlo = pages & 3, immhi = (pages >> 2) & 0x7ffff;
            uint32_t *t = (uint32_t *)(img + tramp);
            t[0] = 0x90000000u | (immlo << 29) | (immhi << 5) | 16;
            t[1] = 0xf9400000u | (uint32_t)(((slot & 0xfff) / 8) << 10) | (16 << 5) | 16;
            t[2] = 0xd61f0200u;
            *(uint32_t *)(img + callsite) = enc_branch(ins & 0xfc000000u, callsite, tramp);
            hook_slots[i] = slot;
            hooks_ok++;
            LOG("so_load(%s): hooked call at %#lx (%s -> %s)\n", name, (unsigned long)callsite,
                h->caller, h->target);
            tramp += 16;
            slot += 8;
        }
        for (size_t i = 0; i < patch_count; i++) {
            Elf64_Sym *s = lookup_sym(m, patches[i].symbol);
            if (!s || !s->st_value) {
                LOG("so_load(%s): patch target %s not found\n", name, patches[i].symbol);
                continue;
            }
            uint64_t off = patches[i].offset;
            if (off == SO_PATCH_SCAN) {
                // find the expected instruction instead of trusting an offset
                uint64_t len = s->st_size ? s->st_size : 1024;
                if (len > 1024) len = 1024;
                bool found = false;
                for (uint64_t o = 0; o + 4 <= len && !found; o += 4)
                    if (*(uint32_t *)(img + s->st_value + o) == patches[i].expect) { off = o; found = true; }
                if (!found) {
                    LOG("so_load(%s): patch %s skipped (%08x not found)\n", name, patches[i].symbol,
                        patches[i].expect);
                    continue;
                }
            }
            uint8_t *at = img + s->st_value + off;
            if (patches[i].expect && *(uint32_t *)at != patches[i].expect) {
                LOG("so_load(%s): patch %s+%#lx skipped (unexpected code %08x)\n", name,
                    patches[i].symbol, (unsigned long)patches[i].offset, *(uint32_t *)at);
                continue;
            }
            memcpy(at, patches[i].code, patches[i].count * 4);
            LOG("so_load(%s): patched %s+%#lx\n", name, patches[i].symbol, (unsigned long)off);
        }
    }

    // NRO0 header over the (unused at runtime) ELF header.
    uint8_t *h = img;
    memset(h, 0, 0x80);
    *(uint32_t *)(h + 0x10) = 0x304f524e; // "NRO0"
    *(uint32_t *)(h + 0x14) = 0;
    *(uint32_t *)(h + 0x18) = (uint32_t)image_size;
    *(uint32_t *)(h + 0x1c) = 0;
    *(uint32_t *)(h + 0x20) = 0;
    *(uint32_t *)(h + 0x24) = (uint32_t)text_size;
    *(uint32_t *)(h + 0x28) = (uint32_t)text_size;
    *(uint32_t *)(h + 0x2c) = (uint32_t)ro_size;
    *(uint32_t *)(h + 0x30) = (uint32_t)data_start;
    *(uint32_t *)(h + 0x34) = (uint32_t)(data_end - data_start);
    *(uint32_t *)(h + 0x38) = 0;
    make_module_id(h + 0x40, name, elf, elf_size);

    armDCacheFlush(img, image_size);

    if (!g_ro_ready) {
        // hbloader hands each NRO kaiHEN's already-initialised ldr:ro
        // session as a service override.
        Result rc = kaihenGetRoSession(&g_ro);
        LOG("so_load: kaihenGetRoSession: 0x%x\n", rc);
        if (R_FAILED(rc)) {
            rc = ldrRoInitialize();
            LOG("so_load: ldrRoInitialize: 0x%x\n", rc);
            if (R_FAILED(rc))
                fatal_error("ldr:ro is not available (0x%x).\nStart the game through kaiHEN's hbmenu.", rc);
            g_ro = *ldrRoGetServiceSession();
        }
        g_ro_ready = true;
    }
    u64 base = 0;
    const struct { u64 pid_placeholder, nro_address, nro_size, bss_address, bss_size; } in =
        { 0, (u64)img, image_size, 0, 0 };
    LOG("so_load(%s): LoadNro %#lx size %#lx\n", name, (unsigned long)img, (unsigned long)image_size);
    Result rc = serviceDispatchInOut(&g_ro, 0, in, base, .in_send_pid = true);
    if (R_FAILED(rc)) {
        LOG("so_load(%s): ldrRoLoadNro failed: 0x%x\n", name, rc);
        fatal_error("Could not map %s (0x%x).", name, rc);
    }
    // The heap pages now back the mapping and are no longer ours to touch.
    m->base = base;
    for (size_t i = 0; i < hook_count && i < 16; i++)
        if (hook_slots[i]) *(uintptr_t *)(base + hook_slots[i]) = (uintptr_t)hooks[i].hook;
    (void)hooks_ok;
    m->hash = NULL; m->gnu_hash = NULL;
    parse_dynamic(m, NULL, (Elf64_Dyn *)(base + dyn_vaddr), base);
    if (m->hash) m->nsyms = m->hash[1];
    else if (m->gnu_hash) m->nsyms = gnu_hash_symcount(m->gnu_hash);

    m->next = g_modules;
    g_modules = m;
    LOG("so_load(%s): mapped at %#lx (text %#lx ro %#lx data %#lx, %zu syms)\n", name,
        (unsigned long)base, (unsigned long)text_size, (unsigned long)ro_size,
        (unsigned long)(data_end - data_start), m->nsyms);
    return m;
}

static int apply_relocs(so_module *m, Elf64_Rela *r, size_t count, so_resolver resolver, int *missing)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t type = (uint32_t)r[i].r_info;
        uint32_t symi = (uint32_t)(r[i].r_info >> 32);
        uintptr_t *where = (uintptr_t *)(m->base + r[i].r_offset);
        switch (type) {
        case R_AARCH64_RELATIVE:
            *where = m->base + r[i].r_addend;
            break;
        case R_AARCH64_ABS64:
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT: {
            Elf64_Sym *s = &m->dynsym[symi];
            const char *sname = m->dynstr + s->st_name;
            uintptr_t addr = 0;
            // A library's own definitions (its static libc++ and so on) stay
            // its own; only undefined symbols go to the resolver.
            if (s->st_shndx != 0) {
                addr = m->base + s->st_value;
            } else {
                addr = resolver(sname, m);
                if (!addr) {
                    if ((s->st_info >> 4) == 2) { // STB_WEAK
                        addr = 0;
                    } else {
                        LOG("  unresolved: %s (%s)\n", sname, m->name);
                        (*missing)++;
                        addr = (uintptr_t)unresolved_import_stub;
                    }
                }
            }
            *where = addr + (addr ? r[i].r_addend : 0);
            break;
        }
        default:
            LOG("so_relocate(%s): unsupported relocation type %u\n", m->name, type);
            return -1;
        }
    }
    return 0;
}

int so_relocate(so_module *m, so_resolver resolver)
{
    int missing = 0;
    if (apply_relocs(m, m->rela, m->rela_count, resolver, &missing) < 0) return -1;
    if (apply_relocs(m, m->jmprel, m->jmprel_count, resolver, &missing) < 0) return -1;
    LOG("so_relocate(%s): done, %d unresolved\n", m->name, missing);
    return missing;
}

void so_init(so_module *m)
{
    if (m->init)
        ((void (*)(void))m->init)();
    for (size_t i = 0; i < m->init_count; i++) {
        uintptr_t fn = m->init_array[i];
        if (!fn || fn == (uintptr_t)-1) continue;
        if (g_config.trace_init) {
            char buf[256];
            LOG("  ctor %zu/%zu %s\n", i + 1, m->init_count, so_describe_address(fn, buf, sizeof(buf)));
        }
        ((void (*)(void))fn)();
    }
    LOG("so_init(%s): %zu constructors\n", m->name, m->init_count);
}

// Unmaps every loaded library through RO (UnloadNro, command 1). Only call
// once nothing can run their code any more.
bool so_unload_all(void)
{
    bool ok = true;
    for (so_module *m = g_modules; m; m = m->next) {
        const struct { u64 pid_placeholder, nro_address; } in = { 0, m->base };
        Result rc = serviceDispatchIn(&g_ro, 1, in, .in_send_pid = true);
        LOG("so_unload(%s): 0x%x\n", m->name, rc);
        if (R_FAILED(rc)) ok = false;
    }
    if (ok) g_modules = NULL;
    return ok;
}

const char *so_describe_address(uintptr_t addr, char *buf, size_t size)
{
    for (so_module *m = g_modules; m; m = m->next) {
        if (addr < m->base || addr >= m->base + m->size) continue;
        const char *best = NULL;
        uintptr_t best_addr = 0;
        for (size_t i = 0; i < m->nsyms; i++) {
            Elf64_Sym *s = &m->dynsym[i];
            if (!s->st_value || (s->st_info & 0xf) != 2) continue;
            uintptr_t a = m->base + s->st_value;
            if (a <= addr && a > best_addr) { best_addr = a; best = m->dynstr + s->st_name; }
        }
        if (best)
            snprintf(buf, size, "%s+%#lx (%s+%#lx)", m->name, (unsigned long)(addr - m->base),
                     best, (unsigned long)(addr - best_addr));
        else
            snprintf(buf, size, "%s+%#lx", m->name, (unsigned long)(addr - m->base));
        return buf;
    }
    snprintf(buf, size, "%#lx", (unsigned long)addr);
    return buf;
}

// The exported function at or before addr in a loaded library (profiler).
bool so_function_at(uintptr_t addr, const char **module, const char **name, uintptr_t *start)
{
    for (so_module *m = g_modules; m; m = m->next) {
        if (addr < m->base || addr >= m->base + m->size) continue;
        const char *best = NULL;
        uintptr_t best_addr = 0;
        for (size_t i = 0; i < m->nsyms; i++) {
            Elf64_Sym *s = &m->dynsym[i];
            if (!s->st_value || (s->st_info & 0xf) != 2) continue;
            uintptr_t a = m->base + s->st_value;
            if (a <= addr && a > best_addr) { best_addr = a; best = m->dynstr + s->st_name; }
        }
        *module = m->name;
        *name = best ? best : "?";
        *start = best ? best_addr : m->base;
        return true;
    }
    return false;
}

// bionic's struct dl_phdr_info
typedef struct {
    uint64_t dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    uint16_t dlpi_phnum;
    unsigned long long dlpi_adds, dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
} bionic_dl_phdr_info;

int so_iterate_phdr(bionic_phdr_cb cb, void *data)
{
    for (so_module *m = g_modules; m; m = m->next) {
        bionic_dl_phdr_info info = {
            .dlpi_addr = m->base, .dlpi_name = m->name,
            .dlpi_phdr = m->phdr, .dlpi_phnum = (uint16_t)m->phnum,
            .dlpi_adds = 1, .dlpi_subs = 0,
        };
        int rc = cb(&info, sizeof(info), data);
        if (rc) return rc;
    }
    return 0;
}
