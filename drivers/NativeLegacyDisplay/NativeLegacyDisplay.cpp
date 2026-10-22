// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeLegacyDisplay: the legacy display probe symbol, for a board that has no
// display driver yet.
//
// Upstream's offline-component-load-probes.patch calls native_legacy_display_probe()
// from bsd_init.c without the `#if XNU_NATIVE_LEGACY_DISPLAY` guard that every other
// call site in the patch set carries. With the flag at 0 the call is still emitted, so
// a board that does not select the display provider cannot link without the symbol.
// The kernel script has a hook for exactly this case ("a stub for an unselected offline
// probe hook"): XNU_EXTRA_LD_ARCHIVES, which this board's defconfig points at the
// archive built from this file.
//
// This file provides the symbol and nothing else. It does not map a framebuffer, does
// not publish a provider, and does not claim the display path works: the Lumia 950 has
// no display driver yet (README.md, "What is not implemented"). When one exists, its
// archive replaces this one and the probe does the real work.
//
// Both entry points are dead in this configuration, because every caller sits behind a
// boot argument or behind XNU_NATIVE_LEGACY_DISPLAY. They log rather than stay silent,
// so a future run that does reach them records what answered.
#include <IOKit/IOLib.h>

extern "C" void
native_legacy_display_probe(void)
{
    IOLog("NativeLegacyDisplay: no legacy display provider on this board; probe does nothing\n");
}

extern "C" void
native_legacy_display_late_probe(void)
{
    IOLog("NativeLegacyDisplay: no legacy display provider on this board; late probe does nothing\n");
}
