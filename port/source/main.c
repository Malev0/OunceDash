// Geometry Dash for Nintendo Switch 2 - loader entry point.
//
// The NRO carries no game code or assets. At startup it opens the user's
// own Geometry Dash APK from the SD card, loads its arm64 libraries
// (libfmod.so, libcocos2dcpp.so) with the so-loader, and plays the part of
// the Android Java activity around them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "port.h"

GdConfig g_config = {
    .fps = 120, .vrr = true, .controller_glyphs = true, .cursor_speed = 1.1f, .show_cursor = true,
    .trace_init = false, .quit_to_hbmenu = true, .uhd = true, .cpu_boost = CPU_BOOST_LOADING, .gl_buffer_ring = true,
};
GameFuncs g_game;
jint (*g_fmod_JNI_OnLoad)(JavaVM *, void *);
volatile bool g_running = true;
int g_screen_w = 1920, g_screen_h = 1080;

static FILE *g_log;
static Mutex g_log_lock;
static EGLDisplay g_display;
static EGLSurface g_surface;
static EGLContext g_context;

// ------------------------------------------------------------------ logging

void log_init(void)
{
    mutexInit(&g_log_lock);
    g_log = fopen(GD_LOG_PATH, "w");
}

// Every line is flushed to the SD card immediately, so the log survives a
// crash at any point.
void log_printf(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    svcOutputDebugString(buf, n);
    if (!g_log) return;
    mutexLock(&g_log_lock);
    fwrite(buf, 1, n, g_log);
    fflush(g_log);
    mutexUnlock(&g_log_lock);
}

void log_flush(void)
{
    if (!g_log) return;
    mutexLock(&g_log_lock);
    fflush(g_log);
    mutexUnlock(&g_log_lock);
}

void fatal_error(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LOG("FATAL: %s\n", msg);
    log_flush();
    ErrorApplicationConfig cfg;
    if (R_SUCCEEDED(errorApplicationCreate(&cfg, "Geometry Dash loader error", msg))) {
        errorApplicationShow(&cfg);
    }
    exit(1);
}

// Crash reports name the game function that faulted.
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
u32 __nx_exception_ignoredebug = 1;

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    char pc[256], lr[256];
    so_describe_address(ctx->pc.x, pc, sizeof(pc));
    so_describe_address(ctx->lr.x, lr, sizeof(lr));
    LOG("\n*** CRASH: error_desc %#x\n  pc  %s\n  lr  %s\n  far %#lx sp %#lx\n", ctx->error_desc,
        pc, lr, (unsigned long)ctx->far.x, (unsigned long)ctx->sp.x);
    for (int i = 0; i < 29; i++) {
        char b[256];
        LOG("  x%-2d %#018lx %s\n", i, (unsigned long)ctx->cpu_gprs[i].x,
            so_describe_address(ctx->cpu_gprs[i].x, b, sizeof(b)));
    }
    // a short stack scan for return addresses
    uint64_t *sp = (uint64_t *)ctx->sp.x;
    for (int i = 0; i < 64; i++) {
        char b[256];
        so_describe_address(sp[i], b, sizeof(b));
        if (strchr(b, '+')) LOG("  stack[%d] %s\n", i, b);
    }
    if (g_log) fflush(g_log);
    char msg[600];
    snprintf(msg, sizeof(msg), "The game crashed.\nPC: %s\nLR: %s\nSee %s", pc, lr, GD_LOG_PATH);
    ErrorApplicationConfig cfg;
    if (R_SUCCEEDED(errorApplicationCreate(&cfg, "OunceDash crashed", msg)))
        errorApplicationShow(&cfg);
    svcExitProcess();
}

// ------------------------------------------------------------------ config

static bool cfg_bool(const char *v) { return !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes"); }

void config_load(void)
{
    FILE *f = fopen(GD_CONFIG, "r");
    if (!f) {
        f = fopen(GD_CONFIG, "w");
        if (f) {
            fputs("; OunceDash settings (button mapping is in controls.ini)\n"
                  "; max_fps: frame-rate cap. 120 needs the handheld screen or a 120 Hz TV;\n"
                  ";   60 Hz displays run at up to 60. Divisors of 120 (60, 40, 30) pace evenly.\n"
                  "max_fps=120\n"
                  "; width/height: render size, 0 = native for handheld/docked\n"
                  "width=0\nheight=0\n"
                  "; vrr: variable refresh rate where the display supports it\n"
                  "vrr=1\n"
                  "; controller_glyphs: let the game know a controller is connected\n"
                  "controller_glyphs=1\n"
                  "; cursor_speed: menu cursor speed (screen heights per second)\n"
                  "cursor_speed=1.1\n"
                  "show_cursor=1\n"
                  "; cpu_boost: loading = boost the CPU during the loading screen, off = never\n"
                  "cpu_boost=loading\n"
                  "; apk: path to your Geometry Dash APK (empty = first .apk in this folder)\n"
                  "apk=\n", f);
            fclose(f);
        }
        return;
    }
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (line[0] == ';' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "max_fps") || !strcmp(k, "fps")) g_config.fps = atoi(v);
        else if (!strcmp(k, "width")) g_config.width = atoi(v);
        else if (!strcmp(k, "height")) g_config.height = atoi(v);
        else if (!strcmp(k, "vrr")) g_config.vrr = cfg_bool(v);
        else if (!strcmp(k, "controller_glyphs")) g_config.controller_glyphs = cfg_bool(v);
        else if (!strcmp(k, "cursor_speed")) g_config.cursor_speed = strtof(v, NULL);
        else if (!strcmp(k, "show_cursor")) g_config.show_cursor = cfg_bool(v);
        else if (!strcmp(k, "trace_init")) g_config.trace_init = cfg_bool(v);
        else if (!strcmp(k, "uhd")) g_config.uhd = cfg_bool(v);
        else if (!strcmp(k, "quit_action")) g_config.quit_to_hbmenu = strcmp(v, "home") != 0;
        else if (!strcmp(k, "profile")) g_config.profile = cfg_bool(v);
        else if (!strcmp(k, "gl_buffer_ring")) g_config.gl_buffer_ring = cfg_bool(v);
        else if (!strcmp(k, "cpu_boost"))
            // ("always" from an earlier build: the boost also slows the GPU)
            g_config.cpu_boost = !strcmp(v, "off") || !strcmp(v, "0") ? CPU_BOOST_OFF : CPU_BOOST_LOADING;
        else if (!strcmp(k, "apk") && v[0]) snprintf(g_config.apk_path, sizeof(g_config.apk_path), "%s", v);
    }
    fclose(f);
    if (g_config.fps <= 0 || g_config.fps > 240) g_config.fps = 120;
    if (g_config.cursor_speed <= 0) g_config.cursor_speed = 1.1f;
}

static bool find_apk(void)
{
    struct stat st;
    if (g_config.apk_path[0] && stat(g_config.apk_path, &st) == 0) return true;
    DIR *d = opendir(GD_ROOT);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".apk")) {
            snprintf(g_config.apk_path, sizeof(g_config.apk_path), GD_ROOT "/%s", e->d_name);
            closedir(d);
            return true;
        }
    }
    closedir(d);
    return false;
}

// ------------------------------------------------------------------ loading

static so_module *load_library(const char *apk_name, const char *soname, const so_patch *patches, size_t np)
{
    const ZipEntry *e = zip_find(g_apk, apk_name);
    if (!e) fatal_error("%s is missing from the APK.\nUse an APK with arm64-v8a libraries.", apk_name);
    size_t size;
    void *elf = zip_read(g_apk, e, &size);
    if (!elf) fatal_error("Could not read %s from the APK.", apk_name);
    so_module *m = so_load(soname, elf, size, patches, np);
    free(elf);
    if (!m) fatal_error("Could not load %s.", soname);
    return m;
}

#define SYM(field, name) do { \
        g_game.field = (__typeof__(g_game.field))so_symbol(gd, name); \
        if (!g_game.field) LOG("missing game symbol %s\n", name); \
    } while (0)

static void load_game(void)
{
    so_module *fmod = load_library("lib/arm64-v8a/libfmod.so", "libfmod.so", NULL, 0);
    so_relocate(fmod, imports_resolve);

    // PlatformToolbox::isControllerConnected() -> true, so the game shows
    // its controller UI and button prompts.
    static const uint32_t ret_true[] = { 0x52800020 /* mov w0, #1 */, 0xd65f03c0 /* ret */ };
    // OpenSSL's CPU probe reads CNTVCT_EL0, which traps at EL0 on Horizon
    // (Android turns the trap into SIGILL; here it kills the process).
    // Read the physical counter, which userland may access, instead.
    static const uint32_t tick_cntpct[] = { 0xd53be020 /* mrs x0, cntpct_el0 */, 0xd65f03c0 /* ret */ };
    // With -uhd textures available, Medium/High texture quality use content
    // scale 4 (the Android build caps it at 2): fmov s0, #2.0 -> #4.0.
    static const uint32_t scale4[] = { 0x1e221000 /* fmov s0, #4.0 */ };
    so_patch patches[3];
    size_t np = 0;
    patches[np++] = (so_patch){ "_armv7_tick", tick_cntpct, 2, 0, 0 };
    if (g_config.controller_glyphs)
        patches[np++] = (so_patch){ "_ZN15PlatformToolbox21isControllerConnectedEv", ret_true, 2, 0, 0 };
    if (uhd_detect())
        patches[np++] = (so_patch){ "_ZN7cocos2d10CCDirector18updateContentScaleENS_14TextureQualityE",
                                    scale4, 1, SO_PATCH_SCAN, 0x1e201000 /* fmov s0, #2.0 */ };
    uhd_prepare_hooks();
    so_module *gd = load_library("lib/arm64-v8a/libcocos2dcpp.so", "libcocos2dcpp.so", patches, np);
    so_relocate(gd, imports_resolve);
    uhd_install(gd);

    so_init(fmod);
    so_init(gd);

    g_fmod_JNI_OnLoad = (__typeof__(g_fmod_JNI_OnLoad))so_symbol(fmod, "JNI_OnLoad");
    SYM(JNI_OnLoad, "JNI_OnLoad");
    SYM(nativeSetApkPath, "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
    SYM(nativeInit, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
    SYM(nativeRender, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
    SYM(nativeOnSurfaceChanged, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnSurfaceChanged");
    SYM(nativeOnPause,"Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
    SYM(nativeOnResume, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
    SYM(nativeTouchesBegin, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
    SYM(nativeTouchesEnd, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
    SYM(nativeTouchesMove, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
    SYM(nativeTouchesCancel, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel");
    SYM(nativeKeyDown, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown");
    SYM(nativeKeyboardEvent, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyboardEvent");
    SYM(nativeInsertText, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInsertText");
    SYM(nativeDeleteBackward, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeDeleteBackward");
    SYM(nativeGetContentText, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeGetContentText");
    SYM(nativeTextClosed, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTextClosed");
    SYM(textInputDone, "Java_com_customRobTop_JniToCpp_textInputDone");
    SYM(CCApplication_sharedApplication, "_ZN7cocos2d13CCApplication17sharedApplicationEv");
    SYM(AppDelegate_applicationWillTerminate, "_ZN11AppDelegate24applicationWillTerminateEv");
    SYM(nativeScroll, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeScroll");
    SYM(nativeInitBitmapDC, "Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC");
    SYM(CCDirector_sharedDirector, "_ZN7cocos2d10CCDirector14sharedDirectorEv");
    SYM(CCDirector_getKeyboardDispatcher, "_ZN7cocos2d10CCDirector21getKeyboardDispatcherEv");
    SYM(CCKeyboardDispatcher_dispatchKeyboardMSG, "_ZN7cocos2d20CCKeyboardDispatcher19dispatchKeyboardMSGENS_12enumKeyCodesEbbd");
    if (!g_game.nativeInit || !g_game.nativeRender)
        fatal_error("This APK's libcocos2dcpp.so is not a supported Geometry Dash build.");
}

// ------------------------------------------------------------------ display

static void init_display(void)
{
    NWindow *win = nwindowGetDefault();
    int w = g_config.width, h = g_config.height;
    bool follow_mode = w <= 0 || h <= 0;
    if (follow_mode) {
        // libounce sizes the window for the current mode itself: 1920x1080
        // handheld, the TV output setting docked, and resizes it on every
        // dock/undock (zink then recreates the swapchain and the EGL surface
        // takes the new size; the main loop tells the game). The window has
        // to match the display: VI2 crops an image that is bigger than it.
        s32 dw = 0, dh = 0;
        appletGetDefaultDisplayResolution(&dw, &dh);
        u32 mw = 0, mh = 0;
#ifdef __SWITCH2__
        if (R_SUCCEEDED(nwindowSetModeDimensions(win, 0, 0, 0, 0)))
            nwindowGetDimensions(win, &mw, &mh);
#endif
        LOG("display: mode %s, AM default %dx%d, window default %ux%u\n",
            appletGetOperationMode() == AppletOperationMode_Handheld ? "handheld" : "docked", dw, dh, mw, mh);
        follow_mode = mw >= 640 && mh >= 360; // else no per-mode sizes: fixed below
        if (follow_mode) { w = mw; h = mh; }
        else if (appletGetOperationMode() == AppletOperationMode_Handheld || dw <= 0) { w = 1920; h = 1080; }
        else { w = dw; h = dh; }
    }
    g_screen_w = w;
    g_screen_h = h;
    if (!follow_mode) nwindowSetDimensions(win, w, h); // config.ini: one fixed size
#ifdef __SWITCH2__
    Result rc = nwindowSetFrameRates(win, g_config.fps, g_config.fps > 60 ? 60 : g_config.fps, g_config.vrr);
    if (R_FAILED(rc)) LOG("nwindowSetFrameRates: 0x%x\n", rc);
#endif

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g_display, NULL, NULL)) fatal_error("eglInitialize failed (0x%x)", eglGetError());
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint n = 0;
    if (!eglChooseConfig(g_display, attrs, &config, 1, &n) || n == 0)
        fatal_error("eglChooseConfig failed (0x%x)", eglGetError());
    g_surface = eglCreateWindowSurface(g_display, config, win, NULL);
    if (g_surface == EGL_NO_SURFACE) fatal_error("eglCreateWindowSurface failed (0x%x)", eglGetError());
    const EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE };
    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (g_context == EGL_NO_CONTEXT) fatal_error("eglCreateContext failed (0x%x)", eglGetError());
    eglMakeCurrent(g_display, g_surface, g_surface, g_context);
    eglSwapInterval(g_display, 1);

    // Render at the size the driver actually gave the surface: the game lays
    // its UI out for the size nativeInit receives, so a mismatch crops it.
    EGLint sw = 0, sh = 0;
    eglQuerySurface(g_display, g_surface, EGL_WIDTH, &sw);
    eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &sh);
    u32 nw = 0, nh = 0;
    nwindowGetDimensions(win, &nw, &nh);
    GLint vp[4] = {0};
    glGetIntegerv(GL_VIEWPORT, vp);
    LOG("display: requested %dx%d, window %ux%u, EGL surface %dx%d, viewport %dx%d\n",
        w, h, nw, nh, sw, sh, vp[2], vp[3]);
    if (sw > 0 && sh > 0 && (sw != w || sh != h)) {
        LOG("display: using the surface size %dx%d\n", sw, sh);
        g_screen_w = sw;
        g_screen_h = sh;
    }
    LOG("display %dx%d @ %d fps, GL: %s / %s\n", w, h, g_config.fps,
        (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
}

// After a dock/undock the window (and, from the next frame, the EGL surface)
// takes the new display size. Tell the game the way Android's
// onSurfaceChanged does, so it relays its view out for that size.
static void check_surface_size(jobject renderer)
{
    EGLint sw = 0, sh = 0;
    if (!eglQuerySurface(g_display, g_surface, EGL_WIDTH, &sw) ||
        !eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &sh) ||
        sw <= 0 || sh <= 0 || (sw == g_screen_w && sh == g_screen_h))
        return;
    int ow = g_screen_w, oh = g_screen_h;
    LOG("display: surface %dx%d -> %dx%d (%s)\n", ow, oh, sw, sh,
        appletGetOperationMode() == AppletOperationMode_Handheld ? "handheld" : "docked");
    g_screen_w = sw;
    g_screen_h = sh;
    if (g_game.nativeOnSurfaceChanged) g_game.nativeOnSurfaceChanged(g_env, renderer, sw, sh);
    input_screen_resized(ow, oh);
}

void display_size(int *w, int *h)
{
    EGLint sw = 0, sh = 0;
    eglQuerySurface(g_display, g_surface, EGL_WIDTH, &sw);
    eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &sh);
    *w = sw > 0 ? sw : g_screen_w;
    *h = sh > 0 ? sh : g_screen_h;
}

void display_present(void) { eglSwapBuffers(g_display, g_surface); }

// ------------------------------------------------------------------ watchdog

// Logs where the main loop is when it stops making progress (a hang leaves
// no crash report, so this is the only clue).
static volatile const char *g_phase = "startup";
static volatile uint64_t g_frame;
static Thread g_watchdog;

static void watchdog_main(void *arg)
{
    (void)arg;
    uint64_t last = 0, stalled_since = 0;
    bool reported = false;
    while (g_running) {
        svcSleepThread(1000000000LL);
        uint64_t f = g_frame;
        if (f != last) {
            if (reported) LOG("watchdog: main loop resumed (frame %lu)\n", (unsigned long)f);
            last = f;
            stalled_since = time_ns();
            reported = false;
            continue;
        }
        if (!reported && time_ns() - stalled_since > 3000000000ULL) {
            LOG("watchdog: main loop stalled for 3 s in phase '%s' at frame %lu\n",
                (const char *)g_phase, (unsigned long)f);
            reported = true;
        }
    }
}

int thread_core_mask(void);

static void start_watchdog(void)
{
    // Another core than the main thread and a higher priority than the
    // game, so a spinning main thread cannot starve it.
    int main_core = cores_main();
    u64 mask = cores_worker_mask();
    int core = main_core;
    for (int c = 63; c >= 0; c--)
        if ((mask >> c) & 1) { core = c; break; }
    Result rc = threadCreate(&g_watchdog, watchdog_main, NULL, NULL, 0x4000, 0x20, core);
    LOG("watchdog: core %d (main on %d): 0x%x\n", core, main_core, rc);
    if (R_SUCCEEDED(rc)) threadStart(&g_watchdog);
}

// ------------------------------------------------------------------ lifecycle

static AppletHookCookie g_applet_cookie;
static bool g_paused;

static void applet_hook(AppletHookType hook, void *param)
{
    (void)param;
    if (hook != AppletHookType_OnFocusState) return;
    bool focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (!focused && !g_paused && g_game.nativeOnPause) {
        g_paused = true;
        g_game.nativeOnPause(g_env, NULL);
        prefs_flush();
    } else if (focused && g_paused && g_game.nativeOnResume) {
        g_paused = false;
        g_game.nativeOnResume(g_env, NULL);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    bionic_tls_setup_thread();
    perf_init_main();
    mkdir_p(GD_SAVE_DIR);
    log_init();
    LOG("OunceDash for Switch 2 (build " __DATE__ " " __TIME__ ")\n");
    cores_init();
    config_load();
    cpu_init();
    LOG("[boot] config: fps=%d size=%dx%d vrr=%d glyphs=%d\n", g_config.fps, g_config.width,
        g_config.height, g_config.vrr, g_config.controller_glyphs);

    Result rc = socketInitializeDefault();
    LOG("[boot] socketInitialize: 0x%x\n", rc);
    rc = nifmInitialize(NifmServiceType_User);
    LOG("[boot] nifmInitialize: 0x%x\n", rc);
    rc = plInitialize(PlServiceType_User);
    LOG("[boot] plInitialize: 0x%x\n", rc);

    if (!find_apk())
        fatal_error("Geometry Dash APK not found.\nCopy your own APK to %s/ (any name ending in .apk).", GD_ROOT);
    LOG("[boot] APK: %s\n", g_config.apk_path);
    g_apk = zip_open(g_config.apk_path);
    if (!g_apk) fatal_error("Could not open %s as an APK.", g_config.apk_path);
    LOG("[boot] APK opened, %zu entries\n", zip_count(g_apk));

    jni_init();
    LOG("[boot] jni ready\n");
    prefs_init();
    java_impl_init();
    LOG("[boot] java glue ready\n");
    audio_init();
    LOG("[boot] audio ready\n");
    text_init();

    LOG("[boot] loading game libraries\n");
    load_game();
    LOG("[boot] game libraries loaded\n");
    init_display();
    // first launch with Resources: build the icon cache before the game starts
    if (!loadscreen_run()) g_running = false;
    LOG("[boot] display ready\n");
    input_init();
    LOG("[boot] input ready\n");
    appletHook(&g_applet_cookie, applet_hook, NULL);
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);

    // System.loadLibrary("fmod"), System.loadLibrary("cocos2dcpp")
    if (g_fmod_JNI_OnLoad) g_fmod_JNI_OnLoad(g_vm, NULL);
    LOG("[boot] fmod JNI_OnLoad done\n");
    g_game.JNI_OnLoad(g_vm, NULL);
    LOG("[boot] game JNI_OnLoad done\n");
    // Cocos2dxActivity.onCreate -> Cocos2dxHelper.init
    jclass helper = jni_find_class("org/cocos2dx/lib/Cocos2dxHelper");
    if (g_game.nativeSetApkPath) g_game.nativeSetApkPath(g_env, helper, jni_new_string(ANDROID_APK_PATH));
    // Cocos2dxRenderer.onSurfaceCreated
    jobject renderer = (*g_env)->NewGlobalRef(g_env, jni_new_object("org/cocos2dx/lib/Cocos2dxRenderer"));
    LOG("nativeInit(%d, %d)\n", g_screen_w, g_screen_h);
    log_flush();
    g_phase = "nativeInit";
    g_game.nativeInit(g_env, renderer, g_screen_w, g_screen_h);
    jni_release_locals();
    LOG("game initialised\n");
    log_flush();

    uint64_t last_flush = time_ns();
    start_watchdog();
    prof_start();
    while (g_running) {
        g_phase = "appletMainLoop";
        if (!appletMainLoop()) break;
        g_phase = "resize";
        check_surface_size(renderer);
        uint64_t t_start = time_ns();
        perf_frame_begin();
        g_phase = "input";
        input_update();
        g_phase = "nativeRender";
        g_game.nativeRender(g_env, renderer);
        uint64_t t_game = time_ns();
        g_phase = "overlay";
        fps_frame();
        fps_draw();
        input_draw_overlay();
        g_phase = "eglSwapBuffers";
        uint64_t t_swap = time_ns();
        eglSwapBuffers(g_display, g_surface);
        perf_frame(t_start, t_game, t_swap, time_ns());
        g_phase = "software keyboard (waiting for the player is normal)";
        ime_run_if_requested();
        jni_release_locals();
        g_frame++;
        static uint64_t fps_t0, fps_f0;
        if (g_frame == 1) { fps_t0 = time_ns(); fps_f0 = 1; LOG("frame 1\n"); }
        else if (g_frame % 1200 == 0) {
            uint64_t t = time_ns();
            LOG("frame %lu, %.1f fps\n", (unsigned long)g_frame, (g_frame - fps_f0) * 1e9 / (double)(t - fps_t0));
            fps_t0 = t; fps_f0 = g_frame;
        }
        if (time_ns() - last_flush > 5000000000ULL) {
            prefs_flush();
            log_flush();
            last_flush = time_ns();
        }
    }

    // Quit (the game's Quit button ends in Cocos2dxHelper.terminateProcess)
    // or the system asked us to close. Save the way PC GD does when it quits.
    LOG("exiting (%s)\n", g_running ? "system request" : "game quit");
    g_running = false;
    uint64_t t_quit = time_ns();
#define STEP(what) LOG("shutdown: %s (%lu ms)\n", what, (unsigned long)((time_ns() - t_quit) / 1000000))
    if (g_game.CCApplication_sharedApplication && g_game.AppDelegate_applicationWillTerminate) {
        void *app = g_game.CCApplication_sharedApplication();
        if (app) {
            STEP("saving");
            g_game.AppDelegate_applicationWillTerminate(app);
            STEP("saved");
        }
    }
    prefs_flush();

    audio_shutdown();
    uhd_pack_stop();
    prof_stop();
    STEP("audio stopped");

    // To return to hbloader (and so to hbmenu) nothing of the game may be
    // left: its threads must end and its libraries be unmapped, or hbloader
    // cannot take its heap back. Game threads end themselves when woken
    // (Horizon cannot end another thread). If any refuse, close the program
    // to the HOME menu rather than crash hbloader.
    bool clean = g_config.quit_to_hbmenu;
    if (clean && threads_exit_all(3000000000ULL) != 0) clean = false;
    STEP("game threads done");
    threadWaitForExit(&g_watchdog);
    threadClose(&g_watchdog);
    STEP("watchdog stopped");
    if (clean && !so_unload_all()) clean = false;
    STEP("libraries unloaded");
    if (!clean) {
        LOG("shutdown: exiting to the HOME menu\n");
        log_flush();
        svcExitProcess();
    }

    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g_display, g_context);
    eglDestroySurface(g_display, g_surface);
    eglTerminate(g_display);
    STEP("GPU released");
    plExit();
    nifmExit();
    socketExit();
    STEP("returning to the launcher");
    if (g_log) { fclose(g_log); g_log = NULL; }
    return 0;
}
