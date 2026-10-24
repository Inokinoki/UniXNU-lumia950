// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Host-testable part of the Lumia 950 display driver: the power-domain decode,
// the DSI link arithmetic and the DSI enable-bit decode. Nothing here touches
// hardware, IOKit or XNU, so it compiles and runs on the build host
// (drivers/NativeMdss/Test/check-mdss-core.cpp).
//
// What this file is not: a panel driver. The panel's init sequence and the DSI
// PHY's PLL table are not in this repository and are not guessed at here. What
// is here is the arithmetic that decides whether a timing set can be driven at
// all, and the two register decodes a warm-handoff driver needs before it reads
// anything else.
#ifndef NATIVE_MDSS_CORE_H
#define NATIVE_MDSS_CORE_H

#include "MdssTypes.h"

namespace mdss {

// ---------------------------------------------------------------------------
// The display's power domain
//
// gdsc_mdss is the same kind of gate as the GPU's two OxiLi domains, at 0x2304
// in the MMSS clock controller instead of 0x4024 and 0x4034. PWR_ON is bit 31
// and SW_COLLAPSE is bit 0, as drivers/clk/qcom/gdsc.c defines them; a domain
// under hardware control (bit 1) must not be written, because the power
// controller owns the transition.
// ---------------------------------------------------------------------------

enum class GdscState {
    On,         // PWR_ON is set: the block is powered
    Collapsed,  // it is not: register reads in the block can hang the interconnect
};

GdscState decodeGdscr(u32 gdscr);
bool gdscrCollapseRequested(u32 gdscr);
bool gdscrUnderHardwareControl(u32 gdscr);

// The value to write to bring the domain up: the current register with only
// SW_COLLAPSE cleared, because every other bit is the firmware's business.
u32 gdscrEnableValue(u32 current);

// ---------------------------------------------------------------------------
// DSI link arithmetic
//
// A video-mode DSI link is not "whatever the panel says": the controller has to
// put hactive pixels per line inside htotal pixel clocks, the PHY has to clock
// out the resulting byte stream across `lanes` lanes, and every one of those
// numbers has to agree with what the PLL was programmed to produce. The three
// functions below are that arithmetic, and nothing else.
//
// A timing set is in pixels and lines, as a panel's data sheet states it:
//
//   |<-- hsync -->|<-- hback -->|<---- hactive ---->|<-- hfront -->|
//   |<------------------------------ htotal ---------------------->|
//
// and the frame is vtotal lines of that at `refreshHz`.
// ---------------------------------------------------------------------------

struct PanelTiming {
    u32 hactive;
    u32 hSync;
    u32 hBackPorch;
    u32 hFrontPorch;
    u32 vactive;
    u32 vSync;
    u32 vBackPorch;
    u32 vFrontPorch;
};

u32 hTotal(const PanelTiming &timing);
u32 vTotal(const PanelTiming &timing);

// A timing is usable when every visible pixel is inside the totals and the
// totals are inside what a 16-bit DSI register could hold. This is a sanity
// gate, not a data sheet: it cannot know that 1440x2560 is this panel's.
bool timingWellFormed(const PanelTiming &timing);

// Pixels per second: htotal x vtotal x refresh.
u64 pixelClockHz(const PanelTiming &timing, u32 refreshHz);

// Bytes per second on the link: the pixel clock times the bits per pixel,
// divided by eight. DSI sends whole bytes; a format whose depth is not a whole
// number of bytes is refused rather than rounded.
bool byteClockFor(u32 pixelClock, u32 bitsPerPixel, u64 *byteClock);

// Bits per second on each lane: the byte clock's eight bits split across the
// lanes that carry them.
u64 bitClockHz(u64 byteClock, u32 lanes);

// Whether a clock the hardware reports is the clock this arithmetic asks for,
// within a tolerance in parts per million. The PLL cannot land on every
// frequency exactly, so the comparison has to be a band, and the caller states
// the band rather than inheriting a number from here.
bool clocksAgree(u64 programmedHz, u64 expectedHz, u32 tolerancePpm);

// ---------------------------------------------------------------------------
// DSI enable bits (dsi_host_v2.h): DSI_EN, DSI_VIDEO_MODE_EN, DSI_CMD_MODE_EN.
// A link with neither mode enabled is powered but idle; one with both is a
// configuration mistake, and reading it is how a driver notices.
// ---------------------------------------------------------------------------

enum class DsiMode {
    Off,
    Command,
    Video,
    Both,
};

DsiMode decodeDsiMode(u32 dsiCtrl);
const char *modeName(DsiMode mode);

// A window that answers all ones has nothing behind it. The version registers
// are the first reads a warm-handoff driver makes, so this is the check that
// decides whether the ADT's address is worth believing.
bool windowAnswered(u32 value);

// ---------------------------------------------------------------------------
// The bring-up ladder
//
// The GPU's ladder ends at a Metal device; this one ends at a screen. `written`
// means the code exists; it does not mean it has run on a phone.
// ---------------------------------------------------------------------------

enum class Rung : u32 {
    Identity = 1,    // windows map, the mdss domain is on, versions and DSI state read
    Power = 2,       // PMIC rails, the mdss GDSC and the byte/pixel clocks
    Timings = 3,     // DSI video timing programmed and confirmed against the panel
    Scanout = 4,     // MDP source to mixer to DSI0, and the panel keeps scanning
    Framebuffer = 5, // an IOFramebuffer over that scanout, so iOS can draw
    Effects = 6,     // QuartzCore's filters over the scanout: blur is this rung
};

const char *rungName(Rung rung);

constexpr Rung kWrittenRung = Rung::Identity;
constexpr Rung kFinalRung = Rung::Effects;

// Cores, not wiring: the link arithmetic and the power-domain decode exist and
// are host-tested. Rung 2's rails and clocks, rung 3's PHY programming and the
// panel's init sequence are not written, so the ladder stops at rung 1.
constexpr Rung kCoreRung = Rung::Timings;

} // namespace mdss

#endif // NATIVE_MDSS_CORE_H
