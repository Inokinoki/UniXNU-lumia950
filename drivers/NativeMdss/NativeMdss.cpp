// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeMdss: the Lumia 950 (MSM8992) display driver, bring-up rung 1.
//
// After the firmware's ExitBootServices nothing owns the panel on this phone.
// The firmware did own it: it drew the Lumia logo and the UniXNU menu through
// the same MDSS block this driver maps. So the first thing worth knowing is not
// "can we drive a DSI link" but "what did the firmware leave running", and that
// is what this rung answers: the mdss power domain's state, the MDSS/MDP
// revisions, the DSI controller's own version and enable bits, and the video
// timing registers as raw words.
//
// It writes nothing and publishes nothing. That is deliberate: an IOFramebuffer
// that half-drives a panel is a black screen that looks like a kernel panic to
// everyone who is not reading this log, and this board has no serial console to
// read it on.
//
// What is not written, in the order the ladder adds it (MdssCore.h):
//   * rung 2, power: the pm8994 rails (L2, L12, L14, L28), the mdss GDSC write
//     and the byte/pixel/esc clocks. Without the clocks, nothing else matters.
//   * rung 3, timings: the DSI PHY's PLL programming and the panel's own init
//     sequence. Neither is in this repository, and neither is guessed at.
//   * rung 4, scanout: MDP source, mixer, ping-pong and interface, so a frame
//     reaches DSI 0 by itself.
//   * rung 5, framebuffer: IOFramebuffer over that, which is when iOS can draw.
//   * rung 6, effects: the compositor's filters over that surface. Blur lives
//     here, not in a driver.
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

#include "NativeMdss.h"
#include "BoardConstants.h"
#include "MdssCore.h"

using namespace mdss;

OSDefineMetaClassAndStructors(NativeMdss, IOService)

// Published for the bring-up log and the kernel symbol table, as NativeSdhci
// and NativeAdreno418 do: a boot with no serial console still shows how far the
// display got.
extern "C" {
volatile uint32_t native_mdss_stage         = 0;   // last completed rung
volatile uint32_t native_mdss_gdscr         = 0;   // gdsc_mdss as read
volatile uint32_t native_mdss_version       = 0;   // MDSS top-level version
volatile uint32_t native_mdss_mdp_version   = 0;   // MDP register block version
volatile uint32_t native_mdss_dsi_version   = 0;
volatile uint32_t native_mdss_dsi_ctrl      = 0;
volatile uint32_t native_mdss_window_alive  = 0;   // the windows answered a read
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

bool NativeMdss::mapWindows(IOService *provider)
{
    struct Window {
        IOMemoryMap **map;
        uint32_t expected;
        const char *name;
    };
    const Window windows[] = {
        { &mdpMap,  kMdpSize,       "mdp" },
        { &mmssMap, kMmssSize,      "mmss" },
        { &dsiMap,  kDsi0Size,      "dsi0" },
        { &phyMap,  kDsiPhy0Size,   "dsi0 phy" },
        { &miscMap, kMmssMiscSize,  "mmss misc" },
    };

    for (unsigned index = 0; index < sizeof windows / sizeof windows[0]; index++) {
        // IOPlatformDevice publishes the ADT node's reg entries as device memory
        // in order, so the index is the order the node lists them in.
        *windows[index].map = provider->mapDeviceMemoryWithIndex(index, 0);
        if (!*windows[index].map) {
            IOLog("NativeMdss: provider has no %s window (index %u); the ADT node "
                  "needs all five reg entries\n", windows[index].name, index);
            return false;
        }
        const IOByteCount length = (*windows[index].map)->getLength();
        if (length < windows[index].expected) {
            IOLog("NativeMdss: %s window is %llu bytes, expected at least 0x%x\n",
                  windows[index].name, static_cast<unsigned long long>(length),
                  windows[index].expected);
            return false;
        }
    }

    mdp  = reinterpret_cast<volatile uint8_t *>(mdpMap->getVirtualAddress());
    mmss = reinterpret_cast<volatile uint8_t *>(mmssMap->getVirtualAddress());
    dsi  = reinterpret_cast<volatile uint8_t *>(dsiMap->getVirtualAddress());
    phy  = reinterpret_cast<volatile uint8_t *>(phyMap->getVirtualAddress());
    misc = reinterpret_cast<volatile uint8_t *>(miscMap->getVirtualAddress());
    if (!mdp || !mmss || !dsi || !phy || !misc) {
        IOLog("NativeMdss: window mapping returned no virtual address\n");
        return false;
    }
    return true;
}

uint32_t NativeMdss::readWindow(volatile uint8_t *base, uint32_t offset) const
{
    if (!base) { return 0xffffffffu; }
    uint32_t value = 0;
    memcpy(&value, const_cast<uint8_t *>(base) + offset, sizeof value);
    return value;
}

// ---------------------------------------------------------------------------
// Rung 1: what the firmware left
// ---------------------------------------------------------------------------

bool NativeMdss::observe()
{
    gdscr = readWindow(mmss, kGdscrMdssOffset);
    native_mdss_gdscr = gdscr;

    const GdscState state = decodeGdscr(gdscr);
    IOLog("NativeMdss: gdsc_mdss 0x%08x (%s)%s%s\n", gdscr,
          state == GdscState::On ? "on" : "collapsed",
          gdscrCollapseRequested(gdscr) ? ", collapse requested" : "",
          gdscrUnderHardwareControl(gdscr) ? ", under hardware control" : "");

    if (state != GdscState::On) {
        IOLog("NativeMdss: the display domain is collapsed, so no register in the MDSS "
              "window is read: a block that is off answers badly enough to hang the "
              "interconnect. Rung 2 (pm8994 rails, the GDSC write and the byte/pixel "
              "clocks) has not run and is what would bring it up.\n");
        return false;
    }
    if (gdscrCollapseRequested(gdscr)) {
        IOLog("NativeMdss: a collapse request is pending, so the domain is mid-transition "
              "and is not read\n");
        return false;
    }

    const uint32_t mdssVersion = readWindow(mdp, kRegMdssHwVersion);
    const uint32_t mdpVersion  = readWindow(mdp, kMdpRegisterOffset + kRegMdpHwVersion);
    const uint32_t intfSel     = readWindow(mdp, kMdpRegisterOffset + kRegMdpDispIntfSel);
    const uint32_t intrStatus  = readWindow(mdp, kMdpRegisterOffset + kRegMdpIntrStatus);
    native_mdss_version = mdssVersion;
    native_mdss_mdp_version = mdpVersion;

    if (!windowAnswered(mdssVersion) || !windowAnswered(mdpVersion)) {
        IOLog("NativeMdss: the MDSS window answered all ones (MDSS 0x%08x, MDP 0x%08x), "
              "so nothing is backing the address the ADT gives; check it against the "
              "firmware before blaming the driver\n", mdssVersion, mdpVersion);
        return false;
    }
    native_mdss_window_alive = 1;

    IOLog("NativeMdss: MDSS_HW_VERSION 0x%08x, MDP HW_VERSION 0x%08x, "
          "DISP_INTF_SEL 0x%08x, INTR_STATUS 0x%08x\n",
          mdssVersion, mdpVersion, intfSel, intrStatus);

    // The DSI controller is a second block behind the same domain; its version
    // register is the cheapest proof that its window is backed.
    const uint32_t dsiVersion = readWindow(dsi, kRegDsiVersion);
    native_mdss_dsi_version = dsiVersion;
    if (!windowAnswered(dsiVersion)) {
        IOLog("NativeMdss: the DSI0 window answered 0xffffffff at its version register, "
              "so the link is not backed at this address\n");
        return false;
    }

    const uint32_t dsiCtrl     = readWindow(dsi, kRegDsiCtrl);
    const uint32_t videoCtrl   = readWindow(dsi, kRegDsiVideoModeCtrl);
    const uint32_t activeH     = readWindow(dsi, kRegDsiVideoModeActiveH);
    const uint32_t activeV     = readWindow(dsi, kRegDsiVideoModeActiveV);
    const uint32_t total       = readWindow(dsi, kRegDsiVideoModeTotal);
    const uint32_t hsync       = readWindow(dsi, kRegDsiVideoModeHsync);
    const uint32_t vsync       = readWindow(dsi, kRegDsiVideoModeVsync);
    const uint32_t clkCtrl     = readWindow(dsi, kRegDsiClkCtrl);
    const uint32_t clkStatus   = readWindow(dsi, kRegDsiClkStatus);
    native_mdss_dsi_ctrl = dsiCtrl;

    const DsiMode mode = decodeDsiMode(dsiCtrl);
    IOLog("NativeMdss: DSI_VERSION 0x%08x, DSI_CTRL 0x%08x (%s)\n",
          dsiVersion, dsiCtrl, modeName(mode));

    // Logged, not interpreted. The field layout of these six registers is not
    // sourced in this repository, and a driver that decodes a timing it cannot
    // cite is a driver that will believe its own arithmetic over the panel.
    IOLog("NativeMdss: firmware video timing words: ctrl 0x%08x active_h 0x%08x "
          "active_v 0x%08x total 0x%08x hsync 0x%08x vsync 0x%08x\n",
          videoCtrl, activeH, activeV, total, hsync, vsync);
    IOLog("NativeMdss: DSI clocks: CLK_CTRL 0x%08x, CLK_STATUS 0x%08x; "
          "mmss misc window mapped, PHY window mapped and not read\n",
          clkCtrl, clkStatus);

    if (mode == DsiMode::Off) {
        IOLog("NativeMdss: the link is not enabled, so the firmware left the panel "
              "dark or already off; nothing here claims to know which\n");
    } else if (mode == DsiMode::Both) {
        IOLog("NativeMdss: both video and command mode are enabled, which is not a "
              "configuration this driver would program; the read is reported as it is\n");
    }

    IOLog("NativeMdss: panel is %ux%u (the firmware's GOP mode); the timing above is "
          "the firmware's, not this driver's\n", kPanelWidth, kPanelHeight);

    native_mdss_stage = static_cast<uint32_t>(Rung::Identity);
    return true;
}

// ---------------------------------------------------------------------------
// IOService
// ---------------------------------------------------------------------------

bool NativeMdss::start(IOService *provider)
{
    if (!IOService::start(provider)) { return false; }
    if (!mapWindows(provider)) { return false; }

    IOLog("NativeMdss: mdp at %p, mmss at %p, dsi0 at %p\n",
          reinterpret_cast<void *>(const_cast<uint8_t *>(mdp)),
          reinterpret_cast<void *>(const_cast<uint8_t *>(mmss)),
          reinterpret_cast<void *>(const_cast<uint8_t *>(dsi)));

    if (!observe()) {
        IOLog("NativeMdss: rung 1 of %u (%s) did not complete; the display stays "
              "unowned and nothing is published\n",
              static_cast<unsigned>(kFinalRung), rungName(Rung::Identity));
        return false;
    }

    IOLog("NativeMdss: rung 1 of %u (%s) is the furthest this build has: power, "
          "timings, scanout, framebuffer and effects are not written. No framebuffer "
          "is published, so iOS draws nothing rather than drawing into a surface "
          "nothing scans out.\n",
          static_cast<unsigned>(kFinalRung), rungName(kWrittenRung));
    return true;
}

void NativeMdss::stop(IOService *provider)
{
    IOService::stop(provider);
}

void NativeMdss::free()
{
    if (mdpMap)  { mdpMap->release();  mdpMap = nullptr;  mdp = nullptr; }
    if (mmssMap) { mmssMap->release(); mmssMap = nullptr; mmss = nullptr; }
    if (dsiMap)  { dsiMap->release();  dsiMap = nullptr;  dsi = nullptr; }
    if (phyMap)  { phyMap->release();  phyMap = nullptr;  phy = nullptr; }
    if (miscMap) { miscMap->release(); miscMap = nullptr; misc = nullptr; }
    IOService::free();
}
