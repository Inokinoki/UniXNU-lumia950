// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NATIVE_MDSS_H
#define NATIVE_MDSS_H

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>

// NativeMdss: the MSM8992 display controller (MDP + DSI 0) for the Lumia 950.
//
// It is not a framebuffer. IOFramebuffer is rung 5; this class exists to answer
// the question that comes before it — did the firmware leave the display
// running, and does the block the ADT points at answer at all — without writing
// a register or publishing a service.
class NativeMdss : public IOService {
    OSDeclareDefaultStructors(NativeMdss)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    // The ADT node's five windows, in the order the node lists them:
    // 0 mdp, 1 MMSS clock controller, 2 DSI0 ctrl, 3 DSI0 phy, 4 MMSS misc.
    bool mapWindows(IOService *provider);

    uint32_t readWindow(volatile uint8_t *base, uint32_t offset) const;

    // Rung 1: the domain first, then the version and status registers. Returns
    // false when the driver must not go further.
    bool observe();

    IOMemoryMap      *mdpMap = nullptr;
    IOMemoryMap      *mmssMap = nullptr;
    IOMemoryMap      *dsiMap = nullptr;
    IOMemoryMap      *phyMap = nullptr;
    IOMemoryMap      *miscMap = nullptr;

    volatile uint8_t *mdp = nullptr;
    volatile uint8_t *mmss = nullptr;
    volatile uint8_t *dsi = nullptr;
    volatile uint8_t *phy = nullptr;
    volatile uint8_t *misc = nullptr;

    uint32_t gdscr = 0;
};

#endif // NATIVE_MDSS_H
