#!/usr/bin/env python3
"""Package the coordinated core checkout with portable Git executable modes.

The core packager uses os.access(X_OK) to set archive modes, which cannot report
Git's executable bit on Windows. Use the clean checkout's index for tracked files;
all other access checks and all package contents remain the core packager's own.
"""

import os
import runpy
import subprocess
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: package_dev_sdk.py CORE_CHECKOUT OUTPUT_DIR")
    root = Path(sys.argv[1]).resolve()
    output = str(Path(sys.argv[2]).resolve())
    listing = subprocess.check_output(["git", "-C", str(root), "ls-files", "--stage", "-z"])
    executable = {}
    for entry in listing.split(b"\0"):
        if not entry:
            continue
        metadata, name = entry.split(b"\t", 1)
        mode, _, stage = metadata.split(b" ")
        if stage != b"0":
            raise SystemExit("unmerged core checkout index")
        path = os.path.normcase(os.path.abspath(root / os.fsdecode(name)))
        executable[path] = mode == b"100755"

    original_access = os.access

    def index_access(path, mode, *args, **kwargs):
        key = os.path.normcase(os.path.abspath(path))
        if mode == os.X_OK and key in executable:
            return executable[key]
        return original_access(path, mode, *args, **kwargs)

    os.access = index_access
    package = root / "sdk" / "package.py"
    sys.argv = [str(package), "--profile", "host-source-sc", "--out", output]
    runpy.run_path(str(package), run_name="__main__")


if __name__ == "__main__":
    main()
