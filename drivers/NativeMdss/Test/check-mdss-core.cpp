// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Host test for MdssCore. Build and run on the development machine:
//
//   g++ -std=c++17 -Wall -Wextra -Werror -I drivers/NativeMdss
//       drivers/NativeMdss/Test/check-mdss-core.cpp
//       drivers/NativeMdss/MdssCore.cpp -o build/check-mdss-core
//   ./build/check-mdss-core
//
// What this pins is the arithmetic and the refusals, not any hardware. The
// timing below is a fixture: 1440x2560 with the blanking an AMOLED of this size
// might plausibly have. It is here to make the sums checkable, and it is not a
// claim about what the Lumia 950's panel actually requires — that number is not
// in this repository.
#include <cstdio>

#include "MdssCore.h"
#include "BoardConstants.h"

using mdss::u32;
using mdss::u64;

static int failures = 0;

static void expect(bool condition, const char *what)
{
    if (condition) {
        std::printf("ok   %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

static void expectEq(u64 actual, u64 expected, const char *what)
{
    if (actual == expected) {
        std::printf("ok   %s (%llu)\n", what, static_cast<unsigned long long>(actual));
    } else {
        std::printf("FAIL %s: got %llu, expected %llu\n", what,
                    static_cast<unsigned long long>(actual),
                    static_cast<unsigned long long>(expected));
        ++failures;
    }
}

// A fixture, not a data sheet: see the note at the top of the file.
static const mdss::PanelTiming kPanel = {
    1440,   // hactive
    8,      // hSync
    32,     // hBackPorch
    20,     // hFrontPorch
    2560,   // vactive
    4,      // vSync
    8,      // vBackPorch
    16,     // vFrontPorch
};

int main()
{
    // --- the domain -------------------------------------------------------
    expect(mdss::decodeGdscr(0x80000000u) == mdss::GdscState::On,
           "PWR_ON is the domain being on");
    expect(mdss::decodeGdscr(0x00000000u) == mdss::GdscState::Collapsed,
           "a register without it is a collapsed domain");
    expect(mdss::decodeGdscr(0x80000001u) == mdss::GdscState::On,
           "a pending collapse still reads as on until it happens");
    expect(mdss::gdscrCollapseRequested(0x80000001u), "bit 0 is a collapse request");
    expect(!mdss::gdscrCollapseRequested(0x80000000u), "and absent when it is clear");
    expect(mdss::gdscrUnderHardwareControl(0x80000002u), "bit 1 is hardware control");
    expectEq(mdss::gdscrEnableValue(0x80000001u), 0x80000000u,
             "enabling clears the collapse request and nothing else");
    expectEq(mdss::gdscrEnableValue(0x00000001u), 0x00000000u,
             "and leaves a collapsed domain's state to the hardware");
    expectEq(mdss::gdscrEnableValue(0xffffffffu), 0xfffffffeu,
             "even an all-ones read keeps its PWR_ON bit");

    // --- the link arithmetic ----------------------------------------------
    expectEq(mdss::hTotal(kPanel), 1500, "h total is the four horizontal intervals");
    expectEq(mdss::vTotal(kPanel), 2588, "v total is the four vertical intervals");
    expect(mdss::timingWellFormed(kPanel), "the fixture timing is well formed");

    mdss::PanelTiming blank = kPanel;
    blank.hactive = 0;
    expect(!mdss::timingWellFormed(blank), "no visible pixels is not a timing");
    blank = kPanel;
    blank.vactive = 0;
    expect(!mdss::timingWellFormed(blank), "no visible lines is not a timing");
    blank = kPanel;
    blank.hFrontPorch = 0;
    blank.hBackPorch = 0;
    blank.hSync = 0;
    expect(!mdss::timingWellFormed(blank),
           "a total equal to the visible width leaves no blanking, and is refused");
    blank = kPanel;
    blank.hFrontPorch = 0x10000;
    expect(!mdss::timingWellFormed(blank),
           "a total past 16 bits is refused: the controller programs 16-bit counts");

    // 1500 x 2588 x 60
    expectEq(mdss::pixelClockHz(kPanel, 60), 232920000ull, "the pixel clock at 60 Hz");
    expectEq(mdss::pixelClockHz(kPanel, 0), 0, "no refresh rate is no clock");

    u64 bytes = 1;
    expect(mdss::byteClockFor(232920000, 24, &bytes), "RGB888 is a whole number of bytes");
    expectEq(bytes, 698760000ull, "the byte clock for RGB888");
    expect(!mdss::byteClockFor(232920000, 18, &bytes), "an 18-bit format is not whole bytes");
    expectEq(bytes, 0, "a refused format leaves no byte clock behind");
    expect(!mdss::byteClockFor(232920000, 0, &bytes), "no depth is no link");
    expect(!mdss::byteClockFor(232920000, 24, nullptr), "no destination is a failure");

    expectEq(mdss::bitClockHz(698760000ull, 4), 1397520000ull,
             "four lanes carry the byte clock's eight bits each");
    expectEq(mdss::bitClockHz(698760000ull, 0), 0, "no lanes is no link");

    expect(mdss::clocksAgree(1397520000ull, 1397520000ull, 1000),
           "an exact clock agrees with itself");
    expect(mdss::clocksAgree(1397520000ull + 1397520ull, 1397520000ull, 2000),
           "a clock 1000 ppm high is inside a 2000 ppm band");
    expect(!mdss::clocksAgree(1397520000ull + 13975200ull, 1397520000ull, 2000),
           "a clock 10000 ppm high is not");
    expect(!mdss::clocksAgree(1397520000ull, 0, 2000), "no expected clock agrees with nothing");

    // --- the DSI enable bits ----------------------------------------------
    expect(mdss::decodeDsiMode(0) == mdss::DsiMode::Off, "no enable bit is off");
    expect(mdss::decodeDsiMode(1) == mdss::DsiMode::Off,
           "enabled without a mode is not a mode");
    expect(mdss::decodeDsiMode(1 | 2) == mdss::DsiMode::Video, "video mode");
    expect(mdss::decodeDsiMode(1 | 4) == mdss::DsiMode::Command, "command mode");
    expect(mdss::decodeDsiMode(1 | 2 | 4) == mdss::DsiMode::Both,
           "both mode bits is a configuration mistake, reported as such");
    expect(mdss::decodeDsiMode(2) == mdss::DsiMode::Off,
           "a mode bit with no enable is not enabled");

    expectEq(static_cast<u64>(mdss::modeName(mdss::DsiMode::Off)[0]), 'o',
             "the off mode has a name");
    expect(mdss::windowAnswered(0x00010000u), "a version that is not all ones answered");
    expect(!mdss::windowAnswered(0xffffffffu), "all ones is nothing behind the window");

    // --- the ladder -------------------------------------------------------
    expect(mdss::kWrittenRung == mdss::Rung::Identity,
           "only rung 1 is written, and the driver says so");
    expect(mdss::kCoreRung == mdss::Rung::Timings,
           "cores exist through rung 3: the link arithmetic, not the PHY table");
    expect(mdss::kFinalRung == mdss::Rung::Effects, "the ladder ends at effects, not at pixels");
    expectEq(static_cast<u64>(mdss::rungName(mdss::Rung::Identity)[0]), 'i',
             "rung 1 is named identity");
    expectEq(static_cast<u64>(mdss::rungName(mdss::Rung::Effects)[0]), 'e',
             "rung 6 is named effects");

    // --- the constants hold together --------------------------------------
    // A window has to contain the registers the driver reads through it; the
    // GPU's MMSS window did not, until this test existed.
    expect(mdss::kGdscrMdssOffset + 4 <= mdss::kMmssSize,
           "gdsc_mdss is inside the MMSS window the ADT gives");
    expect(mdss::kMdpRegisterOffset + mdss::kRegMdpDispIntfSel + 4 <= mdss::kMdpSize,
           "the MDP register block is inside the MDP window");
    expect(mdss::kRegDsiVersion + 4 <= mdss::kDsi0Size,
           "DSI_VERSION is inside the DSI window");
    expect(mdss::kRegDsiClkStatus + 4 <= mdss::kDsi0Size,
           "DSI_CLK_STATUS is inside the DSI window");
    expect(mdss::kPanelWidth == 1440 && mdss::kPanelHeight == 2560,
           "the panel is the firmware's GOP mode");

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
