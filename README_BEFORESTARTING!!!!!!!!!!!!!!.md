# STOP. READ THIS BEFORE STARTING ANYTHING.

## The GitHub Action is the compiler. Not this machine.

There is **no macOS here**. `scripts/build-arm-driver.py` replays the compile command XNU
recorded and can only run where Xcode and the iphoneos SDK exist. So:

> **A driver that has only been edited is not compiled. A driver is compiled when the
> `kernel` job on GitHub Actions says so. Push, dispatch the job, read the log.**

Do not say a driver works, or even builds, because `py_compile`, `g++` on the host test and
`bash -n` passed. Those test the *core* and the *scripts*. They do not compile
`NativeSdhci.cpp`, `NativeAdreno418.cpp` or anything else that includes `<IOKit/...>`.

## Run it

```sh
# The job that compiles every driver listed in [kernel] archives of board.toml:
gh workflow run build.yml -f target=kernel --repo ManGuy-Legend/UniXNU-lumia950

# See it, follow it, get the artifact:
gh run list --workflow build.yml --repo ManGuy-Legend/UniXNU-lumia950
gh run watch <run-id> --repo ManGuy-Legend/UniXNU-lumia950
gh run download <run-id> -n lumia950-kernel --repo ManGuy-Legend/UniXNU-lumia950
```

`target` is `payload`, `kernel` or `both`; the default is `payload`, so **name `kernel`
explicitly** or you get the EFI build and no driver compile at all.

| Job | Runs when | What it is the only proof of | Artifact |
| --- | --- | --- | --- |
| `validate` | every push to `main`, every PR, **and every dispatch** | `board.toml` and defconfig parse, all three core host tests pass (39 eMMC + 184 GPU + 49 display checks), the GPU addon checker refuses all 15 deliberate defects, ADT generates and passes `check-adt.py` (9 mutants refused), `unixnu` accepts the board, `example` is still refused | none |
| `payload` | dispatch with `target=payload` or `both` | `UniXNU.efi`, `DarwinBoot.efi` and the staged `out/payload` tree | `lumia950-payload` |
| `kernel` | dispatch with `target=kernel` or `both` | **every driver compiles against the recorded XNU ABI with `-Werror`**, the kernel links, and the pinned 16.7.16 kernelcache merges | `lumia950-kernel` |

## Run it even when you are not sure you need to

If a change *might* have broken compilation, **just dispatch the job**. Actions minutes are
free on a public repository, the run is the cheapest possible smoke test, and a red log that
names a file and a line is worth more than an hour of reading the diff. There is no quota to
spend here and no reason to hoard a run. Same for the payload job: if the menu or the loader
changed, build it and look at the artifact.

Do not ask "should I run CI?" — run it, then read the log.

## The traps, in the order people fall into them

1. **A new driver is invisible to CI until it is in `[kernel] archives` in `board.toml`.**
   `drivers/Foo/` existing is not enough. Add `"build-arm-driver.py Foo"` and the matching
   `COMPONENTS` entry in `scripts/build-arm-driver.py`, or the job happily compiles everything
   except your new file and goes green.
2. **A green `validate` is not a compiled driver.** It never includes `<IOKit/...>`. Read the
   `kernel` job's log for your class name, not the checkmark.
3. **A new ADT node has to pass `check-adt.py` first.** `validate` runs the generator and the
   checker, including the checker's `--self-test`; a node the checker rejects makes the
   `kernel` job pointless because the phone would never get the tree.
4. **The archive is built, not linked.** `-Wl,-force_load` and the `KernelConfigTables.cpp`
   personality are separate steps (README, "Linking it into the kernel"). A compiled driver
   with no personality is never matched, and nothing on this machine can tell you that.
5. **The `kernel` job is long (macOS + Xcode).** Dispatch it as soon as a driver change is
   ready, do not stack five changes onto one run whose failure will not name the culprit.
6. **A core in the archive is not a rung.** `CommandStream.cpp` compiles into
   `native-adreno418` and nothing calls it: rungs 3–5 are arithmetic proven on this machine,
   and the ladder constant `kCoreRung` says exactly how far that goes. Reading a core test
   as "the GPU works" is the single easiest lie in this repository.
7. **The display driver publishes no framebuffer, on purpose.** `NativeMdss` runs at rung 1:
   it maps and reads. A green `validate` says its arithmetic is right, not that anything is
   on the panel. Rung 4 is where a picture could appear and rung 5 where iOS could draw; the
   DSI PHY's PLL table and the panel's init sequence are still absent, and they are not
   things to guess at.
8. **A window has to contain the registers read through it.** The MMSS window was declared
   4 KiB while the GDSCs sit at `0x4024`; the read went past the mapping. Both core tests now
   assert the offsets are inside their windows — do the same for any node you add.

## Resolution and screenshots

This board's panel is **1440x2560**. iOS takes its geometry from the boot video the kernel is
handed (`PE_current_console` in upstream `NativeMobileDisplay.cpp`), not from anything in this
repository, so on the phone the number comes from the firmware's GOP mode. A QEMU run must be
given it explicitly: `--display-size 1440x2560` on the UniXNU runner. A screenshot that is not
1440x2560 is not this panel: check it with `python3 scripts/check-screenshot.py FILE` before
quoting it as evidence.

## The short version

```
edited a driver  ->  push  ->  gh workflow run build.yml -f target=kernel  ->  read the log
edited the menu  ->  push  ->  gh workflow run build.yml -f target=payload ->  take the artifact
edited a script  ->  push  ->  validate is enough
```

Nothing in this repository is verified on this machine. The Action is the verification.
