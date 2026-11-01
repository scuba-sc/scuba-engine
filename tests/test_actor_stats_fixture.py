#!/usr/bin/env python3
"""Validate the selected actor-stats package pair and extracted plugin fixture.

The bf56 manifest declares the probe in game-sc, which legacy includes; the
host-source-sc profile includes core-source instead. This records package
selection, not a claim about product support or provider availability.

Example:
  python3 tests/test_actor_stats_fixture.py --host-archive HOST.zip \\
      --plugin-archive COMBINED.zip --plugin-root EXTRACTED_ROOT \\
      --source bf56d2879393b2b5a603256347fc1d3cf9a1a751
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile

HOST_SHA256 = "d41724ce46e34fa05a3ebfebc89ec3c16e6a2996061a183281dd17bcaa7fe0a8"
PLUGIN_SHA256 = "f7347ad09104ba64a697870c6245ff598949d3fb37cb84df43fe7e7138cba0e5"
HEADERS = ("include/sc_actor_stats.h", "include/sc_actor_stats_write.h")
PROBE = ("examples/actor_stats_probe/actor_stats_probe.c",
         "examples/actor_stats_probe/plugin.ini")


class FixtureError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise FixtureError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def inspect_archive(path, expected_hash, top, role, source):
    require(digest(path.read_bytes()) == expected_hash, f"{path}: archive SHA-256 mismatch")
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        require(len(names) == len(set(names)), f"{path}: duplicate archive entries")
        require(all(n.startswith(top + "/") for n in names), f"{path}: unexpected archive root")
        files = {n[len(top) + 1:]: archive.read(n) for n in names if not n.endswith("/")}
        require("SHA256SUMS" in files, f"{path}: missing SHA256SUMS")
        sums = {}
        for line in files["SHA256SUMS"].decode("utf-8").splitlines():
            match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
            require(match is not None, f"{path}: malformed SHA256SUMS line")
            expected, name = match.groups()
            require(name not in sums, f"{path}: duplicate checksum for {name}")
            sums[name] = expected
        require(set(sums) == set(files) - {"SHA256SUMS"}, f"{path}: incomplete checksum inventory")
        for name, expected in sums.items():
            require(digest(files[name]) == expected, f"{path}: checksum mismatch for {name}")
        compat = json.loads(files["COMPATIBILITY.json"])
        require(compat.get("profile") == role, f"{path}: profile mismatch")
        require(compat.get("schema_version") == 1 and compat.get("core_sdk_version") == "2.4.0"
                and compat.get("game_overlay_version") == "0.8.0"
                and compat.get("plugin_abi_version") == "1.1"
                and compat.get("base_core_sha256") is None,
                f"{path}: compatibility coordinate mismatch")
        require(compat.get("source_commits") == {"core": source, "game-sc": source},
                f"{path}: source coordinate mismatch")
        return files


def validate(host_path, plugin_path, plugin_root, source):
    host = inspect_archive(host_path, HOST_SHA256, "sco-sdk-host-source-sc-2.4.0",
                           "host-source-sc", source)
    plugin = inspect_archive(plugin_path, PLUGIN_SHA256, "sco-sdk-2.4.0", "legacy", source)
    for name in HEADERS:
        require(name in host and name in plugin, f"missing public header: {name}")
        require(host[name] == plugin[name], f"public header differs between archives: {name}")
    for name in HEADERS + PROBE:
        require(name in plugin, f"plugin archive missing {name}")
        require((plugin_root / name).is_file(), f"extracted plugin fixture missing {name}")
        require((plugin_root / name).read_bytes() == plugin[name],
                f"extracted plugin fixture differs from archive: {name}")
    for name in PROBE:
        require("sdk/" + name not in host and name not in host,
                f"host-source-sc unexpectedly contains probe: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host-archive", type=Path, required=True)
    parser.add_argument("--plugin-archive", type=Path, required=True)
    parser.add_argument("--plugin-root", type=Path, required=True)
    parser.add_argument("--source", required=True, help="expected full source commit for both roles")
    args = parser.parse_args()
    try:
        require(re.fullmatch(r"[0-9a-f]{40}", args.source) is not None,
                "source must be a full lowercase Git commit")
        validate(args.host_archive, args.plugin_archive, args.plugin_root, args.source)
        # The same immutable archive must reject an incorrect source coordinate.
        wrong_source = ("0" if args.source[0] != "0" else "1") + args.source[1:]
        try:
            inspect_archive(args.host_archive, HOST_SHA256, "sco-sdk-host-source-sc-2.4.0",
                            "host-source-sc", wrong_source)
        except FixtureError as error:
            require("source coordinate mismatch" in str(error), f"unexpected negative-control failure: {error}")
        else:
            raise FixtureError("negative control accepted an incorrect source coordinate")
    except (FixtureError, OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"actor-stats fixture: FAIL: {error}", file=sys.stderr)
        return 1
    print("actor-stats fixture: archive hashes, compatibility, checksums, headers and packaged probe verified; incorrect source rejected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
