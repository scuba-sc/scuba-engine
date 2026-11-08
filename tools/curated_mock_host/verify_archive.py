"""Verify host-source archive bytes and provenance before CMake extracts/builds them."""
import hashlib
import json
import re
from pathlib import Path
import sys
import zipfile

archive, expected_hash, expected_commit = sys.argv[1:]
if len(expected_hash) != 64 or len(expected_commit) != 40:
    sys.exit("Expected a full SHA256 and full source commit")
if hashlib.sha256(Path(archive).read_bytes()).hexdigest() != expected_hash:
    sys.exit("Host-source archive SHA256 mismatch")
match = re.fullmatch(r"sco-sdk-host-source-sc-([A-Za-z0-9][A-Za-z0-9._+-]*)\.zip", Path(archive).name)
if not match:
    sys.exit("Unexpected host-source archive filename")
top = "sco-sdk-host-source-sc-" + match.group(1) + "/"
with zipfile.ZipFile(archive) as kit:
    names = kit.namelist()
    if len(names) != len(set(names)):
        sys.exit("Duplicate archive paths")
    for name in names:
        if not name.startswith(top) or ".." in Path(name).parts or "\\" in name:
            sys.exit("Unsafe host-source archive path")
    compatibility = json.loads(kit.read(top + "COMPATIBILITY.json"))
    if (compatibility["profile"] != "host-source-sc" or
            compatibility.get("core_sdk_version") != match.group(1) or set(
        compatibility["source_commits"].values()
    ) != {expected_commit}):
        sys.exit("Host-source archive provenance mismatch")
    checksums = {}
    for line in kit.read(top + "SHA256SUMS").decode().splitlines():
        digest, name = line.split("  ", 1)
        if name in checksums:
            sys.exit("Duplicate checksum path")
        checksums[name] = digest
        if hashlib.sha256(kit.read(top + name)).hexdigest() != digest:
            sys.exit("Host-source archive member checksum mismatch: " + name)
    if set(checksums) != {name[len(top):] for name in names} - {"SHA256SUMS"}:
        sys.exit("Host-source archive checksum inventory mismatch")
print(f"Verified host-source archive: commit={expected_commit} sha256={expected_hash} members={len(names)}")
