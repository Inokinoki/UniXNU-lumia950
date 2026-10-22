// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeSdhci: the Lumia 950 (MSM8992) eMMC driver, bring-up rung 3.
//
// What this driver does today: takes over the controller the firmware left
// running, probes the card the firmware already selected, decodes its capacity,
// and serves 512-byte reads by programmed I/O with every wait polled. That is
// enough to read the GPT and prove the command path on real silicon.
//
// What it does not do yet, in the order the bring-up ladder adds it:
//   * interrupt delivery (hc_irq is a GICv2 SPI; ArmCpu's GIC path is the gate)
//   * ADMA2 transfers (buildAdma2 in SdhciCore.cpp is written and host-tested;
//     the descriptor registers are not wired up)
//   * writes (CMD24/CMD25, cache flush, and the eMMC power-on write protect)
//   * HS200/HS400 (QSDCC vendor registers; see README.md, "Vendor registers")
// Until then the medium reports itself read-only, which is honest rather than
// silently dropping writes.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/libkern.h>

#include "NativeSdhci.h"
#include "SdhciCore.h"
#include "SdhciRegs.h"
#include "BoardConstants.h"

using namespace sdhci;

OSDefineMetaClassAndStructors(NativeSdhci, IOBlockStorageDevice)

namespace {

constexpr uint32_t kCommandTimeoutUs  = 100000;
constexpr uint32_t kTransferTimeoutUs = 500000;
constexpr uint32_t kPollStepUs        = 10;

} // namespace

// Published for the bring-up log and the kernel symbol table, in the style of
// the other native drivers: a boot with no serial console still shows whether
// the driver started, how many blocks it moved and where it gave up.
extern "C" {
volatile uint32_t native_sdhci_ready  = 0;
volatile uint32_t native_sdhci_stage  = 0;   // last completed bring-up step
volatile uint32_t native_sdhci_rca    = 0;
volatile uint64_t native_sdhci_bytes  = 0;
volatile uint64_t native_sdhci_reads  = 0;
volatile uint64_t native_sdhci_errors = 0;
}

// ---------------------------------------------------------------------------
// Register access
// ---------------------------------------------------------------------------

uint8_t NativeSdhci::readHc8(uint32_t offset) const
{
    return hc ? hc[offset] : 0;
}

uint16_t NativeSdhci::readHc16(uint32_t offset) const
{
    if (!hc) return 0;
    uint16_t value = 0;
    memcpy(&value, const_cast<volatile uint8_t *>(hc) + offset, sizeof value);
    return value;
}

uint32_t NativeSdhci::readHc32(uint32_t offset) const
{
    if (!hc) return 0;
    uint32_t value = 0;
    memcpy(&value, const_cast<volatile uint8_t *>(hc) + offset, sizeof value);
    return value;
}

void NativeSdhci::writeHc8(uint32_t offset, uint8_t value)
{
    if (hc) hc[offset] = value;
}

void NativeSdhci::writeHc16(uint32_t offset, uint16_t value)
{
    if (hc) memcpy(const_cast<volatile uint8_t *>(hc) + offset, &value, sizeof value);
}

void NativeSdhci::writeHc32(uint32_t offset, uint32_t value)
{
    if (hc) memcpy(const_cast<volatile uint8_t *>(hc) + offset, &value, sizeof value);
}

uint32_t NativeSdhci::readCore32(uint32_t offset) const
{
    if (!core) return 0;
    uint32_t value = 0;
    memcpy(&value, const_cast<volatile uint8_t *>(core) + offset, sizeof value);
    return value;
}

// ---------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------

bool NativeSdhci::mapWindows(IOService *provider)
{
    // IOPlatformDevice publishes the ADT node's reg entries as device memory,
    // in order: 0 is the SDHCI window, 1 the Qualcomm core window.
    hcMap = provider->mapDeviceMemoryWithIndex(0, 0);
    coreMap = provider->mapDeviceMemoryWithIndex(1, 0);
    if (!hcMap || !coreMap) {
        IOLog("NativeSdhci: provider has no reg windows (hc %p core %p); "
              "the ADT node needs both reg entries\n", hcMap, coreMap);
        return false;
    }
    if (hcMap->getLength() < lumia950::kSdhciHcSize) {
        IOLog("NativeSdhci: hc window is %llu bytes, expected at least 0x%x\n",
              static_cast<unsigned long long>(hcMap->getLength()), lumia950::kSdhciHcSize);
        return false;
    }

    hc = reinterpret_cast<volatile uint8_t *>(hcMap->getVirtualAddress());
    core = reinterpret_cast<volatile uint8_t *>(coreMap->getVirtualAddress());
    if (!hc || !core) {
        IOLog("NativeSdhci: window mapping returned no virtual address\n");
        return false;
    }
    return true;
}

void NativeSdhci::logHost()
{
    hostVersion = readHc16(kRegHostVersion);
    const uint32_t capabilities = readHc32(kRegCapabilities);
    baseClockHz = ((capabilities >> kCapBaseClockShift) & kCapBaseClockMask) * 1000000u;

    const uint16_t clockControl = readHc16(kRegClockControl);
    const uint32_t divider = decodeDivider(clockControl);
    const uint32_t cardHz = baseClockHz && divider ? baseClockHz / (2 * divider) : 0;

    IOLog("NativeSdhci: host version 0x%04x, capabilities 0x%08x\n", hostVersion, capabilities);
    IOLog("NativeSdhci: base clock %u Hz, clock control 0x%04x (divider %u, card %u Hz, enabled %u)\n",
          baseClockHz, clockControl, divider, cardHz, (clockControl & kClockSdClockEnable) ? 1 : 0);
    IOLog("NativeSdhci: present state 0x%08x, host control 0x%02x, power 0x%02x, "
          "core[0x00] 0x%08x\n",
          readHc32(kRegPresentState), readHc8(kRegHostControl1), readHc8(kRegPowerControl),
          readCore32(0x00));
}

void NativeSdhci::resetCommandLine()
{
    // Reset only the command and data state machines. A full reset would clear
    // the clock and power the firmware configured, which is the one thing this
    // warm handoff relies on.
    writeHc8(kRegSoftwareReset, kResetCommand | kResetData);
    for (uint32_t waited = 0; waited < kCommandTimeoutUs; waited += kPollStepUs) {
        if (!(readHc8(kRegSoftwareReset) & (kResetCommand | kResetData))) return;
        IODelay(kPollStepUs);
    }
    IOLog("NativeSdhci: command reset did not complete\n");
}

// ---------------------------------------------------------------------------
// Command path
// ---------------------------------------------------------------------------

bool NativeSdhci::waitPresent(uint32_t mask, uint32_t value, uint32_t timeoutUs)
{
    for (uint32_t waited = 0; waited < timeoutUs; waited += kPollStepUs) {
        if ((readHc32(kRegPresentState) & mask) == value) return true;
        IODelay(kPollStepUs);
    }
    return false;
}

void NativeSdhci::clearInterrupts()
{
    writeHc32(kRegInterruptStatus, 0xFFFFFFFF);
}

bool NativeSdhci::sendCommand(uint8_t index, uint32_t argument, uint16_t flags, uint32_t response[4])
{
    if (!waitPresent(kPresentCmdInhibit | kPresentDataInhibit, 0, kCommandTimeoutUs)) {
        IOLog("NativeSdhci: command %u: host stayed inhibited\n", index);
        __atomic_fetch_add(&native_sdhci_errors, 1, __ATOMIC_RELAXED);
        return false;
    }

    clearInterrupts();
    writeHc32(kRegArgument, argument);
    writeHc16(kRegCommand, static_cast<uint16_t>((index << 8) | flags));

    uint32_t status = 0;
    bool complete = false;
    for (uint32_t waited = 0; waited < kCommandTimeoutUs; waited += kPollStepUs) {
        status = readHc32(kRegInterruptStatus);
        if (status & kIntError) {
            IOLog("NativeSdhci: command %u failed, status 0x%08x\n", index, status);
            __atomic_fetch_add(&native_sdhci_errors, 1, __ATOMIC_RELAXED);
            return false;
        }
        if (status & kIntCommandComplete) {
            complete = true;
            break;
        }
        IODelay(kPollStepUs);
    }

    if (!complete) {
        IOLog("NativeSdhci: command %u timed out, status 0x%08x\n", index, status);
        __atomic_fetch_add(&native_sdhci_errors, 1, __ATOMIC_RELAXED);
        return false;
    }

    if (response) {
        for (uint32_t word = 0; word < 4; ++word) {
            response[word] = readHc32(kRegResponse + word * 4);
        }
    }
    return true;
}

bool NativeSdhci::readBlockPio(uint64_t lba, void *bytes)
{
    if (!bytes) return false;

    writeHc16(kRegBlockSize, static_cast<uint16_t>(lumia950::kLogicalBlockBytes));
    writeHc16(kRegBlockCount, 1);
    writeHc32(kRegArgument, static_cast<uint32_t>(lba));
    writeHc16(kRegTransferMode, kModeBlockCountEnable | kModeDirectionRead);

    uint32_t response[4] = {};
    if (!sendCommand(kCmd17ReadSingleBlock, static_cast<uint32_t>(lba),
                     kCmdDataPresent | kCmdResponse48 | kCmdCrcCheck | kCmdIndexCheck, response)) {
        return false;
    }

    uint32_t *words = reinterpret_cast<uint32_t *>(bytes);
    for (uint32_t word = 0; word < lumia950::kLogicalBlockBytes / 4; ++word) {
        if (!waitPresent(kPresentBufferRead, kPresentBufferRead, kTransferTimeoutUs)) {
            IOLog("NativeSdhci: LBA %llu stalled before word %u\n",
                  static_cast<unsigned long long>(lba), word);
            __atomic_fetch_add(&native_sdhci_errors, 1, __ATOMIC_RELAXED);
            return false;
        }
        words[word] = readHc32(kRegBufferPort);
    }

    const uint32_t status = readHc32(kRegInterruptStatus);
    if (!(status & kIntTransferComplete)) {
        IOLog("NativeSdhci: LBA %llu ended without transfer complete, status 0x%08x\n",
              static_cast<unsigned long long>(lba), status);
        __atomic_fetch_add(&native_sdhci_errors, 1, __ATOMIC_RELAXED);
        return false;
    }
    clearInterrupts();
    return true;
}

// ---------------------------------------------------------------------------
// Card identification
// ---------------------------------------------------------------------------

bool NativeSdhci::readCsd(uint16_t rca, uint32_t csd[4])
{
    return sendCommand(kCmd9SendCsd, static_cast<uint32_t>(rca) << 16,
                       kCmdResponse136 | kCmdCrcCheck | kCmdIndexCheck, csd);
}

bool NativeSdhci::readExtCsd(uint32_t extCsd[128])
{
    if (!extCsd) return false;

    writeHc16(kRegBlockSize, static_cast<uint16_t>(lumia950::kLogicalBlockBytes));
    writeHc16(kRegBlockCount, 1);
    writeHc32(kRegArgument, 0);
    writeHc16(kRegTransferMode, kModeBlockCountEnable | kModeDirectionRead);

    uint32_t response[4] = {};
    if (!sendCommand(kCmd8SendExtCsd, 0, kCmdDataPresent | kCmdResponse48 | kCmdCrcCheck | kCmdIndexCheck,
                     response)) {
        return false;
    }
    for (uint32_t word = 0; word < 128; ++word) {
        if (!waitPresent(kPresentBufferRead, kPresentBufferRead, kTransferTimeoutUs)) {
            IOLog("NativeSdhci: EXT_CSD stalled before word %u\n", word);
            return false;
        }
        extCsd[word] = readHc32(kRegBufferPort);
    }

    const uint32_t status = readHc32(kRegInterruptStatus);
    if (!(status & kIntTransferComplete)) {
        IOLog("NativeSdhci: EXT_CSD ended without transfer complete, status 0x%08x\n", status);
        return false;
    }
    clearInterrupts();
    return true;
}

bool NativeSdhci::probeCard()
{
    // The firmware selected the card and left it that way, so there is no
    // CMD0/CMD1/CMD2/CMD3 sequence here: the only thing to find out is which
    // relative address it used.
    uint32_t response[4] = {};
    bool selected = false;
    for (uint16_t candidate : lumia950::kRcaCandidates) {
        if (sendCommand(kCmd13SendStatus, static_cast<uint32_t>(candidate) << 16,
                        kCmdResponse48 | kCmdCrcCheck | kCmdIndexCheck, response)) {
            relativeAddress = candidate;
            selected = true;
            break;
        }
    }
    if (!selected) {
        IOLog("NativeSdhci: no response from the card at RCA 1, 2 or 0; "
              "the firmware may not have left it selected\n");
        return false;
    }
    __atomic_store_n(&native_sdhci_rca, relativeAddress, __ATOMIC_RELAXED);
    IOLog("NativeSdhci: card answers at RCA %u, status 0x%08x\n", relativeAddress, response[0]);
    native_sdhci_stage = 1;

    uint32_t csd[4] = {};
    if (!readCsd(relativeAddress, csd)) {
        IOLog("NativeSdhci: CMD9 (SEND_CSD) failed\n");
        return false;
    }

    CsdInfo info = {};
    if (!decodeCsd(csd, &info)) {
        IOLog("NativeSdhci: CSD %08x %08x %08x %08x is not decodable\n",
              csd[0], csd[1], csd[2], csd[3]);
        return false;
    }
    IOLog("NativeSdhci: CSD structure %u, read block length %u, "
          "C_SIZE %u, C_SIZE_MULT %u, saturated %u\n",
          info.structure, info.readBlockLen, info.cSize, info.cSizeMult, info.saturated ? 1 : 0);
    native_sdhci_stage = 2;

    if (info.saturated) {
        uint32_t extCsd[128] = {};
        if (!readExtCsd(extCsd)) {
            IOLog("NativeSdhci: CMD8 (SEND_EXT_CSD) failed, capacity unknown\n");
            return false;
        }
        capacityBytes = parseExtCsdSectorCount(reinterpret_cast<const uint8_t *>(extCsd));
        if (!capacityBytes) {
            IOLog("NativeSdhci: EXT_CSD reports no sectors\n");
            return false;
        }
        IOLog("NativeSdhci: EXT_CSD sector count gives %llu bytes\n",
              static_cast<unsigned long long>(capacityBytes));
    } else {
        capacityBytes = info.capacityBytes;
    }

    if (capacityBytes < lumia950::kLogicalBlockBytes * 2) {
        IOLog("NativeSdhci: capacity %llu bytes is too small to hold a partition table\n",
              static_cast<unsigned long long>(capacityBytes));
        return false;
    }
    blockCount = capacityBytes / lumia950::kLogicalBlockBytes;
    native_sdhci_stage = 3;

    // eMMC ignores CMD16 when EXT_CSD sets the block length, so a refusal here
    // is informational, not fatal.
    if (!sendCommand(kCmd16SetBlockLength, lumia950::kLogicalBlockBytes,
                     kCmdResponse48 | kCmdCrcCheck | kCmdIndexCheck, response)) {
        IOLog("NativeSdhci: CMD16 (SET_BLOCKLEN) refused; continuing\n");
    }

    // The cheapest end-to-end proof that the command path works: read the GPT
    // header and look for its signature.
    alignas(8) uint8_t gpt[lumia950::kLogicalBlockBytes] = {};
    if (readBlockPio(lumia950::kGptHeaderLba, gpt)) {
        const bool signature = memcmp(gpt, lumia950::kGptSignature, sizeof lumia950::kGptSignature) == 0;
        IOLog("NativeSdhci: GPT header %s\n", signature ? "found" : "missing (signature mismatch)");
        native_sdhci_stage = signature ? 4 : 3;
    }
    return true;
}

// ---------------------------------------------------------------------------
// IOService
// ---------------------------------------------------------------------------

bool NativeSdhci::start(IOService *provider)
{
    if (!IOBlockStorageDevice::start(provider)) return false;

    gate = IOLockAlloc();
    if (!gate) {
        IOLog("NativeSdhci: no lock\n");
        return false;
    }
    if (!mapWindows(provider)) return false;

    IOLog("NativeSdhci: hc at %p (virtual), core at %p\n",
          reinterpret_cast<void *>(const_cast<volatile uint8_t *>(hc)),
          reinterpret_cast<void *>(const_cast<volatile uint8_t *>(core)));

    logHost();
    resetCommandLine();
    native_sdhci_stage = 0;

    if (!probeCard()) {
        IOLog("NativeSdhci: card probe failed; not publishing a medium\n");
        return false;
    }

    IOLockLock(gate);
    online = true;
    IOLockUnlock(gate);
    __atomic_store_n(&native_sdhci_ready, 1, __ATOMIC_RELAXED);

    IOLog("NativeSdhci: %llu blocks of %u bytes (%llu bytes), read-only until the "
          "write path exists\n",
          static_cast<unsigned long long>(blockCount), lumia950::kLogicalBlockBytes,
          static_cast<unsigned long long>(capacityBytes));
    registerService();
    return true;
}

void NativeSdhci::stop(IOService *provider)
{
    if (gate) {
        IOLockLock(gate);
        online = false;
        IOLockUnlock(gate);
    }
    IOBlockStorageDevice::stop(provider);
}

void NativeSdhci::free()
{
    if (hcMap) { hcMap->release(); hcMap = nullptr; hc = nullptr; }
    if (coreMap) { coreMap->release(); coreMap = nullptr; core = nullptr; }
    if (gate) { IOLockFree(gate); gate = nullptr; }
    IOBlockStorageDevice::free();
}

// ---------------------------------------------------------------------------
// IOBlockStorageDevice
// ---------------------------------------------------------------------------

IOReturn NativeSdhci::reportBlockSize(UInt64 *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = lumia950::kLogicalBlockBytes;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportEjectability(bool *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportMaxValidBlock(UInt64 *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = blockCount ? blockCount - 1 : 0;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportMediaState(bool *present, bool *changed)
{
    if (!present) return kIOReturnBadArgument;
    *present = online;
    if (changed) *changed = false;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportRemovability(bool *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::reportWriteProtection(bool *value)
{
    if (!value) return kIOReturnBadArgument;
    // The medium is not physically write protected; this driver has no write
    // path yet, so it reports the protection that is actually in force.
    *value = true;
    return kIOReturnSuccess;
}

IOReturn NativeSdhci::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                       IOStorageAttributes *, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !completion->action || !count) return kIOReturnBadArgument;
    if (block >= blockCount || count > blockCount - block) return kIOReturnBadArgument;

    if (buffer->getDirection() == kIODirectionOut) {
        if (!writeRefused) {
            writeRefused = true;
            IOLog("NativeSdhci: refusing a write: the write path is not implemented "
                  "and the medium reports write protection\n");
        }
        return kIOReturnUnsupported;
    }
    if (buffer->getDirection() != kIODirectionIn) return kIOReturnBadArgument;

    const uint64_t bytes = count * lumia950::kLogicalBlockBytes;
    if (buffer->getLength() < bytes) return kIOReturnBadArgument;

    IOLockLock(gate);
    if (!online) {
        IOLockUnlock(gate);
        return kIOReturnNotReady;
    }

    alignas(8) uint8_t staging[lumia950::kLogicalBlockBytes] = {};
    uint64_t done = 0;
    IOReturn status = kIOReturnSuccess;
    for (uint64_t index = 0; index < count; ++index) {
        if (!readBlockPio(block + index, staging)) {
            status = kIOReturnIOError;
            break;
        }
        const uint64_t offset = index * lumia950::kLogicalBlockBytes;
        if (buffer->writeBytes(offset, staging, lumia950::kLogicalBlockBytes) != lumia950::kLogicalBlockBytes) {
            status = kIOReturnIOError;
            break;
        }
        done += lumia950::kLogicalBlockBytes;
    }

    __atomic_fetch_add(&native_sdhci_bytes, done, __ATOMIC_RELAXED);
    if (done) __atomic_fetch_add(&native_sdhci_reads, 1, __ATOMIC_RELAXED);
    IOLockUnlock(gate);

    completion->action(completion->target, completion->parameter, status, done);
    return kIOReturnSuccess;
}
