"""Bounded Linux artifact check; this does not certify Windows or arbitrary modules."""
import re
import subprocess
import sys

nm, readelf, executable = sys.argv[1:]


def output(*args):
    return subprocess.check_output(args, text=True)


symbols = output(nm, "-a", "-C", "--defined-only", executable)
authority = re.findall(r"^[0-9a-f]+ [tT] sco::reserved::CreateExecutableAuthority\(\)$", symbols, re.M)
if len(authority) != 1:
    sys.exit("expected exactly one executable authority definition")
if re.search(r"sco::(?:app|plugins|hook|host|game)::|\bsco_(?:query|provide|raw|tasks|events|commands)", symbols):
    sys.exit("unexpected legacy implementation in curated host")
if "ReaderDrainObserved" in symbols or "reserved_copy_probe::" in symbols:
    sys.exit("test-only output-copy instrumentation in production host")
dynamic = output(readelf, "-d", executable)
if re.search(r"\((?:RPATH|RUNPATH|AUDIT|DEPAUDIT)\)", dynamic):
    sys.exit("unexpected dynamic search path or audit loader")
needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
allowed = {"libstdc++.so.6", "libgcc_s.so.1", "libm.so.6", "libc.so.6", "libpthread.so.0", "libdl.so.2"}
if not needed or set(needed) - allowed:
    sys.exit("unexpected dependency: " + repr(needed))
exports = output(nm, "-D", "-C", "--defined-only", executable)
if "sco::" in exports or "scuba::" in exports:
    sys.exit("module-free composition must not export project APIs")
print("PASS: one executable authority; no legacy implementation, project exports, loader overrides; reviewed system dependencies", needed)
