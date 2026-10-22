#!/usr/bin/env python3
"""Clear the incomplete-board guard in a CI scratch copy of board.toml.

The committed board keeps `example = true` so the build refuses to pretend the port
is complete. CI builds from a throwaway copy with the guard cleared, and this is the
only place that clears it. It edits the key line, not the prose that mentions it, and
re-reads the file to prove the guard is really gone.
"""
import pathlib
import sys
import tomllib


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: ci-clear-example.py PATH/TO/board.toml")

    path = pathlib.Path(sys.argv[1])
    lines = path.read_text().splitlines(keepends=True)

    # Only the bare key line counts: the header comment quotes "example = true" too.
    hits = [index for index, line in enumerate(lines) if line.strip() == "example = true"]
    if len(hits) != 1:
        raise SystemExit(f"{path}: expected one 'example = true' line, found {len(hits)}")

    lines[hits[0]] = "# example guard cleared in the CI scratch copy\n"
    text = "".join(lines)
    path.write_text(text)

    if tomllib.loads(text)["board"].get("example"):
        raise SystemExit(f"{path}: the guard is still set after the edit")

    print(f"{path}: guard cleared for this run")


if __name__ == "__main__":
    main()
