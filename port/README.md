# OunceDash

A Geometry Dash loader for Nintendo Switch 2.

A native Switch 2 homebrew (kaiHEN + ounce devkit) that runs **your own copy**
of Geometry Dash for Android. The NRO contains only the loader. The game code
(`libcocos2dcpp.so`, `libfmod.so`) and every asset are read at runtime from
the APK you put on the SD card. Nothing copyrighted is built into
`OunceDash.nro`.

## What it does

- Loads the APK's arm64 libraries natively. Each ELF is wrapped as an NRO in
  memory and mapped executable by kaiHEN's `ldr:ro`, then relocated against a
  bionic→newlib/libnx compatibility layer (no emulation).
- Plays the role of the Android Java side: Cocos2dx helpers, RobTop's
  activity/crypto helpers, FMOD's Java class, JNI, AAssetManager (reads
  straight from the APK) and AAudio (mixed into one libnx audio-renderer
  voice on a dedicated high-priority thread).
- Fixed core layout (`source/cores.c`): core 0 runs the game thread (game
  logic and draw-call recording, which GD does together in one step). The
  next core runs the GL/Vulkan driver threads that turn those draw calls
  into GPU work. They're raised from libnx's default priority and kept off
  the game's core. The remaining cores run the game's own worker threads,
  and the audio mixer stays on the last core. Horizon doesn't time-slice
  equal-priority threads, and the game busy-waits on its workers.
- Native resolution: 1920×1080 handheld; docked uses the console's TV
  output resolution (up to 3840×2160). The render size follows every
  dock/undock, and the game relays itself out through its own
  `nativeOnSurfaceChanged`, as on an Android resize.
- 120 fps through `nwindowSetFrameRates` (120 Hz handheld screen or a
  120 Hz TV; 60 fps on 60 Hz displays), with VRR where supported.
- Controller support through the game's own controller code:
  `PlatformToolbox::isControllerConnected` is patched to return true, so the
  game shows its controller glyphs.
- Touchscreen (multi-touch), USB keyboard, and the system keyboard for text
  fields.
- Online features (level search, song search/download, accounts) over the
  Switch network stack. The game's built-in curl/OpenSSL do the HTTP(S).
  Links the game opens (such as Newgrounds song search) open in the system
  web browser.

## SD card layout

```
sdmc:/switch/OunceDash/
    OunceDash.nro      <- this loader (launch it from hbmenu)
    <anything>.apk     <- YOUR Geometry Dash APK (must contain lib/arm64-v8a)
    config.ini         <- settings, created on first run
    controls.ini       <- button mapping, created on first run
    save/              <- game saves (CCGameManager.dat, CCLocalLevels.dat, prefs)
    log.txt            <- loader log (check this first if something fails)
    cache/uhd-pack.bin <- the sharp icon files packed into one file (see below)
```

**Sharp textures (optional, recommended):** the Android APK only has `-hd`
textures, which look soft at 1080p and above. If you also own the PC
version, copy its `Resources` folder to `sdmc:/switch/OunceDash/Resources/`.
The loader then uses its `-uhd` textures (with Texture Quality set to
Medium or High), and everything else still comes from the APK. Use the PC
release of the same game build: GD 2.2.145 for Android and the matching PC
version both report build 51 (`binaryVersion`) to the servers, and every
frame of the APK's `-hd` sprite sheets is in the PC `-uhd` sheets.
Only the `-uhd` files in that folder are used.

The PC version keeps its icons in one folder of over 7000 files, and on the
SD card's FAT filesystem opening a file by name scans that folder (~18 ms
per file; an icon page opens ~70). So the loader lists the `-uhd` files once
at startup and answers the game's "does this file exist" checks from that
list. It also keeps every `-uhd` file in `Resources`' subfolders (mainly
`icons/`) in memory and serves the game from there. On the first launch
(or after `Resources` changes) a progress screen shows while they are read
one by one and saved to `cache/uhd-pack.bin`, about 30 s, before the game
starts. Later launches load that one file in the background in well under
a second. Delete `cache/` after replacing
`Resources` with different files of the same names.

Start it through kaiHEN: launch a game holding **R** to get hbmenu, then
open `OunceDash.nro`. The loader needs kaiHEN's `ldr:ro` to map code.

To bring an existing save over from Android, copy `CCGameManager.dat` and
`CCLocalLevels.dat` into `save/`. Downloaded levels are stored in
`CCGameManager.dat` and created levels in `CCLocalLevels.dat`. Downloaded
songs and SFX go in `save/` too (`<id>.mp3`, `sfx/`, `music/`), as on
Android.

**Show FPS:** press **−** on any controller to toggle a frame-rate counter
in the top-left corner. The setting is remembered (`save/prefs.ini`).

**Quit** in the main menu saves, stops audio, makes every game thread end
itself (Horizon can't end another thread, so threads are woken and exit at
their next blocking call), unmaps the game and returns to hbmenu. If a
thread won't end within 3 seconds, the log names it and the program closes
to the HOME menu instead. Set `quit_action=home` to always close to HOME.
Tapping a text field (search, comments, level names) opens the system
keyboard; confirming it submits the field like Android's Done key.

## Controls

| Switch | In a level | In menus |
| --- | --- | --- |
| A | Jump (`Controller_A`) | Click at the cursor |
| R / ZR | Jump (`Controller_RB`) | ZR: click at the cursor |
| L / ZL | Player 2 jump (`Controller_LB`; also jumps in normal levels) | — |
| B | `Controller_B` | Back |
| + | Pause (`Controller_Start`) | — |
| − | Toggle the FPS counter | Toggle the FPS counter |
| D-pad / left stick | Platformer movement (`Controller_Left`/`Right`, left-stick codes) | D-pad: game navigation; stick: cursor |
| Right stick | Player 2 movement (right-stick codes) | Cursor |
| X / Y | `Controller_X` / `Controller_Y` | same |
| Second controller | Player 2: A/R/ZR jump, D-pad/stick move (second-controller codes) | — |
| Touchscreen | Works everywhere, as on Android | |

These match GD 2.2's default controller keybinds (`GJKeybindManager::loadDefaults`):
player 1 jumps on A and RB, player 2 on LB and the second controller's A,
and nothing is bound to LT/RT.

These are the defaults. Every button can be remapped in `controls.ini`
(Switch button = up to four actions: the game's `CTRL_*` controller buttons,
keyboard keys like `KEY_SPACE`, or the loader's `CLICK`, `BACK` and `FPS`).
The file lists every name it accepts; delete it to get the defaults back.
Sections are `[player1]` and `[player2]` (the second controller); a button
left out of a section does nothing, and `;` or `#` starts a comment. Unknown
names are reported in `log.txt` (`controls.ini:<line>: ...`). Several
buttons can share an action: it stays held until all of them are released.
The game's keybind settings apply to the controller keys.

Example: jump on B as well, and swap the FPS counter onto Y:

```ini
[player1]
B = CTRL_A, BACK
Y = FPS
MINUS = NONE
```

## Other game versions

Bug-fix updates of 2.2 will most likely load as they are: drop the new APK
in instead of the old one. The loader finds the game's functions by their
exported names, and the few code patches it makes (the OpenSSL timer, the
controller glyphs, the -uhd content scale, the file hooks) find their spot by
scanning the named function for the instruction they expect. A patch that
can't find its spot is skipped and logged (`patch ... skipped`) rather than
applied in the wrong place, so a changed build loses that one feature
instead of crashing. Things that may still need updates for a new version:
new Android/Java functions the game calls (logged as unresolved imports or
missing Java methods) and the `CCDirector` field the controller code reads
to tell a level from a menu. Bigger updates (2.3) will need testing.

## config.ini

| key | default | meaning |
| --- | --- | --- |
| `max_fps` | 120 | frame-rate cap (`fps` also works). 120 needs the handheld screen or a 120 Hz TV; 60 Hz displays cap at 60. 60/40/30 pace evenly |
| `width`, `height` | 0 | render size; 0 = native for handheld/docked |
| `vrr` | 1 | variable refresh rate |
| `controller_glyphs` | 1 | report a connected controller to the game |
| `cursor_speed` | 1.1 | menu cursor speed (screen heights per second) |
| `show_cursor` | 1 | show the virtual cursor |
| `apk` | (empty) | explicit APK path; empty = first `.apk` in the folder |
| `quit_action` | hbmenu | `hbmenu` = return to hbmenu, `home` = close to the HOME menu |
| `cpu_boost` | loading | the system's CPU boost mode during the loading screen (about a third faster); `off` disables it. It isn't used in levels: it also slows the GPU |
| `gl_buffer_ring` | 1 | give vertex buffers that the game rewrites every frame (particle systems) three GPU buffers in rotation, so writing one never has to ask whether the GPU is still drawing it |
| `profile` | 0 | `1` samples GD's update about 1000 times a second and logs the functions it spends its time in (`profile:` lines) |
| `trace_init` | 0 | log each of the game's static constructors (debugging) |

## Building

You need devkitPro with the ounce devkit packages (`libounce`, `ouncevk`,
`libkaihen`) from the `[ounce]` pacman repository, plus `switch-zlib`. The
packages this project was built against are libounce/ouncevk/libkaihen
20261117.1 and devkitA64 r30.

```bash
export DEVKITPRO=/opt/devkitpro
make
```

On Windows, run that from devkitPro's MSYS2 shell. To update the devkit
later, run `pacman -Syu` in devkitPro's MSYS2 (or `dkp-pacman -Syu` elsewhere).

## Source map

| file | role |
| --- | --- |
| `source/so_loader.c` | ELF → in-memory NRO → `ldr:ro`, relocation, symbols, `dl_iterate_phdr` |
| `source/imports.c` | every import of both libraries, plus `dlopen`/`dlsym` |
| `source/bionic_libc.c` | stdio/`__sF`, stat/dirent layouts, open flags, locale, clocks, mmap, path redirection |
| `source/bionic_pthread.c` | pthread/sem ABI on libnx primitives |
| `source/bionic_net.c` | Linux sockets on the BSD service (sockaddr, options, errno, getaddrinfo) |
| `source/jni.c` | the fake JavaVM/JNIEnv |
| `source/java_impl.c` | the Java methods the game and FMOD call, preferences |
| `source/android.c` | AAssetManager and AAudio |
| `source/text.c` | CCLabelTTF bitmaps from the system's shared fonts |
| `source/input.c` | controllers and `controls.ini`, cursor, touch, keyboard, swkbd |
| `source/main.c` | startup, EGL/display, lifecycle, crash log |
| `source/uhd.c` | -uhd textures from the PC `Resources` folder |
| `source/uhd_pack.c` | RAM cache of the `-uhd` files in `Resources`' subfolders (`cache/uhd-pack.bin`) |
| `source/loadscreen.c` | the progress screen while that cache is built on the first launch |
| `source/cores.c` | core layout: game thread, GL/Vulkan driver threads, game workers |
| `source/glbuf.c` | rotating per-frame vertex buffers (`gl_buffer_ring`) |
| `source/cpu.c` | CPU clock estimate and the loading-screen CPU boost |
| `source/perf.c` | frame timing for the log (`frames:`, `slow frame:`) |
| `source/prof.c` | sampling profiler (`profile=1`) |
| `source/fps.c` | the FPS counter |
| `tools/` | development helpers (`check_imports.py` verifies import coverage, `make_icon.py` draws the icon) |

## Status

Runs GD 2.2.145 on Switch 2 hardware: menus, levels, online search and song
downloads, -uhd textures, docked and handheld with resizing, controller,
touch and keyboard, and quitting back to hbmenu.

**Performance.** Menus and most levels run at 120 fps. Demanding levels
drop below it: measured with the frame log and the profiler, 85-90% of GD's
update in those levels is GD's own per-object work (moving objects,
visibility, rotation/transform triggers, colours), which takes 12-15 ms per
frame on Switch 2's CPU (~1.1 GHz in games). 120 fps needs 8.3 ms. The
loader keeps its own share small:

- the game thread has core 0 to itself, and the GL/Vulkan driver works on
  core 1;
- `-uhd` file checks are answered from memory and icons come from the RAM
  cache, so icon pages open in ~45 ms instead of over a second;
- per-frame vertex buffers rotate (`gl_buffer_ring`), so particle systems
  don't make the driver query the GPU through a service call each frame
  (that was 4-12% of GD's update in particle-heavy levels);
- the system's CPU boost (~1100 → ~1520 MHz) runs during the loading
  screen only, where it cuts the big texture loads by about a third. In
  levels it would also slow the GPU, which costs more than it gains.

Moving draw calls to another thread wouldn't help: drawing is 2-3 ms of a
frame, and the GPU itself is idle most of the frame.

Fixes made from hardware testing along the way:

- bionic's stack guard is read through `tpidr_el0` (slot 5), so every thread
  that runs game code gets a bionic-style TLS block;
- OpenSSL's CPU probe read `cntvct_el0`, which traps at EL0, so it's patched
  to read `cntpct_el0`;
- SDL's Switch audio is a process-wide singleton, so it was replaced with a
  single audren voice;
- Horizon doesn't time-slice equal-priority threads, so threads are spread
  across cores to avoid starvation.

**Debug aids.** `log.txt` is flushed on every line, `trace_init=1` logs each
static constructor, and a watchdog logs the main-loop phase if it stalls for
3 seconds. Every 1200 frames the log gives measured fps and a `frames:`
line: average and worst time in GD's update (`logic`), its draw-call
recording (`draw`) and `eglSwapBuffers` (`swap`), plus how long the game
thread waited on other threads, spent in Java methods and rendered text. A
frame over 1.5 refresh intervals gets a `slow frame:` line with the files
read, textures uploaded and shaders linked during it. `profile=1` adds
`profile:` blocks (see config.ini).
