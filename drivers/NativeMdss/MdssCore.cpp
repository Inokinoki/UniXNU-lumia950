// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MdssCore.h"

#include "BoardConstants.h"

namespace mdss {

// ---------------------------------------------------------------------------
// The display's power domain
// ---------------------------------------------------------------------------

GdscState decodeGdscr(u32 gdscr)
{
    return (gdscr & kGdscrPowerOn) ? GdscState::On : GdscState::Collapsed;
}

bool gdscrCollapseRequested(u32 gdscr)
{
    return (gdscr & kGdscrSwCollapse) != 0;
}

bool gdscrUnderHardwareControl(u32 gdscr)
{
    return (gdscr & kGdscrHwControl) != 0;
}

u32 gdscrEnableValue(u32 current)
{
    return current & ~kGdscrSwCollapse;
}

// ---------------------------------------------------------------------------
// DSI link arithmetic
// ---------------------------------------------------------------------------

u32 hTotal(const PanelTiming &timing)
{
    return timing.hactive + timing.hSync + timing.hBackPorch + timing.hFrontPorch;
}

u32 vTotal(const PanelTiming &timing)
{
    return timing.vactive + timing.vSync + timing.vBackPorch + timing.vFrontPorch;
}

bool timingWellFormed(const PanelTiming &timing)
{
    // The visible area has to exist, every blanking interval has to be smaller
    // than what a 16-bit total could hold, and the totals have to stay inside a
    // 16-bit register: the controller programs them as 16-bit counts, so a
    // larger timing is a number the hardware cannot be told.
    if (timing.hactive == 0 || timing.vactive == 0) { return false; }
    const u32 h = hTotal(timing);
    const u32 v = vTotal(timing);
    return h > timing.hactive && v > timing.vactive && h <= 0xffffu && v <= 0xffffu;
}

u64 pixelClockHz(const PanelTiming &timing, u32 refreshHz)
{
    if (refreshHz == 0) { return 0; }
    return static_cast<u64>(hTotal(timing)) * vTotal(timing) * refreshHz;
}

bool byteClockFor(u32 pixelClock, u32 bitsPerPixel, u64 *byteClock)
{
    if (!byteClock) { return false; }
    *byteClock = 0;
    // DSI carries whole bytes. RGB888 (24) and RGB565 (16) do; a 18-bit format
    // sent as 24 is a different number than one sent as 18, so the caller has to
    // say which it means rather than have this round it.
    if (bitsPerPixel == 0 || (bitsPerPixel % 8) != 0) { return false; }
    *byteClock = static_cast<u64>(pixelClock) * (bitsPerPixel / 8);
    return true;
}

u64 bitClockHz(u64 byteClock, u32 lanes)
{
    if (lanes == 0) { return 0; }
    return (byteClock * 8) / lanes;
}

bool clocksAgree(u64 programmedHz, u64 expectedHz, u32 tolerancePpm)
{
    if (expectedHz == 0) { return false; }
    const u64 difference = programmedHz > expectedHz ? programmedHz - expectedHz
                                                     : expectedHz - programmedHz;
    // ppm of the expected clock, with 64-bit maths: a 1.4 GHz lane clock times
    // 2000 ppm still fits, and the product is taken after the division so the
    // multiply cannot overflow.
    const u64 allowed = (expectedHz / 1000000ULL) * tolerancePpm;
    return difference <= allowed || difference * 1000000ULL <= expectedHz * tolerancePpm;
}

// ---------------------------------------------------------------------------
// DSI enable bits
// ---------------------------------------------------------------------------

DsiMode decodeDsiMode(u32 dsiCtrl)
{
    const bool enabled = (dsiCtrl & kDsiCtrlEnable) != 0;
    if (!enabled) { return DsiMode::Off; }
    const bool video = (dsiCtrl & kDsiCtrlVideoMode) != 0;
    const bool command = (dsiCtrl & kDsiCtrlCommandMode) != 0;
    if (video && command) { return DsiMode::Both; }
    if (video) { return DsiMode::Video; }
    if (command) { return DsiMode::Command; }
    return DsiMode::Off;
}

const char *modeName(DsiMode mode)
{
    switch (mode) {
    case DsiMode::Off:     return "off";
    case DsiMode::Command: return "command mode";
    case DsiMode::Video:   return "video mode";
    case DsiMode::Both:    return "both modes set";
    }
    return "unknown";
}

bool windowAnswered(u32 value)
{
    return value != 0xffffffffu;
}

// ---------------------------------------------------------------------------
// The ladder
// ---------------------------------------------------------------------------

const char *rungName(Rung rung)
{
    switch (rung) {
    case Rung::Identity:    return "identity";
    case Rung::Power:       return "power";
    case Rung::Timings:     return "timings";
    case Rung::Scanout:     return "scanout";
    case Rung::Framebuffer: return "framebuffer";
    case Rung::Effects:     return "effects";
    }
    return "unknown";
}

} // namespace mdss
