// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Host-testable part of the Lumia 950 GPU driver: the decoding and the ring
// arithmetic. Nothing here touches hardware, IOKit or XNU, so it compiles and
// runs on the build host (drivers/NativeAdreno418/Test/check-adreno-core.cpp).
#ifndef NATIVE_ADRENO418_CORE_H
#define NATIVE_ADRENO418_CORE_H

#include "AdrenoTypes.h"

namespace adreno {

// The device tree's qcom,chipid, taken apart. Qualcomm writes it as
// generation.major.minor.patch, most significant byte first; 0x04010800 is
// generation 4 (a4xx), major 1, minor 8, patch 0.
struct ChipIdentity {
    u32 generation;
    u32 major;
    u32 minor;
    u32 patch;
};

ChipIdentity decodeChipId(u32 chipId);

// True when the part is an a4xx, the family this driver's register database and
// command stream cover. Another generation is a refusal, not a fallback.
bool isA4xx(const ChipIdentity &identity);

// Where the OxiLi power domains stand, as one GDSCR register reports it.
enum class PowerState {
    On,         // PWR_ON (bit 31) is set: the domain is powered
    Collapsed,  // it is not: register reads can hang the interconnect
};

PowerState decodeGdscr(u32 gdscr);

// SW_COLLAPSE (bit 0): set means software asked for the domain to collapse.
// A domain that still reports On while this is set is mid-transition, which is
// not a state to program a GPU in.
bool gdscrCollapseRequested(u32 gdscr);

// ---------------------------------------------------------------------------
// Powering a domain on (rung 2, core only)
//
// Qualcomm's drivers/clk/qcom/gdsc.c is the sequence: refuse while the domain is
// under hardware control, clear or set SW_COLLAPSE, then poll PWR_ON for a
// bounded number of reads. The registers and the poll live here so the ordering
// is testable; the RPM votes and clock registers that make the sequence actually
// power the block on a phone are rung 2's other half and are not written.
// ---------------------------------------------------------------------------

constexpr u32 kGdscrSwCollapse = 1u << 0;
constexpr u32 kGdscrHwControl  = 1u << 1;

// gdsc.c: MAX_GDSCR_READS is 100, one microsecond apart.
constexpr u32 kGdscrMaxPolls = 100;

// The value to write to bring the domain up or down: the current register with
// only SW_COLLAPSE changed, because every other bit is the firmware's business.
u32 gdscrEnableValue(u32 current);
u32 gdscrDisableValue(u32 current);

// gdsc.c refuses an enable or disable while HW_CONTROL is set: the power
// controller owns the domain and software writing SW_COLLAPSE would race it.
bool gdscrUnderHardwareControl(u32 current);

// The poll, as a state machine: feed it the value read back after the write and
// it says whether to read again, whether the domain reached the wanted state, or
// whether the sequence timed out.
enum class GdscPoll {
    Waiting,
    PoweredOn,
    PoweredOff,
    TimedOut,
};

struct GdscPoller {
    u32 polls;
};

void gdscPollBegin(GdscPoller *poller);
GdscPoll gdscPollUntilOn(GdscPoller *poller, u32 sample);
GdscPoll gdscPollUntilOff(GdscPoller *poller, u32 sample);

// What a register read in the GPU window is worth. An interconnect that has no
// device behind it answers with all ones; that is not a version number.
enum class WindowAnswer {
    NoResponse,  // 0xffffffff: the window is not backed by a live block
    Answered,    // some other value; see plausibleVersion for whether it means anything
};

WindowAnswer classifyWindowRead(u32 value);

// A version register that reads 0 is as unhelpful as one that reads all ones:
// neither is a revision of an a4xx. Used to keep the log honest about what the
// read proves (that the window is backed) and what it does not (which revision).
bool plausibleVersion(u32 value);

// ---------------------------------------------------------------------------
// Command processor ring
//
// The a4xx CP walks a ring of 32-bit command words between the read pointer the
// hardware keeps and the write pointer this side hands it. Every pointer is a
// dword index into a ring of sizeDwords words, so both wrap at that size and
// the difference always measures a distance in the ring.
// ---------------------------------------------------------------------------

// Words queued: the distance the write pointer has run past the read pointer.
// A full ring is indistinguishable from an empty one this way, which is why the
// submission rung keeps one word of slack.
u32 ringUsedDwords(u32 writePointer, u32 readPointer, u32 sizeDwords);

// Words the ring can still take, reserving nothing.
u32 ringFreeDwords(u32 writePointer, u32 readPointer, u32 sizeDwords);

// Advances a pointer by dwords, wrapping at the ring size.
u32 ringAdvance(u32 pointer, u32 dwords, u32 sizeDwords);

// A pointer is usable when it is a whole dword index inside the ring. The ring
// size is a power of two, so the mask is the only bound that matters.
bool ringPointerValid(u32 pointer, u32 sizeDwords);

// Byte offset of a register the register database names by dword index.
constexpr u32 byteOffset(u32 index)
{
    return index << 2;
}

// ---------------------------------------------------------------------------
// The bring-up ladder
//
// Each rung is what the driver proves before the next one may run. `written`
// means the code exists; it does not mean it has run on a phone.
// ---------------------------------------------------------------------------

enum class Rung : u32 {
    Identity = 1,  // map the windows, prove the domain is on, read the revision
    Power = 2,     // RPM vote plus GDSC on, clocks and bus at a level
    Smmu = 3,      // IOMMU contexts and buffers the GPU can dereference
    Ring = 4,      // CP ring, fences and submission
    Blit = 5,      // PM4 that clears or copies, and a fence that says it finished
    Metal = 6,     // the Metal plugin over Metal, metal2vulkan, Turnip, IR3
};

const char *rungName(Rung rung);

// The furthest rung this build can start, and the first one it cannot. The
// driver prints both, because "rung 1 of 6" is the honest state of the GPU on
// this board and a log that omits it invites worse assumptions.
constexpr Rung kWrittenRung = Rung::Identity;
constexpr Rung kFinalRung = Rung::Metal;

// Cores, not wiring: the power sequence, the page-table encoder and the command
// stream (CommandStream.h) exist and are host-tested, but no driver programs
// them yet. The distinction is the whole point of the ladder, so it is a
// constant and a test rather than a sentence. Rung 5 is not here because its
// blit is not written: the stream's flush, wait and fence are, which is the half
// a submission needs to be *confirmed* rather than the half that draws.
constexpr Rung kCoreRung = Rung::Ring;

} // namespace adreno

#endif // NATIVE_ADRENO418_CORE_H
