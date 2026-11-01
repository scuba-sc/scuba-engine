"""Verify candidate bytes and provenance before CMake extracts/builds them."""
import hashlib
import json
from pathlib import Path
import sys
import zipfile

archive, expected_hash, expected_commit = sys.argv[1:]
if len(expected_hash) != 64 or len(expected_commit) != 40:
    sys.exit("Expected a full SHA256 and full source commit")
if hashlib.sha256(Path(archive).read_bytes()).hexdigest() != expected_hash:
    sys.exit("Candidate archive SHA256 mismatch")
top = "sco-sdk-host-source-sc-2.4.0/"
with zipfile.ZipFile(archive) as kit:
    names = kit.namelist()
    if len(names) != len(set(names)):
        sys.exit("Duplicate archive paths")
    for name in names:
        if not name.startswith(top) or ".." in Path(name).parts or "\\" in name:
            sys.exit("Unsafe candidate archive path")
    compatibility = json.loads(kit.read(top + "COMPATIBILITY.json"))
    if compatibility["profile"] != "host-source-sc" or set(
        compatibility["source_commits"].values()
    ) != {expected_commit}:
        sys.exit("Candidate source provenance mismatch")
    checksums = {}
    for line in kit.read(top + "SHA256SUMS").decode().splitlines():
        digest, name = line.split("  ", 1)
        if name in checksums:
            sys.exit("Duplicate checksum path")
        checksums[name] = digest
        if hashlib.sha256(kit.read(top + name)).hexdigest() != digest:
            sys.exit("Candidate member checksum mismatch: " + name)
    if set(checksums) != {name[len(top):] for name in names} - {"SHA256SUMS"}:
        sys.exit("Candidate checksum inventory mismatch")
print(f"HOLD candidate verified: commit={expected_commit} sha256={expected_hash} members={len(names)}")
