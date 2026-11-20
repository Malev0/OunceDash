// CPU clock: measurement and the system's CPU boost mode.
//
// The system's boost mode (what games use for loading screens) raises the
// CPU from ~1100 to ~1520 MHz on Switch 2, but it also slows the GPU a lot
// (tested: frames waited 8.5-9.6 ms on the GPU in levels instead of 1-3 ms),
// so it only runs during the loading screen, where it shortens the big
// texture loads by about a third:
//   cpu_boost=loading  boost until the game's loading screen finishes (default)
//   cpu_boost=off
#include <stdio.h>
#include "port.h"

// Roughly one loop iteration per cycle on Cortex-A78-class cores
// (dependent subtract + taken branch), so iterations/us ~ MHz.
static unsigned estimate_mhz(void)
{
    uint64_t n = 20000000;
    uint64_t t = armGetSystemTick();
    __asm__ volatile("1: subs %0, %0, #1\n\tb.ne 1b" : "+r"(n) : : "cc");
    uint64_t ns = armTicksToNs(armGetSystemTick() - t);
    return ns ? (unsigned)(20000000ULL * 1000ULL / ns) : 0;
}

static bool g_boosted;

static void set_boost(bool on)
{
    Result rc = on ? appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad) : appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    g_boosted = on && R_SUCCEEDED(rc);
    svcSleepThread(30000000); // the clock changes shortly after the request
    LOG("cpu: boost %s (0x%x), ~%u MHz\n", on ? "on" : "off", rc, estimate_mhz());
}

void cpu_init(void)
{
    LOG("cpu: ~%u MHz at start (game thread core)\n", estimate_mhz());
    if (g_config.cpu_boost != CPU_BOOST_OFF) set_boost(true);
}

// The game's loading screen is done (RobTop's loadingFinished).
void cpu_boost_loading_done(void)
{
    if (g_boosted && g_config.cpu_boost == CPU_BOOST_LOADING) set_boost(false);
}
