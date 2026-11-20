// The Java half of the Android game, reimplemented: Cocos2dx helpers,
// RobTop's activity/crypto helpers and FMOD's Java class.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "port.h"

double g_animation_interval = 1.0 / 120.0;
bool g_ime_requested, g_ime_close_requested;
static char *g_clipboard;
static char g_user_id[64];

#define RET_VOID  { jvalue r; memset(&r, 0, sizeof(r)); return r; }
static jvalue v_int(jint i) { jvalue r; memset(&r, 0, sizeof(r)); r.i = i; return r; }
static jvalue v_bool(bool b) { jvalue r; memset(&r, 0, sizeof(r)); r.z = b; return r; }
static jvalue v_float(float f) { jvalue r; memset(&r, 0, sizeof(r)); r.f = f; return r; }
static jvalue v_double(double d) { jvalue r; memset(&r, 0, sizeof(r)); r.d = d; return r; }
static jvalue v_long(jlong l) { jvalue r; memset(&r, 0, sizeof(r)); r.j = l; return r; }
static jvalue v_obj(jobject o) { jvalue r; memset(&r, 0, sizeof(r)); r.l = o; return r; }
static jvalue v_str(const char *s) { return v_obj(jni_new_string(s)); }
#define ARGS (void)self; (void)args
#define A_STR(i) jni_string_utf(args[i].l)

// ------------------------------------------------------------------ prefs

typedef struct Pref { char *key, *value; struct Pref *next; } Pref;
static Pref *g_prefs;
static Mutex g_prefs_lock;
static bool g_prefs_dirty;

static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s == '=' ? '=' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void write_escaped(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') fputs("\\n", f);
        else if (*s == '\r') fputs("\\r", f);
        else if (*s == '\\') fputs("\\\\", f);
        else if (*s == '=') fputs("\\=", f);
        else fputc(*s, f);
    }
}

void prefs_init(void)
{
    mutexInit(&g_prefs_lock);
    FILE *f = fopen(GD_PREFS, "rb");
    if (!f) return;
    char *line = malloc(1 << 20);
    while (fgets(line, 1 << 20, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *eq = NULL;
        for (char *p = line; *p; p++) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '=') { eq = p; break; }
        }
        if (!eq) continue;
        *eq = 0;
        unescape(line);
        unescape(eq + 1);
        prefs_set(line, eq + 1);
    }
    free(line);
    fclose(f);
    g_prefs_dirty = false;
}

const char *prefs_get(const char *key)
{
    for (Pref *p = g_prefs; p; p = p->next)
        if (!strcmp(p->key, key)) return p->value;
    return NULL;
}

void prefs_set(const char *key, const char *value)
{
    mutexLock(&g_prefs_lock);
    for (Pref *p = g_prefs; p; p = p->next) {
        if (!strcmp(p->key, key)) {
            if (strcmp(p->value, value)) { free(p->value); p->value = strdup(value); g_prefs_dirty = true; }
            mutexUnlock(&g_prefs_lock);
            return;
        }
    }
    Pref *p = calloc(1, sizeof(*p));
    p->key = strdup(key);
    p->value = strdup(value);
    p->next = g_prefs;
    g_prefs = p;
    g_prefs_dirty = true;
    mutexUnlock(&g_prefs_lock);
}

void prefs_remove(const char *key)
{
    mutexLock(&g_prefs_lock);
    for (Pref **pp = &g_prefs; *pp; pp = &(*pp)->next) {
        if (!strcmp((*pp)->key, key)) {
            Pref *p = *pp;
            *pp = p->next;
            free(p->key); free(p->value); free(p);
            g_prefs_dirty = true;
            break;
        }
    }
    mutexUnlock(&g_prefs_lock);
}

void prefs_flush(void)
{
    mutexLock(&g_prefs_lock);
    if (g_prefs_dirty) {
        FILE *f = fopen(GD_PREFS ".tmp", "wb");
        if (f) {
            for (Pref *p = g_prefs; p; p = p->next) {
                write_escaped(f, p->key);
                fputc('=', f);
                write_escaped(f, p->value);
                fputc('\n', f);
            }
            fclose(f);
            remove(GD_PREFS);
            rename(GD_PREFS ".tmp", GD_PREFS);
        }
        g_prefs_dirty = false;
    }
    mutexUnlock(&g_prefs_lock);
}

// ------------------------------------------------------------------ Cocos2dxHelper

static jvalue getBoolForKey(jobject self, const jvalue *args)
{ ARGS; const char *v = prefs_get(A_STR(0)); return v_bool(v ? atoi(v) != 0 : args[1].z); }
static jvalue getIntegerForKey(jobject self, const jvalue *args)
{ ARGS; const char *v = prefs_get(A_STR(0)); return v_int(v ? atoi(v) : args[1].i); }
static jvalue getFloatForKey(jobject self, const jvalue *args)
{ ARGS; const char *v = prefs_get(A_STR(0)); return v_float(v ? strtof(v, NULL) : args[1].f); }
static jvalue getDoubleForKey(jobject self, const jvalue *args)
{ ARGS; const char *v = prefs_get(A_STR(0)); return v_double(v ? strtod(v, NULL) : args[1].d); }
static jvalue getStringForKey(jobject self, const jvalue *args)
{ ARGS; const char *v = prefs_get(A_STR(0)); return v_str(v ? v : A_STR(1)); }
static jvalue setBoolForKey(jobject self, const jvalue *args)
{ ARGS; prefs_set(A_STR(0), args[1].z ? "1" : "0"); RET_VOID }
static jvalue setIntegerForKey(jobject self, const jvalue *args)
{ ARGS; char b[32]; snprintf(b, sizeof(b), "%d", args[1].i); prefs_set(A_STR(0), b); RET_VOID }
static jvalue setFloatForKey(jobject self, const jvalue *args)
{ ARGS; char b[64]; snprintf(b, sizeof(b), "%.9g", args[1].f); prefs_set(A_STR(0), b); RET_VOID }
static jvalue setDoubleForKey(jobject self, const jvalue *args)
{ ARGS; char b[64]; snprintf(b, sizeof(b), "%.17g", args[1].d); prefs_set(A_STR(0), b); RET_VOID }
static jvalue setStringForKey(jobject self, const jvalue *args)
{ ARGS; prefs_set(A_STR(0), A_STR(1)); RET_VOID }

static jvalue getCocos2dxPackageName(jobject self, const jvalue *args) { ARGS; return v_str(ANDROID_PACKAGE); }
static jvalue getCocos2dxWritablePath(jobject self, const jvalue *args) { ARGS; return v_str(ANDROID_FILES_DIR); }

static jvalue getCurrentLanguage(jobject self, const jvalue *args)
{
    ARGS;
    u64 code = 0;
    SetLanguage lang = SetLanguage_ENUS;
    if (R_SUCCEEDED(setGetSystemLanguage(&code)))
        setMakeLanguage(code, &lang);
    static const char *names[] = {
        [SetLanguage_JA] = "ja", [SetLanguage_ENUS] = "en", [SetLanguage_FR] = "fr",
        [SetLanguage_DE] = "de", [SetLanguage_IT] = "it", [SetLanguage_ES] = "es",
        [SetLanguage_ZHCN] = "zh", [SetLanguage_KO] = "ko", [SetLanguage_NL] = "nl",
        [SetLanguage_PT] = "pt", [SetLanguage_RU] = "ru", [SetLanguage_ZHTW] = "zh",
        [SetLanguage_ENGB] = "en", [SetLanguage_FRCA] = "fr", [SetLanguage_ES419] = "es",
        [SetLanguage_ZHHANS] = "zh", [SetLanguage_ZHHANT] = "zh", [SetLanguage_PTBR] = "pt",
    };
    const char *n = (lang >= 0 && (size_t)lang < sizeof(names) / sizeof(names[0]) && names[lang]) ? names[lang] : "en";
    return v_str(n);
}

static jvalue showDialog(jobject self, const jvalue *args)
{
    ARGS;
    LOG("Dialog: %s: %s\n", A_STR(0), A_STR(1));
    RET_VOID
}

static jvalue showEditTextDialog(jobject self, const jvalue *args)
{
    ARGS;
    LOG("showEditTextDialog(%s, %s)\n", A_STR(0), A_STR(1));
    g_ime_requested = true;
    RET_VOID
}

static jvalue terminateProcess(jobject self, const jvalue *args) { ARGS; g_running = false; RET_VOID }
static jvalue noop(jobject self, const jvalue *args) { ARGS; RET_VOID }
static jvalue ret_false(jobject self, const jvalue *args) { ARGS; return v_bool(false); }
static jvalue ret_true(jobject self, const jvalue *args) { ARGS; return v_bool(true); }
// Constant: GD lays out by design resolution, and 320 is what handheld uses.
static jvalue getDPI(jobject self, const jvalue *args) { ARGS; return v_int(320); }
static jvalue setAnimationInterval(jobject self, const jvalue *args)
{
    ARGS;
    g_animation_interval = args[0].d;
    LOG("setAnimationInterval(%f)\n", args[0].d);
    RET_VOID
}

static jvalue openURL(jobject self, const jvalue *args)
{
    ARGS;
    const char *url = A_STR(0);
    LOG("openURL(%s)\n", url);
    WebCommonConfig cfg;
    WebCommonReply reply;
    Result rc = webPageCreate(&cfg, url);
    if (R_SUCCEEDED(rc)) {
        webConfigSetWhitelist(&cfg, "^http*");
        rc = webConfigShow(&cfg, &reply);
    }
    if (R_FAILED(rc)) LOG("web applet failed: 0x%x\n", rc);
    RET_VOID
}

// Text bitmaps for CCLabelTTF
static jvalue createTextBitmapShadowStroke(jobject self, const jvalue *args)
{
    ARGS;
    text_create_bitmap(A_STR(0), A_STR(1), args[2].i, args[3].f, args[4].f, args[5].f,
                       args[6].i, args[7].i, args[8].i, args[13].z,
                       args[14].f, args[15].f, args[16].f, args[17].f);
    RET_VOID
}
static jvalue getFontSizeAccordingHeight(jobject self, const jvalue *args) { ARGS; return v_int(args[0].i); }
static jvalue getStringWithEllipsis(jobject self, const jvalue *args) { ARGS; return v_str(A_STR(0)); }

// IME
static jvalue openIMEKeyboard(jobject self, const jvalue *args) { ARGS; g_ime_requested = true; RET_VOID }
static jvalue closeIMEKeyboard(jobject self, const jvalue *args) { ARGS; g_ime_close_requested = true; RET_VOID }

// LocalStorage (WebSocket/JS bridge, unused by the game, backed by prefs)
static jvalue ls_setItem(jobject self, const jvalue *args)
{ ARGS; char k[512]; snprintf(k, sizeof(k), "ls:%s", A_STR(0)); prefs_set(k, A_STR(1)); RET_VOID }
static jvalue ls_getItem(jobject self, const jvalue *args)
{ ARGS; char k[512]; snprintf(k, sizeof(k), "ls:%s", A_STR(0)); const char *v = prefs_get(k); return v ? v_str(v) : v_obj(NULL); }
static jvalue ls_removeItem(jobject self, const jvalue *args)
{ ARGS; char k[512]; snprintf(k, sizeof(k), "ls:%s", A_STR(0)); prefs_remove(k); RET_VOID }

// android.os.SystemClock / class loading
static jvalue uptimeMillis(jobject self, const jvalue *args) { ARGS; return v_long((jlong)(time_ns() / 1000000)); }
static jvalue getClassLoader(jobject self, const jvalue *args)
{
    ARGS;
    static jobject loader;
    if (!loader) loader = (*g_env)->NewGlobalRef(g_env, jni_new_object("java/lang/ClassLoader"));
    return v_obj(loader);
}
static jvalue loadClass(jobject self, const jvalue *args) { ARGS; return v_obj(jni_find_class(A_STR(0))); }

// ------------------------------------------------------------------ BaseRobTopActivity

static jvalue isNetworkAvailable(jobject self, const jvalue *args)
{
    ARGS;
    NifmInternetConnectionStatus st;
    NifmInternetConnectionType type;
    u32 strength;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &st)))
        return v_bool(st == NifmInternetConnectionStatus_Connected);
    return v_bool(true);
}

static jvalue getUserID(jobject self, const jvalue *args) { ARGS; return v_str(g_user_id); }
static jvalue getDeviceRefreshRate(jobject self, const jvalue *args) { ARGS; return v_float((float)g_config.fps); }

static jvalue copyToClipboard(jobject self, const jvalue *args)
{
    ARGS;
    free(g_clipboard);
    g_clipboard = strdup(A_STR(0));
    RET_VOID
}
static jvalue getClipboardString(jobject self, const jvalue *args) { ARGS; return v_str(g_clipboard ? g_clipboard : ""); }

static jvalue loadingFinished(jobject self, const jvalue *args) { ARGS; LOG("loadingFinished\n"); cpu_boost_loading_done(); RET_VOID }

// GD's text fields (CCTextInputNode) open the keyboard through
// PlatformToolbox::setKeyboardState -> BaseRobTopActivity.setKeyboardState.
int g_ime_flags;
static jvalue robtop_setKeyboardState(jobject self, const jvalue *args)
{
    ARGS;
    LOG("setKeyboardState(%d)\n", args[0].z);
    if (args[0].z) g_ime_requested = true;
    else g_ime_close_requested = true;
    RET_VOID
}
static jvalue robtop_onToggleKeyboard(jobject self, const jvalue *args)
{
    ARGS;
    LOG("onToggleKeyboard\n");
    g_ime_requested = true;
    RET_VOID
}
static jvalue robtop_setKeyboardInputFlags(jobject self, const jvalue *args)
{
    ARGS;
    g_ime_flags = args[0].i;
    LOG("setKeyboardInputFlags(%#x)\n", args[0].i);
    RET_VOID
}

// SimpleCrypto: Android's encrypted local copies; stored as plain files here.
static void crypto_path(const char *name, char *out, size_t n)
{
    if (name[0] == '/') path_translate(name, out, n);
    else snprintf(out, n, GD_SAVE_DIR "/%s", name);
}

static jvalue saveAndEncryptStringToFile(jobject self, const jvalue *args)
{
    ARGS;
    char p[512];
    crypto_path(A_STR(1), p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (f) { const char *s = A_STR(0); fwrite(s, 1, strlen(s), f); fclose(f); }
    RET_VOID
}

static jvalue loadAndDecryptFileToString(jobject self, const jvalue *args)
{
    ARGS;
    char p[512];
    crypto_path(A_STR(0), p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) return v_str("");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    size_t got = fread(buf, 1, n, f);
    buf[got] = 0;
    fclose(f);
    jvalue r = v_str(buf);
    free(buf);
    return r;
}

// ------------------------------------------------------------------ FMOD's Java class

static jvalue fmod_checkInit(jobject self, const jvalue *args) { ARGS; return v_bool(true); }
static jvalue fmod_getOutputSampleRate(jobject self, const jvalue *args) { ARGS; return v_int(48000); }
static jvalue fmod_getOutputBlockSize(jobject self, const jvalue *args) { ARGS; return v_int(256); }
static jvalue fmod_getAudioDevices(jobject self, const jvalue *args)
{
    ARGS;
    return v_obj((*g_env)->NewObjectArray(g_env, 0, jni_find_class("android/media/AudioDeviceInfo"), NULL));
}

static jvalue fmod_getAssetManager(jobject self, const jvalue *args)
{
    ARGS;
    static jobject am;
    if (!am) am = (*g_env)->NewGlobalRef(g_env, jni_new_object("android/content/res/AssetManager"));
    return v_obj(am);
}

// ------------------------------------------------------------------ registration

void java_impl_init(void)
{
    // a stable per-console user id for the game's account code
    u64 id = 0;
    randomGet(&id, sizeof(id));
    snprintf(g_user_id, sizeof(g_user_id), "S%016lx", (unsigned long)(id ^ 0x5d1f2a3b4c6e7f80ULL));
    const char *saved = prefs_get("__ounce_user_id");
    if (saved) snprintf(g_user_id, sizeof(g_user_id), "%s", saved);
    else prefs_set("__ounce_user_id", g_user_id);

    const char *H = "org/cocos2dx/lib/Cocos2dxHelper";
    java_register(H, "getBoolForKey", NULL, getBoolForKey);
    java_register(H, "getIntegerForKey", NULL, getIntegerForKey);
    java_register(H, "getFloatForKey", NULL, getFloatForKey);
    java_register(H, "getDoubleForKey", NULL, getDoubleForKey);
    java_register(H, "getStringForKey", NULL, getStringForKey);
    java_register(H, "setBoolForKey", NULL, setBoolForKey);
    java_register(H, "setIntegerForKey", NULL, setIntegerForKey);
    java_register(H, "setFloatForKey", NULL, setFloatForKey);
    java_register(H, "setDoubleForKey", NULL, setDoubleForKey);
    java_register(H, "setStringForKey", NULL, setStringForKey);
    java_register(H, "getCocos2dxPackageName", NULL, getCocos2dxPackageName);
    java_register(H, "getCocos2dxWritablePath", NULL, getCocos2dxWritablePath);
    java_register(H, "getCurrentLanguage", NULL, getCurrentLanguage);
    java_register(H, "showDialog", NULL, showDialog);
    java_register(H, "showEditTextDialog", NULL, showEditTextDialog);
    java_register(H, "terminateProcess", NULL, terminateProcess);
    java_register(H, "enableAccelerometer", NULL, noop);
    java_register(H, "disableAccelerometer", NULL, noop);
    java_register(H, "setAccelerometerInterval", NULL, noop);

    java_register("org/cocos2dx/lib/Cocos2dxRenderer", "setAnimationInterval", NULL, setAnimationInterval);
    java_register("org/cocos2dx/lib/Cocos2dxActivity", "openURL", NULL, openURL);
    java_register("org/cocos2dx/lib/Cocos2dxGLSurfaceView", "openIMEKeyboard", NULL, openIMEKeyboard);
    java_register("org/cocos2dx/lib/Cocos2dxGLSurfaceView", "closeIMEKeyboard", NULL, closeIMEKeyboard);
    java_register("org/cocos2dx/lib/Cocos2dxGLSurfaceView", "getDPI", NULL, getDPI);
    java_register("*", "getDPI", NULL, getDPI);
    java_register("org/cocos2dx/lib/Cocos2dxBitmap", "createTextBitmapShadowStroke", NULL, createTextBitmapShadowStroke);
    java_register("org/cocos2dx/lib/Cocos2dxBitmap", "getFontSizeAccordingHeight", NULL, getFontSizeAccordingHeight);
    java_register("org/cocos2dx/lib/Cocos2dxBitmap", "getStringWithEllipsis", NULL, getStringWithEllipsis);
    java_register("org/cocos2dx/lib/Cocos2dxETCLoader", "loadTexture", NULL, ret_false);
    java_register("org/cocos2dx/lib/Cocos2dxLocalStorage", "init", NULL, ret_true);
    java_register("org/cocos2dx/lib/Cocos2dxLocalStorage", "destory", NULL, noop);
    java_register("org/cocos2dx/lib/Cocos2dxLocalStorage", "setItem", NULL, ls_setItem);
    java_register("org/cocos2dx/lib/Cocos2dxLocalStorage", "getItem", NULL, ls_getItem);
    java_register("org/cocos2dx/lib/Cocos2dxLocalStorage", "removeItem", NULL, ls_removeItem);
    java_register("android/os/SystemClock", "uptimeMillis", NULL, uptimeMillis);
    java_register("*", "getClassLoader", NULL, getClassLoader);
    java_register("*", "loadClass", NULL, loadClass);

    const char *R = "com/customRobTop/BaseRobTopActivity";
    java_register(R, "isNetworkAvailable", NULL, isNetworkAvailable);
    java_register(R, "getUserID", NULL, getUserID);
    java_register(R, "getDeviceRefreshRate", NULL, getDeviceRefreshRate);
    java_register(R, "copyToClipboard", NULL, copyToClipboard);
    java_register(R, "getClipboardString", NULL, getClipboardString);
    java_register(R, "loadingFinished", NULL, loadingFinished);
    java_register(R, "setKeyboardState", NULL, robtop_setKeyboardState);
    java_register(R, "onToggleKeyboard", NULL, robtop_onToggleKeyboard);
    java_register(R, "setKeyboardInputFlags", NULL, robtop_setKeyboardInputFlags);
    java_register(R, "openURL", NULL, openURL);
    java_register(R, "shouldResumeSound", NULL, ret_true);
    java_register(R, "gameServicesIsSignedIn", NULL, ret_false);
    java_register(R, "hasCachedInterstitial", NULL, ret_false);
    java_register(R, "hasCachedRewardedVideo", NULL, ret_false);
    static const char *robtop_noops[] = {
        "onNativePause", "onNativeResume", "onGameLaunch",
        "setBlockBackButton", "gameServicesSignIn", "gameServicesSignOut", "unlockAchievement",
        "showAchievements", "sendMail", "tryShowRateDialog", "openAppPage",
        "shareFiles", "pickImportFiles", "queueRefreshBanner", "enableBannerNoRefresh", "enableBanner",
        "disableBanner", "showInterstitial", "cacheInterstitial", "showRewardedVideo",
        "cacheRewardedVideo", "showAdDebug",
    };
    for (size_t i = 0; i < sizeof(robtop_noops) / sizeof(robtop_noops[0]); i++)
        java_register(R, robtop_noops[i], NULL, noop);

    java_register("com/customRobTop/SimpleCrypto", "saveAndEncryptStringToFile", NULL, saveAndEncryptStringToFile);
    java_register("com/customRobTop/SimpleCrypto", "loadAndDecryptFileToString", NULL, loadAndDecryptFileToString);

    const char *F = "org/fmod/FMOD";
    java_register(F, "checkInit", NULL, fmod_checkInit);
    java_register(F, "supportsAAudio", NULL, ret_true);
    java_register(F, "supportsLowLatency", NULL, ret_true);
    java_register(F, "isBluetoothOn", NULL, ret_false);
    java_register(F, "getOutputSampleRate", NULL, fmod_getOutputSampleRate);
    java_register(F, "getOutputBlockSize", NULL, fmod_getOutputBlockSize);
    java_register(F, "getAssetManager", NULL, fmod_getAssetManager);
    java_register(F, "getAudioDevices", NULL, fmod_getAudioDevices);
}
