// Shared declarations for the Geometry Dash Switch 2 loader.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <switch.h>

#define GD_ROOT       "sdmc:/switch/OunceDash"
#define GD_SAVE_DIR   GD_ROOT "/save"
#define GD_LOG_PATH   GD_ROOT "/log.txt"
#define GD_CONFIG     GD_ROOT "/config.ini"
#define GD_CONTROLS   GD_ROOT "/controls.ini"
#define GD_PREFS      GD_SAVE_DIR "/prefs.ini"

// The Android paths the game believes it runs under.
#define ANDROID_PACKAGE   "com.robtopx.geometryjump"
#define ANDROID_DATA_DIR  "/data/data/" ANDROID_PACKAGE "/"
#define ANDROID_FILES_DIR ANDROID_DATA_DIR "files"
#define ANDROID_APK_PATH  "/data/app/" ANDROID_PACKAGE "/base.apk"

// ---------------------------------------------------------------- logging
void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_flush(void);
#define LOG(...) log_printf(__VA_ARGS__)
void fatal_error(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

// ---------------------------------------------------------------- config
typedef struct {
    int fps;             // target frame rate at 120 Hz (default 120)
    int width, height;   // 0 = native for the current mode
    bool vrr;
    bool controller_glyphs; // report a connected controller to the game
    float cursor_speed;  // virtual cursor speed, screen-heights per second
    bool show_cursor;
    bool trace_init;     // log every static constructor of the game (debugging)
    bool quit_to_hbmenu; // quit_action=hbmenu (default) returns to hbmenu; =home closes
    bool uhd;            // use -uhd textures from Resources/ when present
    int cpu_boost;       // CPU_BOOST_* (cpu.c)
    bool profile;        // sample GD's update and log hot functions (prof.c)
    bool gl_buffer_ring; // rotate per-frame vertex buffers (glbuf.c)
    char apk_path[512];
} GdConfig;
extern GdConfig g_config;
enum { CPU_BOOST_OFF, CPU_BOOST_LOADING };
void cpu_init(void);
void cpu_boost_loading_done(void);
void config_load(void);

// ---------------------------------------------------------------- paths
// Converts a path used by the game (Android style) into a Switch path.
// Returns `out`.
const char *path_translate(const char *in, char *out, size_t out_size);
int mkdir_p(const char *path);

// ---------------------------------------------------------------- zip / apk
typedef struct ZipEntry {
    char *name;
    uint64_t local_offset;
    uint64_t data_offset; // resolved lazily, 0 if unknown
    uint32_t comp_size, size;
    uint16_t method;
} ZipEntry;

typedef struct ZipArchive ZipArchive;
ZipArchive *zip_open(const char *path);
const ZipEntry *zip_find(ZipArchive *z, const char *name);
// Resolves the absolute file offset of a stored entry's data.
uint64_t zip_data_offset(ZipArchive *z, const ZipEntry *e);
// Reads/inflates the whole entry into a malloc'd buffer (with a NUL after it).
void *zip_read(ZipArchive *z, const ZipEntry *e, size_t *out_size);
// Reads raw bytes from the archive file (thread-safe).
size_t zip_pread(ZipArchive *z, void *buf, size_t size, uint64_t offset);
size_t zip_count(ZipArchive *z);
const ZipEntry *zip_entry_at(ZipArchive *z, size_t i);
extern ZipArchive *g_apk;

// ---------------------------------------------------------------- so loader
typedef struct so_module so_module;
typedef uintptr_t (*so_resolver)(const char *name, so_module *requester);

typedef struct {
    const char *symbol;     // patch at this exported symbol
    const uint32_t *code;   // instructions to write
    size_t count;
    uint64_t offset;        // bytes after the symbol (SO_PATCH_SCAN: search)
    uint32_t expect;        // if nonzero, the instruction that must be there
} so_patch;
// so_patch.offset: patch the first `expect` instruction within the
// function's first 256 instructions instead of a fixed offset.
#define SO_PATCH_SCAN UINT64_MAX

bool uhd_detect(void);
void uhd_install(so_module *gd);
void uhd_prepare_hooks(void);

// Redirects the first `bl target` / `b target` inside the function `caller`
// to a function of the loader (through a trampoline in the text padding and a
// pointer slot in the data padding). The call is found by scanning `caller`,
// so it does not depend on addresses of one particular game build.
typedef struct {
    const char *caller;     // exported function that contains the call
    const char *target;     // exported function it calls
    void *hook;             // loader function to call instead
} so_call_hook;

so_module *so_load(const char *name, const void *elf, size_t elf_size,
                   const so_patch *patches, size_t patch_count);
void so_set_call_hooks(const so_call_hook *hooks, size_t count); // for the next so_load
int so_relocate(so_module *m, so_resolver resolver);
void so_init(so_module *m);
uintptr_t so_symbol(so_module *m, const char *name);
uintptr_t so_base(so_module *m);
const char *so_name(so_module *m);
// Finds the module and nearest exported symbol for an address (for crash logs).
const char *so_describe_address(uintptr_t addr, char *buf, size_t size);
so_module *so_find(const char *name);

// dl_iterate_phdr support (bionic layout)
typedef int (*bionic_phdr_cb)(void *info, size_t size, void *data);
int so_iterate_phdr(bionic_phdr_cb cb, void *data);

// ---------------------------------------------------------------- imports
uintptr_t imports_resolve(const char *name, so_module *requester);
uintptr_t imports_lookup(const char *name); // shims only
void unresolved_import_stub(void);

// ---------------------------------------------------------------- jni
#include <jni.h>
extern JavaVM *g_vm;
extern JNIEnv *g_env;
void jni_init(void);
jstring jni_new_string(const char *s);
jobject jni_new_object(const char *class_name);
jintArray jni_new_int_array(const jint *data, int n);
jfloatArray jni_new_float_array(const jfloat *data, int n);
jbyteArray jni_new_byte_array(const void *data, int n);
const char *jni_string_utf(jobject s);
void jni_release_locals(void); // drops this thread's local references
jclass jni_find_class(const char *name);

typedef jvalue (*JavaMethodFn)(jobject self, const jvalue *args);
void java_register(const char *cls, const char *name, const char *sig, JavaMethodFn fn);
void java_impl_init(void);

// ---------------------------------------------------------------- game glue
extern volatile bool g_running;
extern double g_animation_interval;
extern bool g_ime_requested;
extern bool g_ime_close_requested;
extern int g_screen_w, g_screen_h;
void prefs_init(void);
const char *prefs_get(const char *key);
void prefs_set(const char *key, const char *value);
void prefs_remove(const char *key);
void prefs_flush(void);

// text rendering for Cocos2dxBitmap
void text_init(void);
void text_create_bitmap(const char *text, const char *font, int font_size,
                        float r, float g, float b, int align, int width, int height,
                        bool stroke, float sr, float sg, float sb, float stroke_size);

uint8_t *text_render_rgba(const char *text, float px, int *out_w, int *out_h);

// Show FPS option and counter (fps.c)
void fps_frame(void);
void fps_draw(void);
void fps_toggle(void);

// game entry points (resolved from libcocos2dcpp.so)
typedef struct {
    jint (*JNI_OnLoad)(JavaVM *, void *);
    void (*nativeSetApkPath)(JNIEnv *, jclass, jstring);
    void (*nativeInit)(JNIEnv *, jobject, jint, jint);
    void (*nativeRender)(JNIEnv *, jobject);
    void (*nativeOnSurfaceChanged)(JNIEnv *, jobject, jint, jint);
    void (*nativeOnPause)(JNIEnv *, jobject);
    void (*nativeOnResume)(JNIEnv *, jobject);
    void (*nativeTouchesBegin)(JNIEnv *, jobject, jint, jfloat, jfloat);
    void (*nativeTouchesEnd)(JNIEnv *, jobject, jint, jfloat, jfloat);
    void (*nativeTouchesMove)(JNIEnv *, jobject, jintArray, jfloatArray, jfloatArray);
    void (*nativeTouchesCancel)(JNIEnv *, jobject, jintArray, jfloatArray, jfloatArray);
    jboolean (*nativeKeyDown)(JNIEnv *, jobject, jint);
    void (*nativeKeyboardEvent)(JNIEnv *, jobject, jint, jboolean, jboolean, jboolean, jboolean, jboolean, jdouble);
    void (*nativeInsertText)(JNIEnv *, jobject, jstring);
    void (*nativeDeleteBackward)(JNIEnv *, jobject);
    jstring (*nativeGetContentText)(JNIEnv *, jobject);
    void (*nativeTextClosed)(JNIEnv *, jobject);
    void (*textInputDone)(JNIEnv *, jclass);
    void *(*CCApplication_sharedApplication)(void);
    void (*AppDelegate_applicationWillTerminate)(void *);
    void (*nativeScroll)(JNIEnv *, jobject, jfloat, jfloat, jfloat, jfloat);
    void (*nativeInitBitmapDC)(JNIEnv *, jclass, jint, jint, jbyteArray);
    // cocos2d internals used for controller support
    void *(*CCDirector_sharedDirector)(void);
    void *(*CCDirector_getKeyboardDispatcher)(void *);
    bool (*CCKeyboardDispatcher_dispatchKeyboardMSG)(void *, int key, bool down, bool repeat, double t);
    void *(*CCDirector_getRunningScene)(void *);
} GameFuncs;
extern GameFuncs g_game;

// fmod JNI
extern jint (*g_fmod_JNI_OnLoad)(JavaVM *, void *);

// ---------------------------------------------------------------- input
void input_init(void);
void input_update(void);        // call on the GL thread before nativeRender
void input_draw_overlay(void);  // call after nativeRender
void input_screen_resized(int old_w, int old_h); // after g_screen_w/h change
void ime_run_if_requested(void);

// ---------------------------------------------------------------- audio (AAudio)
uintptr_t android_lookup(const char *lib, const char *name);
void audio_init(void);
void audio_shutdown(void);

// shutdown
int threads_exit_all(uint64_t timeout_ns);
void bionic_thread_checkpoint(void);
bool so_unload_all(void);
extern int g_ime_flags;

// -uhd icon cache (uhd_pack.c)
void uhd_pack_start(const char *root, char **names, size_t count);
bool uhd_pack_get(const char *rel, const void **data, uint32_t *size);
void uhd_pack_stop(void);
bool uhd_pack_building(void);      // the cache is being built (first launch)
bool uhd_pack_finished(void);      // loaded, or built and saved
void uhd_pack_progress(size_t *done, size_t *total, bool *writing);
bool loadscreen_run(void);         // shows build progress; false = app closing
void display_size(int *w, int *h); // current EGL surface size
void display_present(void);        // eglSwapBuffers

// frame timing (perf.c)
void perf_init_main(void);   // call on the game thread
bool perf_on_main(void);
void perf_add_wait(uint64_t ns);   // game thread blocked on another thread
void perf_add_java(uint64_t ns);   // a (fake) Java method
void perf_add_text(uint64_t ns);   // a CCLabelTTF bitmap
void perf_frame_begin(void);       // start of input + GD's update
bool perf_in_logic(void);          // the game thread is in input + GD's update
Handle perf_main_handle(void);

// sampling profiler (prof.c)
void prof_start(void);
void prof_stop(void);
bool so_function_at(uintptr_t addr, const char **module, const char **name, uintptr_t *start);
void perf_add_file(uint64_t ns, uint64_t bytes);
void perf_frame(uint64_t t_start, uint64_t t_game, uint64_t t_swap_start, uint64_t t_end);

// core layout (cores.c)
void cores_init(void);
int cores_main(void);
u64 cores_worker_mask(void);

// time
uint64_t time_ns(void);

// bionic TLS (TPIDR_EL0) for threads that run Android code
void bionic_tls_setup_thread(void);
bool bionic_tls_ready(void);
