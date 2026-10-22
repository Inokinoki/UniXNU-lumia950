# UniXNU-lumia950

Device repository for the Microsoft Lumia 950 (MSM8992, Snapdragon 808), targeting the
UniXNU stack: an AArch64 UEFI firmware, the UniXNU boot manager, DarwinBoot and a patched
XNU running an iOS userland.

This is a **P1 research skeleton**. `board.toml` keeps `example = true`, so the build driver
refuses to build or boot-image it — that is the intended guard, not an oversight. What is
here is the part that could be built and checked without the phone: the device repository
structure, the register map with its sources, and the eMMC driver's host-testable core.

## State

| Layer | State |
| --- | --- |
| Device repository | `board.toml`, defconfig, `NativeSdhci`, build and test scripts; accepted by `unixnu status` as `[EXAMPLE]` |
| UEFI firmware | not started; needs a decision (see [Blockers](#blockers)) |
| UniXNU menu | blocked on firmware |
| DarwinBoot | blocked on firmware |
| XNU entry | blocked on firmware and the GICv2 question |
| Storage driver | written, host-tested, never run on hardware |
| Root volume and userland | not started |

## What is verified, and how

| Check | Command | Result |
| --- | --- | --- |
| SDHCI core logic (39 checks) | `scripts\test-core.bat` (Windows) or the `g++` line inside it | all pass |
| Board and defconfig parse | `unixnu defconfig lumia950_defconfig -D lumia950` | accepted |
| Module resolution | `unixnu status -D lumia950` | `driver NativeSdhci device` |
| Incomplete-board guard | `unixnu build kernel --dry-run -D lumia950` | refused, as designed |
| CI workflow file | YAML parse, `bash -n` on all 12 run blocks | clean |
| Kernel-ABI compile of `NativeSdhci.cpp` | `kernel` job, [run 39856939504](https://github.com/ManGuy-Legend/UniXNU-lumia950/actions/runs/39856939504) | compiled clean under `-Werror`; `libNativeSdhci.a` sha256 `2510a90c…ea2c84` |

The driver's *kernel* half (`NativeSdhci.cpp`) compiles against the recorded kernel ABI,
and the archive holds two ARM64 `MH_OBJECT` members: `NativeSdhci.o` and `SdhciCore.o`.
The XNU link in that same run stops on `_native_legacy_display_probe`, which is outside the
driver: upstream calls it from `bsd_init.c` with no `#if`, and this board has no display
provider. `libNativeSdhci.a` is the milestone; the kernel's final link stays red until the
board gains a display driver.

## Layout

```
lumia950/
  board.toml                        board description, incomplete by design
  configs/lumia950_defconfig        kernel configuration
  drivers/NativeSdhci/              the eMMC driver
    BoardConstants.h                Lumia 950 addresses, each with its source
    SdhciRegs.h                     SDHCI register map (spec layout, not guessed)
    SdhciCore.{h,cpp}               clock, CSD, EXT_CSD and ADMA2 arithmetic
    NativeSdhci.{h,cpp}             IOKit driver: warm handoff, PIO reads
    Info.plist                      personality for `qcom,msm8994-sdhci`
    Test/check-sdhci-core.cpp       host test for the core
  scripts/build-arm-driver.py       compile a driver against the recorded XNU ABI
  scripts/test-core.bat             host test runner (Windows)
```

## The eMMC map

Everything below is from the mainline MSM8992 device tree (`msm8992.dtsi` is `msm8994.dtsi`
with two A57 cores deleted), node `sdhc1`, alias `mmc1`:

| Item | Value |
| --- | --- |
| compatible | `"qcom,msm8994-sdhci", "qcom,sdhci-msm-v4"` |
| SDHCI window (`hc`) | `0xf9824900`, length `0x1a0` |
| QSDCC window (`core`) | `0xf9824000`, length `0x800` |
| Interrupts | `GIC_SPI 123` (hc_irq), `GIC_SPI 138` (pwr_irq) |
| Clock source | XO `19.2 MHz`; `GCC_SDCC1_AHB_CLK`, `GCC_SDCC1_APPS_CLK` |
| Bus | 8-bit, non-removable |
| Interrupt controller | `qcom,msm-qgic2` at `0xf9000000` + `0xf9002000` — GICv2, no redistributors |
| Supporting windows | GCC `0xfc400000`, MMCC `0xfd8c0000`, TLMM `0xfd510000`, APCS `0xf900d000`, RPM msg RAM `0xfc428000` |
| microSD, for comparison | `0xf98a4900` / `0xf98a4000`, SPI 125 / 221, card detect GPIO 100 |

These are the SoC's addresses for the controller. They are not proof of what the Lumia
firmware programmed. Three ways to prove them, cheapest first:

1. **The open firmware is the oracle.** Lumia950XLPkg contains the SDCC DXE driver that
   actually brings this controller up on this board, in C, for this exact phone. Add
   register-level logging to it and photograph the framebuffer, or dump the same values
   from the driver here and compare.
2. **The ACPI tables.** Lumia950XLPkg copies its tables from the stock Windows Phone FFU,
   so the DSDT carries Qualcomm device entries with `_CRS` memory ranges and `_GSI`
   interrupt numbers.
3. **A UEFI shell.** Enumerate `EFI_BLOCK_IO_PROTOCOL`, print capacity and logical block
   size, read LBA 0 and LBA 1. That is the block map, with no new code.

### Vendor registers

The QSDCC `core` window holds the DLL and power-control registers required for HS200 and
HS400. Their offsets are **not** in this repository: they are absent rather than guessed,
because a wrong offset in a clock or DLL register is indistinguishable from dead hardware.
Collect them from `drivers/mmc/host/sdhci-msm.c` in mainline and confirm them against the
firmware's own writes before adding them.

## Driver design: a warm handoff

The firmware leaves the controller clocked, powered and the card selected. The driver
therefore does not run CMD0/CMD1/CMD2/CMD3, does not touch the clock or power registers, and
does not reset the controller — a full reset would discard the only state that makes the
handoff work. It resets the command and data state machines, then finds out which relative
card address the firmware used by trying CMD13 at RCA 1, 2 and 0.

Bring-up rungs, in order, each with a distinct failure signature:

| Rung | What it proves | State |
| --- | --- | --- |
| Register read at `0xf9824900` | the window is mapped and the controller is alive | written |
| CMD13 at the firmware's RCA | the command path works | written |
| CMD9 → CSD, CMD8 → EXT_CSD | capacity decoding | written, host-tested |
| CMD17 PIO read of LBA 1, `"EFI PART"` | end-to-end data path on real silicon | written |
| ADMA2 transfers | throughput; needs the descriptor registers wired | `buildAdma2` written and tested, unwired |
| Interrupt-driven completion | hc_irq; gated on ArmCpu's GICv2 path | not written |
| CMD24/CMD25 + cache flush | a writable medium | not written |
| HS200/HS400 | usable speed | not written |

Until the write path exists the driver reports the medium as write-protected and refuses
writes loudly, rather than accepting and dropping them.

### Linking it into the kernel

The kernel build needs three things this repository cannot provide on its own:

1. The archive, from `scripts/build-arm-driver.py NativeSdhci` (listed in
   `[kernel] archives`).
2. `XNU_NATIVE_SDHCI=1`, which `CONFIG_XNU_NATIVE_SDHCI=y` in the defconfig produces.
3. A personality in `iokit/KernelConfigTables.cpp`, applied as a patch:

```c
#if defined(XNU_NATIVE_SDHCI) && XNU_NATIVE_SDHCI
    ",{"
    " 'IOClass' = NativeSdhci;"
    " 'IOProviderClass' = IOPlatformDevice;"
    " 'IONameMatch' = 'qcom,msm8994-sdhci';"
    " 'IOProbeScore' = 1000:32;"
    "}"
#endif
```

and the archive itself through `-Wl,-force_load` so the linker keeps the class.

## The ADT node

The driver matches an `/arm-io` child. The ADT generator (`make-lumia950-adt.py`, not
written yet) must emit at least:

```
/arm-io/sdhci@f9824900 {
    name = "sdhci";
    compatible = "qcom,msm8994-sdhci", "qcom,sdhci-msm-v4";
    reg = <0xf9824900 0x1a0 0xf9824000 0x800>;
    interrupts = <0 123 4 0 138 4>;
    clock-frequency = <19200000>;
};
```

The three-cell interrupt encoding is Apple's; confirm it against a ported device's ADT before
trusting it. The generator also owes `/chosen` (with `random-seed`, an NVRAM proxy, the DRAM
description and `osenvironment`), `/cpus`, `/defaults`, `/options`, and the
`/chosen/memory-map` placeholders DarwinBoot fills. `unique-chip-id` and `chip-id` must be
present or anything calling `gethostuuid()` blocks forever.

## Blockers

1. **Firmware has no Mu-Silicium target.** Every UniXNU device starts from a Mu-Silicium
   UEFI port, and Mu-Silicium has no MSM8992 target. The Lumia's own EDK2 firmware
   (Lumia950XLPkg) already reaches a menu, a framebuffer and the eMMC, and is the obvious
   base — but it is not the same tree, so UniXNUDxe and DarwinBoot have to be added to it,
   or a Lumia target has to be added to Mu-Silicium. Pick one before writing more of
   `[firmware]`.
2. **The interrupt controller is GICv2** (`qcom,msm-qgic2`, no redistributors). ArmCpu's GIC
   path is the generic GICv3 one on every ported board, so interrupts — including the eMMC
   completion interrupt — are gated on that work. Polling keeps the storage rung moving
   meanwhile.
3. **Display has no native driver.** Lumia950XLPkg says its framebuffer depends on the stock
   Qualcomm UEFI and that MDP is not fully implemented, so after `ExitBootServices` there is
   no output unless an MSM8992 MDSS driver exists. With no serial console either, this is the
   single most expensive missing piece.
4. **PSCI is HVC and incomplete** on 8992/8994: no `SYSTEM_OFF`, no `SYSTEM_RESET`. The ADT
   tool takes a `psci_method`, so this is expressible, but SMP bring-up is unproven.
5. **No macOS here.** The kernel step needs Xcode; use the upstream macOS CI workflow or a
   Mac. `unixnu` on Windows can plan, not build.
6. **No hardware in this loop.** Every remaining rung needs the phone.

## Building on a macOS runner

There is no Mac here, and the kernel step needs one — Xcode plus the iphoneos SDK. Upstream
solves this with a macOS GitHub Actions runner, so this repository carries
[.github/workflows/build.yml](.github/workflows/build.yml) with two jobs:

| Job | Runner | Trigger | What it does |
| --- | --- | --- | --- |
| `validate` | `ubuntu-latest` | every push and pull request | parses `board.toml` and the defconfig, compiles and runs the core test, checks that the build driver accepts the board, and that the `example` guard still refuses to build |
| `kernel` | `macos-latest` | manual (`workflow_dispatch`) | installs the Homebrew toolchain, fetches the Apple references, runs the XNU build, and compiles `NativeSdhci` against the compile records that pass leaves behind |

The `kernel` job exists to compile the driver — the artifact is `libNativeSdhci.a`, and
the run fails if it is missing. The kernel step can also stop on two known gaps of the
board, neither of them in the driver:

| Gap | Why |
| --- | --- |
| prelink gate | this board has no `[kernel.prelink]`, because the target IPSW and its original kernelcache are not chosen |
| final link | upstream's `offline-component-load-probes.patch` calls `native_legacy_display_probe()` from `bsd_init.c` without a `#if`, so a board with no display provider leaves it undefined |

The workflow tolerates **exactly** those two outcomes and fails on anything else: a
missing or extra undefined symbol, a compile error, or a failure that matches neither
case. Both tolerances come out once the prelink section exists and the board has a display
provider.

That job is the first real test of `NativeSdhci.cpp`: it compiles with `-Wall -Wextra
-Werror` against the recorded kernel ABI and asserts the object is an ARM64 `MH_OBJECT`.

Trigger it from **Actions → build → Run workflow**, or with the CLI:

```sh
gh workflow run build.yml
gh run watch
```

## Commands

```sh
# from the UniXNU checkout, with this directory beside it
py -3 unixnu defconfig lumia950_defconfig -D ../lumia950
py -3 unixnu status -D ../lumia950
py -3 unixnu build kernel --dry-run -D ../lumia950     # refuses: example = true

# host test of the driver core
cd lumia950 && scripts\test-core.bat
```

Once the firmware question is settled and the board is no longer `example = true`, the
kernel step is `unixnu build kernel` on macOS, and the hardware rung is a boot image plus the
phone.
