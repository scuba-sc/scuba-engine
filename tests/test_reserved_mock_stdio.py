"""Exercise the built host's real line protocol and copied-value SDK adapter."""
import json
import subprocess
import sys


def request(method, params=None):
    return json.dumps({"id": 1, "method": method, "params": params or {}})


entity = {"id": "18446744073709551615", "zone": "1", "flags": 7,
          "position": [1.5, -2, 3], "name": "synthetic-editor-object"}
lines = [request("mock.entity_put", entity),
         request("mock.entity_get", {"id": entity["id"]}),
         request("mock.graph"),
         request("mock.scalar_set", {"value": "-9223372036854775808"}),
         request("mock.scalar_tick", {"amount": "17"}),
         request("mock.scalar_snapshot"),
         request("mock.entity_put", {**entity, "position": [2, 4, 6]}),
         request("mock.entity_get", {"id": entity["id"]}),
         request("mock.entity_get", {"id": "18446744073709551616"}),
         "x" * 65537, request("mock.scalar_snapshot"), request("mock.status")]
run = subprocess.run([sys.argv[1]], input="\n".join(lines) + "\n", text=True,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
assert run.returncode == 0, run.stderr
replies = [json.loads(line) for line in run.stdout.splitlines()]
assert len(replies) == len(lines), run.stdout
assert replies[0]["result"] == entity
assert replies[1]["result"] == entity
assert replies[2]["result"]["entities"] == [entity]
assert replies[2]["result"]["zones"][0]["id"] == "1"
assert replies[5]["result"] == {"value": "-9223372036854775808", "ticks": "17"}
assert replies[7]["result"]["position"] == [2, 4, 6]
assert replies[8]["error"]["code"] == -32602
assert replies[9]["error"]["code"] == -32600
assert replies[10]["result"] == replies[5]["result"]
assert replies[11]["result"]["nativeUnavailable"] is True
assert replies[11]["result"]["mockOnly"] is True
assert replies[11]["result"]["scalarTickOnly"] is True
print("PASS: real stdio entity mutation/readback, exact integer copies, scalar ticks, bounded-input recovery and EOF retirement")
