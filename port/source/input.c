// Input: touchscreen, controllers, USB keyboard and the software keyboard.
//
// Controllers drive the game's own controller support: buttons are sent as
// cocos2d's Controller_* key codes through CCKeyboardDispatcher, which the
// game's keybind system (jump, pause, platformer movement...) understands.
// In menus a virtual cursor (stick + A) produces touches.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <GLES2/gl2.h>
#include "port.h"

// cocos2d enumKeyCodes
enum {
    KEY_Backspace = 0x08, KEY_Tab = 0x09, KEY_Enter = 0x0d, KEY_Escape = 0x1b, KEY_Space = 0x20,
    KEY_Left = 0x25, KEY_Up = 0x26, KEY_Right = 0x27, KEY_Down = 0x28,
    KEY_A = 0x41, KEY_D = 0x44, KEY_W = 0x57,
    CONTROLLER_A = 0x3e9, CONTROLLER_B = 0x3eb, CONTROLLER_Y = 0x3ed, CONTROLLER_X = 0x3ef,
    CONTROLLER_Start = 0x3f1, CONTROLLER_Back = 0x3f3, CONTROLLER_RB = 0x3f5, CONTROLLER_LB = 0x3f7,
    CONTROLLER_RT = 0x3f9, CONTROLLER_LT = 0x3fb, CONTROLLER_Up = 0x3fd, CONTROLLER_Down = 0x3ff,
    CONTROLLER_Left = 0x401, CONTROLLER_Right = 0x403,
};

#define ANDROID_KEYCODE_BACK 4
static void controls_load(void);
#define CURSOR_TOUCH_ID 9
#define MAX_TOUCH 10

static PadState g_pad[2];
static u64 g_prev_buttons[2];
static float g_cursor_x, g_cursor_y;
static bool g_cursor_visible;
static bool g_cursor_down;
static uint64_t g_last_cursor_move;
static uint64_t g_last_frame;

static struct { bool active; u32 finger; float x, y; } g_touch[MAX_TOUCH];
static int g_touch_w = 1280, g_touch_h = 720;

static void *g_keyboard_dispatcher;
static uint8_t g_kb_prev[32];

// ------------------------------------------------------------------ helpers

static double now_seconds(void) { return time_ns() / 1e9; }

static void send_key(int key, bool down)
{
    if (!g_game.CCKeyboardDispatcher_dispatchKeyboardMSG) return;
    if (!g_keyboard_dispatcher) {
        void *dir = g_game.CCDirector_sharedDirector();
        g_keyboard_dispatcher = g_game.CCDirector_getKeyboardDispatcher(dir);
        if (!g_keyboard_dispatcher) return;
    }
    g_game.CCKeyboardDispatcher_dispatchKeyboardMSG(g_keyboard_dispatcher, key, down, false, now_seconds());
}

static const char *rtti_name(void *obj)
{
    if (!obj) return "";
    void **vtable = *(void ***)obj;
    if (!vtable) return "";
    void *ti = vtable[-1];
    if (!ti) return "";
    const char *name = *(const char **)((uint8_t *)ti + 8);
    return name ? name : "";
}

// Is a level being played (PlayLayer on top of the running scene)?
static bool in_gameplay(void)
{
    void *dir = g_game.CCDirector_sharedDirector();
    if (!dir) return false;
    void *scene = *(void **)((uint8_t *)dir + 272); // CCDirector::m_pRunningScene
    if (!scene) return false;
    static void *(*getChildren)(void *);
    static unsigned (*count)(void *);
    static void *(*objectAtIndex)(void *, unsigned);
    if (!getChildren) {
        so_module *m = so_find("libcocos2dcpp.so");
        getChildren = (void *)so_symbol(m, "_ZN7cocos2d6CCNode11getChildrenEv");
        count = (void *)so_symbol(m, "_ZNK7cocos2d7CCArray5countEv");
        objectAtIndex = (void *)so_symbol(m, "_ZN7cocos2d7CCArray13objectAtIndexEj");
        if (!getChildren || !count || !objectAtIndex) return false;
    }
    void *children = getChildren(scene);
    if (!children) return false;
    unsigned n = count(children);
    if (!n) return false;
    // The top child must be the PlayLayer itself; pause menus and popups are
    // added above it.
    const char *top = rtti_name(objectAtIndex(children, n - 1));
    return strstr(top, "PlayLayer") != NULL && strstr(top, "Editor") == NULL;
}

static void to_game_coords(float tx, float ty, float *gx, float *gy)
{
    *gx = tx * g_screen_w / (float)g_touch_w;
    *gy = ty * g_screen_h / (float)g_touch_h;
}

static void touch_begin(int id, float x, float y)
{
    if (g_game.nativeTouchesBegin) g_game.nativeTouchesBegin(g_env, NULL, id, x, y);
}
static void touch_end(int id, float x, float y)
{
    if (g_game.nativeTouchesEnd) g_game.nativeTouchesEnd(g_env, NULL, id, x, y);
}
static void touch_move(int id, float x, float y)
{
    if (!g_game.nativeTouchesMove) return;
    jint ids[1] = { id };
    jfloat xs[1] = { x }, ys[1] = { y };
    g_game.nativeTouchesMove(g_env, NULL, jni_new_int_array(ids, 1), jni_new_float_array(xs, 1),
                             jni_new_float_array(ys, 1));
}

// ------------------------------------------------------------------ init

void input_init(void)
{
    padConfigureInput(2, HidNpadStyleSet_NpadStandard);
    padInitialize(&g_pad[0], HidNpadIdType_Handheld, HidNpadIdType_No1);
    padInitialize(&g_pad[1], HidNpadIdType_No2);
    hidInitializeTouchScreen();
    hidInitializeKeyboard();
    controls_load();
    g_cursor_x = g_screen_w * 0.5f;
    g_cursor_y = g_screen_h * 0.5f;
    g_last_frame = time_ns();
}

// Keeps the cursor at the same place on screen when the render size changes
// (docking or undocking).
void input_screen_resized(int old_w, int old_h)
{
    if (old_w > 0 && old_h > 0) {
        g_cursor_x = g_cursor_x * g_screen_w / (float)old_w;
        g_cursor_y = g_cursor_y * g_screen_h / (float)old_h;
    }
}

// ------------------------------------------------------------------ touchscreen

static void update_touch(void)
{
    HidTouchScreenState st = {0};
    if (!hidGetTouchScreenStates(&st, 1)) return;
    bool seen[MAX_TOUCH] = {0};
    for (int i = 0; i < st.count && i < 16; i++) {
        HidTouchState *t = &st.touches[i];
        if ((int)t->x > g_touch_w || (int)t->y > g_touch_h) { g_touch_w = 1920; g_touch_h = 1080; }
        float gx, gy;
        to_game_coords(t->x, t->y, &gx, &gy);
        int slot = -1;
        for (int k = 0; k < MAX_TOUCH; k++)
            if (g_touch[k].active && g_touch[k].finger == t->finger_id) { slot = k; break; }
        if (slot < 0) {
            for (int k = 0; k < MAX_TOUCH; k++)
                if (!g_touch[k].active && k != CURSOR_TOUCH_ID) { slot = k; break; }
            if (slot < 0) continue;
            g_touch[slot].active = true;
            g_touch[slot].finger = t->finger_id;
            g_touch[slot].x = gx; g_touch[slot].y = gy;
            touch_begin(slot, gx, gy);
        } else if (g_touch[slot].x != gx || g_touch[slot].y != gy) {
            g_touch[slot].x = gx; g_touch[slot].y = gy;
            touch_move(slot, gx, gy);
        }
        seen[slot] = true;
        g_cursor_visible = false;
    }
    for (int k = 0; k < MAX_TOUCH; k++) {
        if (g_touch[k].active && !seen[k]) {
            g_touch[k].active = false;
            touch_end(k, g_touch[k].x, g_touch[k].y);
        }
    }
}

// ------------------------------------------------------------------ controllers

// Button mapping, from controls.ini (written with the defaults on first run).
// Each Switch button maps to up to four actions: keys sent to the game's
// controller/keyboard handling, plus loader actions (cursor click and Android
// back in menus, the FPS counter).
#define MAX_ACTIONS 4
enum { ACT_CLICK = 1, ACT_BACK = 2, ACT_FPS = 4 };
typedef struct { u64 mask; int keys[MAX_ACTIONS]; int nkeys; int flags; } ButtonMap;
typedef struct { ButtonMap map[32]; int n; } PlayerMap;
static PlayerMap g_maps[2];
static u64 g_click_mask, g_back_mask, g_fps_mask[2];

static const struct { const char *name; u64 mask; } g_button_names[] = {
    { "A", HidNpadButton_A }, { "B", HidNpadButton_B }, { "X", HidNpadButton_X }, { "Y", HidNpadButton_Y },
    { "L", HidNpadButton_L }, { "R", HidNpadButton_R }, { "ZL", HidNpadButton_ZL }, { "ZR", HidNpadButton_ZR },
    { "PLUS", HidNpadButton_Plus }, { "MINUS", HidNpadButton_Minus },
    { "UP", HidNpadButton_Up }, { "DOWN", HidNpadButton_Down },
    { "LEFT", HidNpadButton_Left }, { "RIGHT", HidNpadButton_Right },
    { "LSTICK", HidNpadButton_StickL }, { "RSTICK", HidNpadButton_StickR },
    { "LSTICK_UP", HidNpadButton_StickLUp }, { "LSTICK_DOWN", HidNpadButton_StickLDown },
    { "LSTICK_LEFT", HidNpadButton_StickLLeft }, { "LSTICK_RIGHT", HidNpadButton_StickLRight },
    { "RSTICK_UP", HidNpadButton_StickRUp }, { "RSTICK_DOWN", HidNpadButton_StickRDown },
    { "RSTICK_LEFT", HidNpadButton_StickRLeft }, { "RSTICK_RIGHT", HidNpadButton_StickRRight },
};

static const struct { const char *name; int key; } g_action_names[] = {
    { "CTRL_A", CONTROLLER_A }, { "CTRL_B", CONTROLLER_B }, { "CTRL_X", CONTROLLER_X }, { "CTRL_Y", CONTROLLER_Y },
    { "CTRL_START", CONTROLLER_Start }, { "CTRL_BACK", CONTROLLER_Back },
    { "CTRL_LB", CONTROLLER_LB }, { "CTRL_RB", CONTROLLER_RB }, { "CTRL_LT", CONTROLLER_LT }, { "CTRL_RT", CONTROLLER_RT },
    { "CTRL_UP", CONTROLLER_Up }, { "CTRL_DOWN", CONTROLLER_Down },
    { "CTRL_LEFT", CONTROLLER_Left }, { "CTRL_RIGHT", CONTROLLER_Right },
    // codes GD 2.2's default keybinds use (GJKeybindManager::loadDefaults):
    // stick directions, and the second controller's (code + 1)
    { "CTRL_LSTICK_LEFT", 0x409 }, { "CTRL_LSTICK_RIGHT", 0x40b },
    { "CTRL_RSTICK_LEFT", 0x411 }, { "CTRL_RSTICK_RIGHT", 0x413 },
    { "CTRL2_A", 0x3ea }, { "CTRL2_LEFT", 0x402 }, { "CTRL2_RIGHT", 0x404 },
    { "CTRL2_LSTICK_LEFT", 0x40a }, { "CTRL2_LSTICK_RIGHT", 0x40c },
    { "KEY_SPACE", KEY_Space }, { "KEY_ENTER", KEY_Enter }, { "KEY_ESCAPE", KEY_Escape },
    { "KEY_TAB", KEY_Tab }, { "KEY_BACKSPACE", KEY_Backspace },
    { "KEY_UP", KEY_Up }, { "KEY_DOWN", KEY_Down }, { "KEY_LEFT", KEY_Left }, { "KEY_RIGHT", KEY_Right },
    { "KEY_W", KEY_W }, { "KEY_A", KEY_A }, { "KEY_S", 0x53 }, { "KEY_D", KEY_D },
    { "KEY_R", 0x52 }, { "KEY_P", 0x50 }, { "KEY_Z", 0x5a }, { "KEY_X", 0x58 },
};

static const char g_default_controls[] =
    "; OunceDash button mapping. Delete this file to get the defaults back.\n"
    ";\n"
    "; <switch button> = <action>, <action>, ...   (up to 4; NONE = unmapped)\n"
    "; A button left out of a section does nothing. ; or # starts a comment.\n"
    "; Mistakes are reported in log.txt.\n"
    ";\n"
    "; Switch buttons: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT\n"
    ";   LSTICK RSTICK (stick presses), LSTICK_UP/_DOWN/_LEFT/_RIGHT,\n"
    ";   RSTICK_UP/_DOWN/_LEFT/_RIGHT (stick tilted past halfway)\n"
    ";\n"
    "; Actions:\n"
    ";   CTRL_A CTRL_B CTRL_X CTRL_Y CTRL_START CTRL_BACK CTRL_LB CTRL_RB CTRL_LT CTRL_RT\n"
    ";   CTRL_UP CTRL_DOWN CTRL_LEFT CTRL_RIGHT\n"
    ";   CTRL_LSTICK_LEFT CTRL_LSTICK_RIGHT CTRL_RSTICK_LEFT CTRL_RSTICK_RIGHT\n"
    ";   CTRL2_A, CTRL2_LEFT, ... (CTRL2_ + any CTRL_ name above)\n"
    ";       the game's controller buttons (CTRL2_ = a second controller).\n"
    ";       What they do is set by the game's keybinds. Its defaults:\n"
    ";         player 1 jump: CTRL_A, CTRL_RB\n"
    ";         player 2 jump: CTRL_LB, CTRL2_A\n"
    ";         player 1 left/right: CTRL_LEFT/RIGHT, CTRL_LSTICK_LEFT/RIGHT\n"
    ";         player 2 left/right: CTRL2_LEFT/RIGHT, CTRL2_LSTICK_LEFT/RIGHT,\n"
    ";                              CTRL_RSTICK_LEFT/RIGHT\n"
    ";         pause: CTRL_START. CTRL_LT/CTRL_RT are unbound by default.\n"
    ";       Outside 2-player mode, player 2's jump also jumps.\n"
    ";   KEY_SPACE KEY_ENTER KEY_ESCAPE KEY_TAB KEY_BACKSPACE KEY_UP KEY_DOWN\n"
    ";   KEY_LEFT KEY_RIGHT KEY_W KEY_A KEY_S KEY_D KEY_R KEY_P KEY_Z KEY_X\n"
    ";       keyboard keys (KEY_UP/LEFT/RIGHT are player 2's keys)\n"
    ";   CLICK   click at the cursor in menus (while the cursor is shown)\n"
    ";   BACK    Android's back key in menus (closes the top popup or page)\n"
    ";   FPS     toggle the frame-rate counter\n"
    ";\n"
    "; In menus, CLICK and BACK replace a button's game keys, and both sticks\n"
    "; move the cursor instead of sending their actions.\n"
    "\n"
    "[player1]\n"
    "A = CTRL_A, CLICK\n"
    "B = CTRL_B, BACK\n"
    "X = CTRL_X\n"
    "Y = CTRL_Y\n"
    "; R and ZR: player 1 jump. L and ZL: player 2 jump (2-player mode on one\n"
    "; controller; also jumps in normal levels).\n"
    "R = CTRL_RB\n"
    "ZR = CTRL_RB, CLICK\n"
    "L = CTRL_LB\n"
    "ZL = CTRL_LB\n"
    "PLUS = CTRL_START\n"
    "MINUS = FPS\n"
    "UP = CTRL_UP\n"
    "DOWN = CTRL_DOWN\n"
    "LEFT = CTRL_LEFT\n"
    "RIGHT = CTRL_RIGHT\n"
    "LSTICK_UP = CTRL_UP\n"
    "LSTICK_DOWN = CTRL_DOWN\n"
    "LSTICK_LEFT = CTRL_LSTICK_LEFT\n"
    "LSTICK_RIGHT = CTRL_LSTICK_RIGHT\n"
    "; right stick: player 2 movement in 2-player mode on one controller\n"
    "RSTICK_LEFT = CTRL_RSTICK_LEFT\n"
    "RSTICK_RIGHT = CTRL_RSTICK_RIGHT\n"
    "\n"
    "; A second controller plays player 2 in 2-player levels.\n"
    "[player2]\n"
    "A = CTRL2_A\n"
    "R = CTRL2_A\n"
    "ZR = CTRL2_A\n"
    "LEFT = CTRL2_LEFT\n"
    "RIGHT = CTRL2_RIGHT\n"
    "LSTICK_LEFT = CTRL2_LSTICK_LEFT\n"
    "LSTICK_RIGHT = CTRL2_LSTICK_RIGHT\n"
    "MINUS = FPS\n";

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static int action_key(const char *a)
{
    for (size_t i = 0; i < sizeof(g_action_names) / sizeof(g_action_names[0]); i++)
        if (!strcasecmp(a, g_action_names[i].name)) return g_action_names[i].key;
    // CTRL2_<x>: the second controller's code for any CTRL_<x> (code + 1)
    if (!strncasecmp(a, "CTRL2_", 6)) {
        char name[40];
        snprintf(name, sizeof(name), "CTRL_%s", a + 6);
        for (size_t i = 0; i < sizeof(g_action_names) / sizeof(g_action_names[0]); i++)
            if (!strcasecmp(name, g_action_names[i].name)) return g_action_names[i].key + 1;
    }
    return 0;
}

static void controls_parse_line(int player, int lineno, char *line)
{
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = 0;
    char *btn = trim(line), *acts = eq + 1;
    u64 mask = 0;
    for (size_t i = 0; i < sizeof(g_button_names) / sizeof(g_button_names[0]); i++)
        if (!strcasecmp(btn, g_button_names[i].name)) mask = g_button_names[i].mask;
    if (!mask) { LOG("controls.ini:%d: unknown button '%s'\n", lineno, btn); return; }
    PlayerMap *pm = &g_maps[player];
    if (pm->n >= 32) return;
    ButtonMap bm = { .mask = mask };
    char *save = NULL;
    for (char *tok = strtok_r(acts, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *a = trim(tok);
        if (!*a || !strcasecmp(a, "NONE")) continue;
        if (!strcasecmp(a, "CLICK")) { bm.flags |= ACT_CLICK; continue; }
        if (!strcasecmp(a, "BACK")) { bm.flags |= ACT_BACK; continue; }
        if (!strcasecmp(a, "FPS")) { bm.flags |= ACT_FPS; continue; }
        int key = action_key(a);
        if (!key) { LOG("controls.ini:%d: unknown action '%s'\n", lineno, a); continue; }
        if (bm.nkeys < MAX_ACTIONS) bm.keys[bm.nkeys++] = key;
    }
    pm->map[pm->n++] = bm;
    if (player == 0 && (bm.flags & ACT_CLICK)) g_click_mask |= mask;
    if (player == 0 && (bm.flags & ACT_BACK)) g_back_mask |= mask;
    if (bm.flags & ACT_FPS) g_fps_mask[player] |= mask;
}

static void controls_parse(char *text)
{
    memset(g_maps, 0, sizeof(g_maps));
    g_click_mask = g_back_mask = g_fps_mask[0] = g_fps_mask[1] = 0;
    int player = 0, lineno = 0;
    char *p = text;
    if (!strncmp(p, "\xEF\xBB\xBF", 3)) p += 3; // UTF-8 BOM (Notepad)
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        lineno++;
        char *c = strpbrk(p, ";#"); // comments, also after a mapping
        if (c) *c = 0;
        char *l = trim(p);
        if (*l == '[') {
            if (!strncasecmp(l, "[player1]", 9)) player = 0;
            else if (!strncasecmp(l, "[player2]", 9)) player = 1;
        } else if (*l) {
            controls_parse_line(player, lineno, l);
        }
        p = nl ? nl + 1 : NULL;
    }
}

static void controls_load(void)
{
    char *text = NULL;
    FILE *f = fopen(GD_CONTROLS, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n >= 0 && n < 1 << 20 && (text = malloc(n + 1)))
            text[fread(text, 1, n, f)] = 0;
        fclose(f);
        if (!text) LOG("controls: cannot read %s, using the defaults\n", GD_CONTROLS);
    } else if ((f = fopen(GD_CONTROLS, "wb"))) { // only when there is no file
        fputs(g_default_controls, f);
        fclose(f);
    }
    if (!text) text = strdup(g_default_controls);
    controls_parse(text);
    free(text);
    LOG("controls: %d mappings for player 1, %d for player 2\n", g_maps[0].n, g_maps[1].n);
}

// The game keys held down are worked out from the buttons each frame and
// compared with the keys last sent. So a key two buttons share (R and ZR)
// stays down until both are released, and a button masked off when a level
// ends (B held into a menu) still sends its release.
#define MAX_HELD 64
static int g_held[MAX_HELD], g_nheld;

static bool has_key(const int *keys, int n, int key)
{
    for (int i = 0; i < n; i++) if (keys[i] == key) return true;
    return false;
}

static int collect_keys(const PlayerMap *pm, u64 buttons, int *out, int n)
{
    for (int i = 0; i < pm->n; i++) {
        const ButtonMap *m = &pm->map[i];
        if (!(buttons & m->mask)) continue;
        for (int k = 0; k < m->nkeys; k++)
            if (n < MAX_HELD && !has_key(out, n, m->keys[k])) out[n++] = m->keys[k];
    }
    return n;
}

static void dispatch_keys(u64 b0, u64 b1)
{
    int want[MAX_HELD];
    int n = collect_keys(&g_maps[0], b0, want, 0);
    n = collect_keys(&g_maps[1], b1, want, n);
    for (int i = 0; i < g_nheld; i++)
        if (!has_key(want, n, g_held[i])) send_key(g_held[i], false);
    for (int i = 0; i < n; i++)
        if (!has_key(g_held, g_nheld, want[i])) send_key(want[i], true);
    memcpy(g_held, want, n * sizeof(int));
    g_nheld = n;
}

static void update_cursor(float dt, bool gameplay)
{
    // In a level both sticks are game input (player 1 / player 2 movement).
    if (gameplay) return;
    HidAnalogStickState l = padGetStickPos(&g_pad[0], 0);
    HidAnalogStickState r = padGetStickPos(&g_pad[0], 1);
    float dx = r.x / 32767.0f, dy = r.y / 32767.0f;
    // either stick moves the cursor in menus
    float lx = l.x / 32767.0f, ly = l.y / 32767.0f;
    if (fabsf(lx) > fabsf(dx)) dx = lx;
    if (fabsf(ly) > fabsf(dy)) dy = ly;
    float mag = sqrtf(dx * dx + dy * dy);
    if (mag > 0.15f) {
        float speed = g_config.cursor_speed * g_screen_h * (0.25f + 0.75f * mag * mag);
        g_cursor_x += dx / mag * mag * speed * dt;
        g_cursor_y -= dy / mag * mag * speed * dt;
        if (g_cursor_x < 0) g_cursor_x = 0;
        if (g_cursor_y < 0) g_cursor_y = 0;
        if (g_cursor_x > g_screen_w - 1) g_cursor_x = g_screen_w - 1;
        if (g_cursor_y > g_screen_h - 1) g_cursor_y = g_screen_h - 1;
        g_cursor_visible = g_config.show_cursor;
        g_last_cursor_move = time_ns();
        if (g_cursor_down) touch_move(CURSOR_TOUCH_ID, g_cursor_x, g_cursor_y);
    }
}

static void update_pads(float dt)
{
    padUpdate(&g_pad[0]);
    padUpdate(&g_pad[1]);
    u64 b0 = padGetButtons(&g_pad[0]);
    u64 b1 = padGetButtons(&g_pad[1]);
    bool gameplay = in_gameplay();

    update_cursor(dt, gameplay);
    if (gameplay && g_cursor_visible && !g_cursor_down) g_cursor_visible = false;

    // In menus the left stick drives the cursor, so do not also send it as
    // a direction; the D-pad still navigates.
    u64 suppress = 0;
    if (!gameplay)
        suppress = HidNpadButton_StickLUp | HidNpadButton_StickLDown | HidNpadButton_StickLLeft | HidNpadButton_StickLRight |
                   HidNpadButton_StickRUp | HidNpadButton_StickRDown | HidNpadButton_StickRLeft | HidNpadButton_StickRRight;
    u64 m0 = b0;
    if (!gameplay) m0 &= ~suppress;

    // BACK buttons are Android's back key in menus (closes the top layer once).
    if (!gameplay && (b0 & ~g_prev_buttons[0] & g_back_mask) && g_game.nativeKeyDown)
        g_game.nativeKeyDown(g_env, NULL, ANDROID_KEYCODE_BACK);
    if (!gameplay) m0 &= ~g_back_mask;
    // While the cursor is shown, CLICK buttons click instead of sending game keys.
    if (!gameplay && (g_cursor_visible || g_cursor_down)) m0 &= ~g_click_mask;

    if ((b0 & ~g_prev_buttons[0] & g_fps_mask[0]) | (b1 & ~g_prev_buttons[1] & g_fps_mask[1]))
        fps_toggle();

    dispatch_keys(m0, b1);

    // CLICK buttons click at the cursor while it is shown in menus.
    bool click = (b0 & g_click_mask) != 0;
    if (!gameplay && g_cursor_visible) {
        if (click && !g_cursor_down) {
            g_cursor_down = true;
            touch_begin(CURSOR_TOUCH_ID, g_cursor_x, g_cursor_y);
        }
    }
    if (!click && g_cursor_down) {
        g_cursor_down = false;
        touch_end(CURSOR_TOUCH_ID, g_cursor_x, g_cursor_y);
    }

    g_prev_buttons[0] = b0;
    g_prev_buttons[1] = b1;

    // hide an idle cursor after a while
    if (g_cursor_visible && !g_cursor_down && time_ns() - g_last_cursor_move > 6000000000ULL)
        g_cursor_visible = false;
}

// ------------------------------------------------------------------ USB keyboard

static int hid_to_android(int k)
{
    if (k >= HidKeyboardKey_A && k <= HidKeyboardKey_Z) return 29 + (k - HidKeyboardKey_A);
    if (k >= HidKeyboardKey_D1 && k <= HidKeyboardKey_D9) return 8 + (k - HidKeyboardKey_D1);
    switch (k) {
    case HidKeyboardKey_D0: return 7;
    case HidKeyboardKey_Return: return 66;
    case HidKeyboardKey_Escape: return 111;
    case HidKeyboardKey_Backspace: return 67;
    case HidKeyboardKey_Tab: return 61;
    case HidKeyboardKey_Space: return 62;
    case HidKeyboardKey_RightArrow: return 22;
    case HidKeyboardKey_LeftArrow: return 21;
    case HidKeyboardKey_DownArrow: return 20;
    case HidKeyboardKey_UpArrow: return 19;
    case HidKeyboardKey_Delete: return 112;
    case HidKeyboardKey_Minus: return 69;
    case HidKeyboardKey_Plus: return 70;
    case HidKeyboardKey_Period: return 56;
    case HidKeyboardKey_Comma: return 55;
    case HidKeyboardKey_Slash: return 76;
    case HidKeyboardKey_LeftShift: return 59;
    case HidKeyboardKey_RightShift: return 60;
    case HidKeyboardKey_LeftControl: return 113;
    case HidKeyboardKey_RightControl: return 114;
    case HidKeyboardKey_LeftAlt: return 57;
    case HidKeyboardKey_RightAlt: return 58;
    case HidKeyboardKey_Home: return 122;
    case HidKeyboardKey_End: return 123;
    case HidKeyboardKey_PageUp: return 92;
    case HidKeyboardKey_PageDown: return 93;
    default: return 0;
    }
}

static void update_keyboard(void)
{
    HidKeyboardState ks;
    if (!hidGetKeyboardStates(&ks, 1) || !g_game.nativeKeyboardEvent) return;
    bool shift = ks.modifiers & HidKeyboardModifier_Shift;
    bool ctrl = ks.modifiers & HidKeyboardModifier_Control;
    bool alt = ks.modifiers & (HidKeyboardModifier_LeftAlt | HidKeyboardModifier_RightAlt);
    for (int k = 0; k < 256; k++) {
        bool now = (ks.keys[k / 64] >> (k % 64)) & 1;
        bool prev = (g_kb_prev[k / 8] >> (k % 8)) & 1;
        if (now == prev) continue;
        if (now) g_kb_prev[k / 8] |= 1 << (k % 8);
        else g_kb_prev[k / 8] &= ~(1 << (k % 8));
        int ak = hid_to_android(k);
        if (!ak) continue;
        if (ak == 111 && now && g_game.nativeKeyDown) // Escape also acts as back
            g_game.nativeKeyDown(g_env, NULL, ANDROID_KEYCODE_BACK);
        g_game.nativeKeyboardEvent(g_env, NULL, ak, now, false, shift, ctrl, alt, now_seconds());
    }
}

void input_update(void)
{
    uint64_t t = time_ns();
    float dt = (t - g_last_frame) / 1e9f;
    if (dt > 0.1f) dt = 0.1f;
    g_last_frame = t;
    update_touch();
    update_pads(dt);
    update_keyboard();
}

// ------------------------------------------------------------------ software keyboard

static int utf8_len(const char *s)
{
    int n = 0;
    for (; *s; s++) if ((*s & 0xc0) != 0x80) n++;
    return n;
}

void ime_run_if_requested(void)
{
    static uint64_t last_closed;
    if (g_ime_close_requested) { g_ime_close_requested = false; g_ime_requested = false; }
    if (!g_ime_requested) return;
    g_ime_requested = false;
    // a request echoed back while the field still had focus would reopen it
    if (last_closed && time_ns() - last_closed < 500000000ULL) return;

    const char *current = "";
    if (g_game.nativeGetContentText) {
        jstring s = g_game.nativeGetContentText(g_env, NULL);
        current = jni_string_utf(s);
    }
    char *initial = strdup(current);

    SwkbdConfig kbd;
    char out[1024] = {0};
    Result rc = swkbdCreate(&kbd, 0);
    if (R_SUCCEEDED(rc)) {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetInitialText(&kbd, initial);
        swkbdConfigSetStringLenMax(&kbd, sizeof(out) / 4 - 1);
        rc = swkbdShow(&kbd, out, sizeof(out));
        swkbdClose(&kbd);
    }
    if (R_SUCCEEDED(rc)) {
        if (g_game.nativeDeleteBackward) {
            int n = utf8_len(initial);
            for (int i = 0; i < n; i++) g_game.nativeDeleteBackward(g_env, NULL);
        }
        if (g_game.nativeInsertText && out[0])
            g_game.nativeInsertText(g_env, NULL, jni_new_string(out));
        LOG("swkbd: entered %zu bytes\n", strlen(out));
    } else {
        LOG("swkbd: 0x%x (cancelled?)\n", rc);
    }
    // Android's keyboard closing ends in JniToCpp.textInputDone ->
    // CCTextInputNode::doneFromIME, which commits the field (and runs the
    // search). Older cocos2d text fields use nativeTextClosed.
    if (g_game.textInputDone) g_game.textInputDone(g_env, NULL);
    else if (g_game.nativeTextClosed) g_game.nativeTextClosed(g_env, NULL);
    g_ime_close_requested = false;
    g_ime_requested = false;
    last_closed = time_ns();
    free(initial);
}

// ------------------------------------------------------------------ cursor overlay

static GLuint g_cursor_prog;
static GLint g_cursor_pos_attr, g_cursor_color_uni;

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

static void init_cursor_program(void)
{
    const char *vs =
        "attribute vec2 a_pos;\n"
        "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    const char *fs =
        "precision mediump float;\n"
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = u_color; }\n";
    g_cursor_prog = glCreateProgram();
    glAttachShader(g_cursor_prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_cursor_prog, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(g_cursor_prog, 0, "a_pos");
    glLinkProgram(g_cursor_prog);
    g_cursor_pos_attr = 0;
    g_cursor_color_uni = glGetUniformLocation(g_cursor_prog, "u_color");
}

static void draw_disc(float cx, float cy, float radius, float r, float g, float b, float a)
{
    enum { SEG = 24 };
    GLfloat v[(SEG + 2) * 2];
    float sx = 2.0f / g_screen_w, sy = 2.0f / g_screen_h;
    v[0] = cx * sx - 1; v[1] = 1 - cy * sy;
    for (int i = 0; i <= SEG; i++) {
        float t = i * 6.2831853f / SEG;
        v[2 + i * 2] = (cx + cosf(t) * radius) * sx - 1;
        v[3 + i * 2] = 1 - (cy + sinf(t) * radius) * sy;
    }
    glUniform4f(g_cursor_color_uni, r, g, b, a);
    glVertexAttribPointer(g_cursor_pos_attr, 2, GL_FLOAT, GL_FALSE, 0, v);
    glDrawArrays(GL_TRIANGLE_FAN, 0, SEG + 2);
}

void input_draw_overlay(void)
{
    if (!g_cursor_visible) return;
    if (!g_cursor_prog) init_cursor_program();

    // Save the state cocos2d caches, draw, restore.
    GLint prog, buf, viewport[4], bsrc, bdst;
    GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean stencil = glIsEnabled(GL_STENCIL_TEST), depth = glIsEnabled(GL_DEPTH_TEST);
    GLint enabled[3];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
    for (int i = 0; i < 3; i++) glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);

    glViewport(0, 0, g_screen_w, g_screen_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(g_cursor_prog);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);

    float s = g_screen_h / 1080.0f;
    draw_disc(g_cursor_x, g_cursor_y, 15 * s, 0, 0, 0, 0.55f);
    draw_disc(g_cursor_x, g_cursor_y, 11 * s, 1, 1, 1, g_cursor_down ? 1.0f : 0.9f);
    if (g_cursor_down) draw_disc(g_cursor_x, g_cursor_y, 6 * s, 0.2f, 0.8f, 1.0f, 1.0f);

    for (int i = 0; i < 3; i++) {
        if (enabled[i]) glEnableVertexAttribArray(i);
        else glDisableVertexAttribArray(i);
    }
    glUseProgram(prog);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBlendFunc(bsrc, bdst);
    if (!blend) glDisable(GL_BLEND);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    if (depth) glEnable(GL_DEPTH_TEST);
}
