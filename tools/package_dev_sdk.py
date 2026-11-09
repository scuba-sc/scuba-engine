#!/usr/bin/env python3
"""Package the coordinated core checkout with portable Git executable modes.

The core packager uses os.access(X_OK) to set archive modes, which cannot report
Git's executable bit on Windows. Use the clean checkout's index for tracked files;
all other access checks and all package contents remain the core packager's own.
"""

import argparse
import os
import runpy
import subprocess
import sys
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core_checkout", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("profile", nargs="?", choices=("host-source-sc", "legacy"),
                        default="host-source-sc")
    parser.add_argument("--development", action="store_true",
                        help="request the Core packager's unpublished development version")
    args = parser.parse_args()
    root = args.core_checkout.resolve()
    output = str(args.output_dir.resolve())
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
    sys.argv = [str(package), "--profile", args.profile, "--out", output]
    if args.development:
        sys.argv.append("--development")
    runpy.run_path(str(package), run_name="__main__")


if __name__ == "__main__":
    main()
