# UniXNU-lumia950

Device repository for the Microsoft Lumia 950 (MSM8992, Snapdragon 808), targeting the
UniXNU stack: the phone's own AArch64 UEFI, the UniXNU boot manager, DarwinBoot and a
patched XNU running an iOS userland.

This is a **P1 research skeleton**. `board.toml` keeps `example = true`, so the build driver
refuses to build or boot-image it — that is the intended guard, not an oversight. What is
here is the part that could be built and checked without the phone: the device repository
structure, the register map with its sources, the eMMC driver's host-testable core, and now
the chainload payload with its build and configuration checks.

## The boot route

The phone boots through its **own** UEFI, not through a firmware this repository builds.
Lumia950XLPkg already brings the MSM8992 up far enough to show a menu and to start EFI
applications from FAT on EFIESP — that is how Windows boots on these phones. The payload is
therefore plain files on that partition, launched from that menu:

| Path on EFIESP | File | Built by |
| --- | --- | --- |
| `\EFI\UniXNU\UniXNU.efi` | the menu | `package/build.sh` in the UniXNU checkout |
| `\EFI\UniXNU\unixnu.toml` | its configuration | [payload/EFI/UniXNU/unixnu.toml](payload/EFI/UniXNU/unixnu.toml) |
| `\EFI\UniXNU\Drivers\DarwinBoot.efi` | the loader, named in the config only once a kernel exists | `DarwinBootPkg/DarwinBoot.dsc` |

Mu-Silicium is not the base: it has no MSM8992 target, and its installation model is an
Android boot image that an Android bootloader loads with `fastboot`, which a Lumia does not
have. Chainloading also keeps the experiment reversible — nothing is flashed.

Two firmware services decide whether the menu can work. Both were measured on the 950 on
2026-10-23:

| Service | It decides | Observed |
| --- | --- | --- |
| `EFI_GRAPHICS_OUTPUT_PROTOCOL` | whether the menu draws | present: the logo and the three cards drew on the 1440x2560 panel |
| `EFI_SIMPLE_TEXT_IN_PROTOCOL` | whether keys reach the menu | present: the volume keys move the selection |

UniXNU's menu reads `ConIn`; touch arrives as an absolute pointer, which the menu does not
consume. That the firmware publishes the buttons as a text input device is measured now,
not assumed.

### Testing it on the phone

Nothing in the Lumia firmware runs an application from `\EFI\UniXNU` by name. It boots the
first `\EFI\BOOT\BOOTAA64.EFI` it can load from any FAT volume it has mounted
(`Application/BootSplashApp/SplashApp.c`, `DiscoverAndBootApp`), and falls back to its own
boot manager menu only when that fails. That removable-media name is the entry point, so the
payload is staged with the menu under both names:

| On EFIESP | From the artifact |
| --- | --- |
| `\EFI\BOOT\BOOTAA64.EFI` | `EFI/BOOT/BOOTAA64.EFI` — the same `UniXNU.efi` |
| `\EFI\UniXNU\unixnu.toml` | `EFI/UniXNU/unixnu.toml` |
| `\EFI\UniXNU\Drivers\DarwinBoot.efi` | not loaded until a kernel entry exists |

Unlocking with WPInternals and installing the Lumia950XLPkg UEFI (`UEFI.elf` in the EFIESP
root, the Boot Shim application, a BCD entry for it) come first and are not in this
repository. That is the one step in the whole plan that can brick the phone.

How that entry is selected matters on these stores: the phone's boot manager has
`displaybootmenu No`, `timeout 0` and a persistent `bootsequence`, so adding the entry to
`displayorder` does nothing. Pointing `{bootmgr} bootsequence` at the BootShim entry is
what actually boots it, and pointing it back at the mobile-startup GUID
(`{01de5a27-8705-40db-bad6-96fa5187d4a6}`) is the revert.

1. Mount EFIESP in Mass Storage Mode and back up `\EFI\BOOT`. Windows Phone's own loader is
   ARM32 (`BOOTARM.EFI`), so `BOOTAA64.EFI` is usually free — but if one is already there it
   is somebody else's fallback loader, so rename it rather than overwriting it.
2. Copy `out/payload/EFI` over `\EFI`. Nothing else on the partition changes.
3. Reboot and leave the splash alone. Expect the UniXNU menu for 20 s; "Exit to firmware"
   then opens the firmware's own menu, the one with System Info.
4. To go back to Windows, mount EFIESP again and delete `\EFI\BOOT\BOOTAA64.EFI`. While it
   is there, that file is what the firmware starts.

What each outcome means:

| What happens | Likely cause |
| --- | --- |
| no menu, the firmware menu appears after a few seconds | the image did not load: check the copy is 95232 bytes and AArch64 |
| text console instead of cards | GOP is present but the graphics path is not; see `[ui] console` |
| menu draws, `Restart` / `Power off` do nothing | MSM8992 has no PSCI SYSTEM_RESET or SYSTEM_OFF; those entries call the firmware's `ResetSystem` |
| menu draws, `Restart` or `Power off` does nothing | the firmware's `ResetSystem` for that action is a stub |

The firmware registers a `UEFI Shell` boot option from a file GUID in its volume and maps it
to Volume Up (`Library/PlatformBootManagerLib/PlatformBm.c`). The release build has no Shell
in that volume, so the volume buttons are only a menu input, not a way into a shell.

## State

| Layer | State |
| --- | --- |
| Device repository | `board.toml`, defconfig, `NativeSdhci`, build and test scripts; accepted by `unixnu status` as `[EXAMPLE]` |
| UEFI firmware | not built here; the phone's own Lumia950XLPkg was observed starting `\EFI\BOOT\BOOTAA64.EFI` |
| Chainload payload | built by the `payload` job ([run 39869583766](https://github.com/ManGuy-Legend/UniXNU-lumia950/actions/runs/39869583766)), artifact `lumia950-payload`; loaded on the phone on 2026-10-23 through the BCD `bootsequence` entry |
| UniXNU menu | drawn on the 1440x2560 panel in the portrait card layout, three action entries, volume keys move the selection; touch is not consumed |
| DarwinBoot | `DarwinBoot.efi` builds standalone from its DSC; not yet loaded |
| ADT | generated and checked on the host (11976 B, six CPUs, NVRAM proxy); not yet handed to a kernel |
| XNU entry | blocked on the kernel line and the GICv2 question |
| Storage driver | written, host-tested, never run on hardware |
| Root volume and userland | not started |

## What is verified, and how

| Check | Command | Result |
| --- | --- | --- |
| SDHCI core logic (39 checks) | `scripts\test-core.bat` (Windows) or the `g++` line inside it | all pass |
| Board and defconfig parse | `unixnu defconfig lumia950_defconfig -D lumia950` | accepted |
| Module resolution | `unixnu status -D lumia950` | `driver NativeSdhci device` |
| Incomplete-board guard | `unixnu build kernel --dry-run -D lumia950` | refused, as designed |
| CI workflow file | YAML parse, `bash -n` on all 17 run blocks | clean |
| Chainload payload configuration | `python3 scripts/check-payload.py` | accepted; the keys and enums are the ones `UniXNULib/Config.c` reads |
| Payload staging | `stage-esp.py` and `check-payload.py --payload-dir` on fixture EFI files | tree staged at `EFI/UniXNU` and accepted |
| Chainload payload build | `payload` job, [run 39869583766](https://github.com/ManGuy-Legend/UniXNU-lumia950/actions/runs/39869583766) | `UniXNU.efi` and `BOOTAA64.EFI` 95232 B, `DarwinBoot.efi` 26112 B, config 2119 B; staged tree checked, artifact uploaded |
| On-phone chainload | mass-storage copy of `out/payload/EFI`, BCD `bootsequence` pointed at the `\EFI\boot\BootShim.efi` entry | Lumia splash then the UniXNU menu with three cards; volume keys move the selection (2026-10-23) |
| ADT generator and checker | `python3 scripts/make-lumia950-adt.py --dram-size 0xC0000000 --unique-chip-id ... --chip-id 0x8015` then `python3 scripts/check-adt.py build/lumia950.adt` | 11976-byte tree with 6 CPUs accepted; the checker's `--self-test` rejects 7 deliberate defects, including a broken NVRAM Adler-32 |
| Kernel-ABI compile of `NativeSdhci.cpp` | `kernel` job, [run 39858813829](https://github.com/ManGuy-Legend/UniXNU-lumia950/actions/runs/39858813829) | compiled clean under `-Werror`; archive holds two ARM64 `MH_OBJECT` members |
| XNU kernel link | same run | links with the board archives |

The driver's *kernel* half (`NativeSdhci.cpp`) compiles against the recorded kernel ABI,
and the archive holds `NativeSdhci.o` and `SdhciCore.o`, both ARM64 `MH_OBJECT`; the
archive is not byte-reproducible, because `ar` records member timestamps, so it is not
quoted as an identity.

The kernel links too. Upstream calls `native_legacy_display_probe()` from `bsd_init.c`
without the `XNU_NATIVE_LEGACY_DISPLAY` guard every other call site carries, so the symbol
is referenced even with the display provider unselected. `drivers/NativeLegacyDisplay`
defines both probe entry points and does nothing else — no framebuffer, no provider — and
the defconfig force-loads it through the kernel script's `XNU_EXTRA_LD_ARCHIVES` hook.

Prelink is paused for a missing image, not a version. The source pin is now the 16.x tree,
but the board has no built kernel and no `[kernel.prelink]`, so there is nothing to merge
with the kexts of the 16.7.16 kernelcache yet. The candidate produced while the pin was
17.x (12 components) answered the tool and not the requirement: those kexts belong to a
different XNU ABI.

The kernel is on 16.x now. `feeds.toml` pins `xnu-8796.141.3` — a source of the same name
replaces the base repository's 17.x entry — and `patches/xnu-8796/` is that line's overlay.
Of the 54 base patches, 47 apply unchanged; five are replaced by their 8796 form (the
non-Apple tunables guards, the external archive hook, the machine layer, the native AES
personalities and the 64-bit physical address shift); three are added: `clang-trivial-auto-var-init`,
because 16.x passes a clang flag the runner's clang no longer knows; `clang-uio-stackbuf`,
which declares the uio stack buffer the way later XNU lines do, because current clang rejects an
array of a one-byte type carrying an eight-byte alignment, which is what every `bsd/` call site
used to say; and `driverkit-rpc-kernel-content`, which keeps the kernel `IORPCMessage` in the rpc
because the runner's `iig` generates DriverKit dispatch code that reads it instead of deriving the
message from the mach header; and `skip.txt` names the seven this line
has no mechanism for, each with its reason: no page-guard zones, no SPTM pmap, no
exclave owner chain, no boot-progress console. NativePrelink's kit patches carry their
line in the file name, so 8796 does not take them. The build prints every skip and refuses
a skip list that names a patch the base tree no longer has, so an omission cannot become a
silent hole. `[kernel.prelink]` stays out until a 16.x kernel image exists to merge.

The first 16.x compile stops in `bsd/`, not at the gate. The patch set applies and the
bootstrap pass compiles for about three minutes before the first error, so the job fails with
`out/<device>/logs/kernel-bootstrap.log` in its artifact — the log the step now uploads and
greps for `error:` on a failure, because the driver echoes only the last thirty lines of a
command. What that log first reported, an aligned-char uio stack buffer, is the
`clang-uio-stackbuf` addition above.

## The reference image

`[ipsw]` pins iPhone X, iOS 16.7.16 (build 20H392), device class d221ap.

| Question | Answer |
| --- | --- |
| Why this release | iOS 16 is the requirement, and 16.7.16 is the last 16.x release and the one the community ports reached userland with |
| Why iPhone10,6 | the phone-shaped identity in a restore that serves iPhone10,3 and iPhone10,6; both use `kernelcache.release.iphone10b` |
| How it was checked | product, version, build, device class, chip `0x8015` and board `0x0e` were read from the image's own `BuildManifest.plist` over HTTP range requests |
| What is not pinned | the kernelcache hash, because nothing consumes the cache until a 16.x kernel route exists; `[kernel.prelink]` returns with that hash |

## Layout

```
lumia950/
  board.toml                        board description, incomplete by design
  configs/lumia950_defconfig        kernel configuration
  drivers/NativeLegacyDisplay/      probe-symbol archive; not a driver, and not a display
  drivers/NativeSdhci/              the eMMC driver
    BoardConstants.h                Lumia 950 addresses, each with its source
    SdhciRegs.h                     SDHCI register map (spec layout, not guessed)
    SdhciCore.{h,cpp}               clock, CSD, EXT_CSD and ADMA2 arithmetic
    NativeSdhci.{h,cpp}             IOKit driver: warm handoff, PIO reads
    Info.plist                      personality for `qcom,msm8994-sdhci`
    Test/check-sdhci-core.cpp       host test for the core
  payload/EFI/UniXNU/unixnu.toml    chainload config, in the layout it deploys as
  scripts/build-arm-driver.py       compile a driver against the recorded XNU ABI
  scripts/build-payload.sh          EDK II build and staging for the two EFI binaries
  scripts/check-payload.py          config and staged-tree check
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

[scripts/make-lumia950-adt.py](scripts/make-lumia950-adt.py) emits the tree and
[scripts/check-adt.py](scripts/check-adt.py) parses it back before DarwinBoot ever sees it.
The driver matches an `/arm-io` child:

```
/arm-io/sdhci@f9824900 {
    name = "sdhci@f9824900";
    compatible = "qcom,msm8994-sdhci", "qcom,sdhci-msm-v4";
    reg = <0xf9824900 0x1a0 0xf9824000 0x800>;   # two windows, 64-bit values
    interrupts = <0 123 4 0 138 4>;
    clock-frequency = <19200000>;
};
```

The cell layout is the one in [scripts/qemu-entry/make-dt.py](../unixnu/scripts/qemu-entry/make-dt.py):
two address cells and two size cells, every address and size a 64-bit little-endian value.
That fixture is the only ADT this project has watched a real `ArmDTPlatform::configure`
accept, so it is the layout to follow. The earlier four-cell `reg` sketch in this file assumed
one-cell addressing and is superseded. The three-cell interrupt encoding is still Apple's,
confirmed only structurally: no driver here has consumed one yet.

The generator emits `/chosen` (random seed, CHRP NVRAM proxy, DRAM base and size,
`osenvironment`, and the `unique-chip-id`/`chip-id` pair), `/cpus` (six cores, boot CPU
first), `/psci`, `/defaults`, `/options`, `/product`, and `/arm-io` with the GICv2 and SDHCI
windows. The NVRAM proxy follows the layout
`iokit/Kernel/IONVRAMCHRPHandler.cpp` parses, down to the Adler-32 it panics on, and the
checker recomputes that checksum rather than trusting the generator. `unique-chip-id` and
`chip-id` are required because anything calling `gethostuuid()` blocks forever without them;
the checker refuses a tree that omits them unless explicitly told to allow it.

What the tree does not solve: `intc` describes a real GICv2 while ArmCpu's interrupt path is
GICv3-only (blocker 2), PSCI is the MSM8992 subset without SYSTEM_OFF/SYSTEM_RESET
(blocker 4), and no display node is emitted because there is no MDSS driver to match it.

## Blockers

1. **The route is decided and the firmware environment is measured.** The phone boots its
   own Lumia950XLPkg UEFI and that firmware starts `UniXNU.efi` from EFIESP. Because this
   repository builds no firmware volume, `[firmware]` and `unixnu bootimg` stay unused. The
   chain ran on the phone on 2026-10-23 — BCD `bootsequence` → BootShim → `UEFI.elf` →
   `\EFI\BOOT\BOOTAA64.EFI` → the menu — with GOP drawing the cards and `ConIn` delivering
   volume-key input, and no `UniXNUDxe` entry: `SplashApp`'s `DiscoverAndBootApp` finds the
   removable-media name. `example = true` stays until the kernel route opens, not because
   the payload is unrun.
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

## The CI jobs

There is no Mac here, and the kernel step needs one — Xcode plus the iphoneos SDK. Upstream
solves that with a macOS runner, and the payload needs an EDK II toolchain that Windows does
not have either, so this repository carries
[.github/workflows/build.yml](.github/workflows/build.yml) with three jobs:

| Job | Runner | Trigger | What it does |
| --- | --- | --- | --- |
| `validate` | `ubuntu-latest` | every push and pull request | parses `board.toml` and the defconfig, compiles and runs the core test, checks that the build driver accepts the board, and that the `example` guard still refuses to build |
| `payload` | `ubuntu-latest` | manual, `target=payload` (the default) | installs clang, lld and the EDK II host tools, clones Mu_Basecore, builds `UniXNU.efi` and `DarwinBoot.efi`, stages `out/payload` and checks it |
| `kernel` | `macos-latest` | manual, `target=kernel` | installs the Homebrew toolchain, fetches the Apple references, runs the XNU build, compiles `NativeSdhci`, and links |

UniXNU's 16.x support travels as `ci/unixnu-16x.patch`: the overlay and the per-line patch
selection belong in UniXNU, but this account cannot push to `UniXNU/UniXNU`. Both CI jobs
apply the shim to their checkout and treat an already-applied shim as a no-op, so the moment
the change is upstream the shim can be deleted without touching the jobs. The build driver
still comes from UniXNU `main`; only the patch selection and its overlay are carried.

The `kernel` job compiles and links the 16.x source line, then stops at the prelink gate
because the board has no `[kernel.prelink]` (there is no kernel image yet to merge with the
IPSW kernelcache). That one stop is tolerated and every other failure fails the job, so the
archive and the link stay verified. When the kernel exists, the prelink section and its pin
return and the gate goes away.

That job is the first real test of `NativeSdhci.cpp`: it compiles with `-Wall -Wextra
-Werror` against the recorded kernel ABI and asserts the object is an ARM64 `MH_OBJECT`.

Trigger a job from **Actions → build → Run workflow**, or with the CLI. The dispatch input
picks the job, so a payload build does not also start the six-hour macOS one:

```sh
gh workflow run build.yml -f target=payload
gh run watch

gh workflow run build.yml -f target=kernel
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

The hardware rung is the staged payload on EFIESP, not a boot image: `unixnu bootimg` packs a
firmware volume for an Android bootloader, and a Lumia has none. The kernel step stays
`unixnu build kernel` on macOS.
