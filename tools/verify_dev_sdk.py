#!/usr/bin/env python3
"""Verify the coordinated development host-source archive before consuming it."""

import hashlib
import json
import sys
import zipfile
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: verify_dev_sdk.py ARCHIVE SHA256 CORE_COMMIT")
    archive, expected_hash, expected_commit = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    actual_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
    if actual_hash != expected_hash:
        raise SystemExit(f"development archive SHA-256 mismatch: {actual_hash}")

    prefix = "sco-sdk-host-source-sc-2.4.0/"
    with zipfile.ZipFile(archive) as kit:
        names = kit.namelist()
        if len(names) != len(set(names)) or any(not name.startswith(prefix) for name in names):
            raise SystemExit("duplicate or unexpected archive entry")
        compatibility = json.loads(kit.read(prefix + "COMPATIBILITY.json"))
        commits = compatibility.get("source_commits", {})
        if (compatibility.get("profile") != "host-source-sc" or
                commits.get("core") != expected_commit or commits.get("game-sc") != expected_commit):
            raise SystemExit("development archive source metadata does not match the pinned commit")
        checksums = kit.read(prefix + "SHA256SUMS").decode("utf-8").splitlines()
        verified = set()
        for line in checksums:
            digest, separator, name = line.partition("  ")
            entry = prefix + name
            if not separator or entry in verified or entry not in names:
                raise SystemExit("invalid archive checksum inventory")
            if hashlib.sha256(kit.read(entry)).hexdigest() != digest:
                raise SystemExit(f"archive entry checksum mismatch: {name}")
            verified.add(entry)
        if verified != set(names) - {prefix + "SHA256SUMS"}:
            raise SystemExit("archive checksum inventory is incomplete")
    print(f"verified development archive {actual_hash}, source {expected_commit}, {len(verified)} entries")


if __name__ == "__main__":
    main()
