#!/usr/bin/env python3
"""Optional acceptance gate: requires tesseract on the inspecting host, not the build host."""
import subprocess
import sys

result = subprocess.run(["tesseract", sys.argv[1], "stdout", "--psm", "11"],
                        check=True, capture_output=True, text=True)
text = result.stdout.lower()
required = ("chat", "no multiplayer session", "room", "#general", "messages", "message", "send")
missing = [label for label in required if label not in text]
print(result.stdout)
if missing:
    raise SystemExit("missing rendered plugin labels: " + ", ".join(missing))
print("plugin visual label gate PASS (input value additionally checked by fixture and visual inspection)")
