"""The Lumia 950's panel, in one place.

Two different things need this number and they must not drift:

  * the menu's layout (scripts/check-payload.py picks portrait for a tall panel), and
  * any screenshot quoted as evidence, which is only this panel's if it is this size.

On the phone the resolution is not configured anywhere in this repository: the firmware's GOP
mode is 1440x2560, DarwinBoot hands that to the kernel as boot video, and iOS takes its
geometry from `PE_current_console` (upstream NativeMobileDisplay.cpp). On QEMU the equivalent
is the runner's `--display-size 1440x2560`. This module is the host-side copy of that fact,
for the layout report and for scripts/check-screenshot.py.
"""

WIDTH = 1440
HEIGHT = 2560
DEPTH = 32

PANEL = (WIDTH, HEIGHT)


def parse(text: str) -> tuple[int, int]:
    """`1440x2560` -> (1440, 2560), refusing anything that is not two sizes."""
    parts = text.lower().split("x", 1)
    if len(parts) != 2:
        raise ValueError(f"panel takes WIDTHxHEIGHT, not {text!r}")
    try:
        width, height = (int(part) for part in parts)
    except ValueError:
        raise ValueError(f"panel takes WIDTHxHEIGHT, not {text!r}") from None
    if not (0 < width < 1 << 14 and 0 < height < 1 << 14):
        raise ValueError(f"panel sides must be 1..16383, got {text!r}")
    return width, height


def describe(panel: tuple[int, int]) -> str:
    return f"{panel[0]}x{panel[1]}"
