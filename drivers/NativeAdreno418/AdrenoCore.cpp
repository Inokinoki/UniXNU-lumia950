// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AdrenoCore.h"

// The decoder's two board values: the generation this driver supports and the
// bit that says a GDSC is powered. Both are cited in BoardConstants.h.
#include "BoardConstants.h"

namespace adreno {

ChipIdentity decodeChipId(u32 chipId)
{
    ChipIdentity identity;
    identity.generation = (chipId >> 24) & 0xff;
    identity.major      = (chipId >> 16) & 0xff;
    identity.minor      = (chipId >> 8) & 0xff;
    identity.patch      = chipId & 0xff;
    return identity;
}

bool isA4xx(const ChipIdentity &identity)
{
    return identity.generation == lumia950::kA4xxGeneration;
}

PowerState decodeGdscr(u32 gdscr)
{
    return (gdscr & lumia950::kGdscrPowerOn) ? PowerState::On : PowerState::Collapsed;
}

bool gdscrCollapseRequested(u32 gdscr)
{
    return (gdscr & kGdscrSwCollapse) != 0;
}

u32 gdscrEnableValue(u32 current)
{
    return current & ~kGdscrSwCollapse;
}

u32 gdscrDisableValue(u32 current)
{
    return current | kGdscrSwCollapse;
}

bool gdscrUnderHardwareControl(u32 current)
{
    return (current & kGdscrHwControl) != 0;
}

void gdscPollBegin(GdscPoller *poller)
{
    if (poller) { poller->polls = 0; }
}

GdscPoll gdscPollUntilOn(GdscPoller *poller, u32 sample)
{
    if (!poller) { return GdscPoll::TimedOut; }
    if (decodeGdscr(sample) == PowerState::On) { return GdscPoll::PoweredOn; }
    return ++poller->polls >= kGdscrMaxPolls ? GdscPoll::TimedOut : GdscPoll::Waiting;
}

GdscPoll gdscPollUntilOff(GdscPoller *poller, u32 sample)
{
    if (!poller) { return GdscPoll::TimedOut; }
    if (decodeGdscr(sample) == PowerState::Collapsed) { return GdscPoll::PoweredOff; }
    return ++poller->polls >= kGdscrMaxPolls ? GdscPoll::TimedOut : GdscPoll::Waiting;
}

WindowAnswer classifyWindowRead(u32 value)
{
    return value == 0xffffffffu ? WindowAnswer::NoResponse : WindowAnswer::Answered;
}

bool plausibleVersion(u32 value)
{
    return value != 0u && value != 0xffffffffu;
}

u32 ringUsedDwords(u32 writePointer, u32 readPointer, u32 sizeDwords)
{
    if (sizeDwords == 0) { return 0; }
    // Both pointers are masked first: a pointer the hardware left outside the
    // ring (a stale value after a reset, say) must not make the subtraction
    // report a distance the ring cannot hold.
    return (ringAdvance(writePointer, 0, sizeDwords)
            - ringAdvance(readPointer, 0, sizeDwords)) & (sizeDwords - 1);
}

u32 ringFreeDwords(u32 writePointer, u32 readPointer, u32 sizeDwords)
{
    if (sizeDwords == 0) { return 0; }
    return sizeDwords - ringUsedDwords(writePointer, readPointer, sizeDwords);
}

u32 ringAdvance(u32 pointer, u32 dwords, u32 sizeDwords)
{
    if (sizeDwords == 0) { return 0; }
    return (pointer + dwords) & (sizeDwords - 1);
}

bool ringPointerValid(u32 pointer, u32 sizeDwords)
{
    if (sizeDwords == 0) { return false; }
    // The ring size the CP accepts is a power of two, so "inside the ring" is
    // "no bit above the mask": a pointer that is misaligned or out of range
    // fails here rather than wrapping into a slot it does not own.
    return (pointer & (sizeDwords - 1)) == pointer;
}

const char *rungName(Rung rung)
{
    switch (rung) {
    case Rung::Identity: return "identity";
    case Rung::Power:    return "power";
    case Rung::Smmu:     return "smmu";
    case Rung::Ring:     return "ring";
    case Rung::Blit:     return "blit";
    case Rung::Metal:    return "metal";
    }
    return "unknown";
}

} // namespace adreno
