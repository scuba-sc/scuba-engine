# Native NetSandbox dock (bounded slice)

## Application integration

The candidate integrates the dock into the editor's View menu and supplies the hostd
executable path from its build. The dock persists across frames and owns its worker and
children. The executable must be available at that path: only hostd implements `--net-peer`.
Packaging and executable discovery still require platform acceptance.

## Scope and ownership

Browser `studio/src/panels/netsim/index.ts` and `docs/net-sim.md` establish slots 1-8, 2-8 initial peers, 400ms polling, host-only stop, and 1-100 burst limits. This dock directly calls the real `Broker::{Start,Snapshot,Join,Leave,Peer,Stop}`, never mocks networking or drives the editor kernel's network singleton.

One worker owns its broker. Readback is a locked bounded snapshot (at most eight peers, bounded displayed strings), no broker I/O under that lock. Admission sets busy before returning; there is one in-flight command and no queued backlog. UI polling is only while expanded/visible and never overlaps commands. Accepted means scheduled, not successful; read busy/error/stale and peer status for completion. Failures preserve the previous roster marked stale; Stop remains available for recovery. Broker construction/spawn is on the worker, not the UI thread.

Initial count and burst controls clamp to broker ranges; model admission independently validates them. Plugin root is at most 1023 bytes and cannot contain NUL. Peer 1 cannot leave. Missing slots 2-8 can rejoin; bursts require an active running peer with probe telemetry. Text from peers uses literal formatting arguments, not peer-supplied format strings. Roster scrolls inside the ordinary dockable ImGui window.

Displays PID, role, members, port, running/failed status, error, UDP/retransmit/drop/quota and reliable/unreliable probe counters. **Not full browser parity:** peer widget trees, per-plugin refusal reasons, and widget-input routing are intentionally not implemented. This is a control/status dock, not a replacement widget renderer. Closing hides it, not stops its processes.

Bounds are compositional: the broker retains its existing 15s per-child response limit and process termination grace; an eight-peer operation can take multiple per-child intervals. Worker separation keeps Draw/Read responsive, but destruction waits for the admitted operation then cleanup. There is no immediate cancellation or new whole-operation deadline. POSIX embedders must retain hostd's existing SIGPIPE handling convention when using the broker; native product is Windows.

## Executed verification

Used strict red/green compile-link slices: missing new header; then missing lifecycle methods; then missing Draw symbol. Built real implementation with `-std=c++20 -pthread -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror` and real ImGui 1.92.8 docking core on macOS. The test exercises out-of-range admission, single-flight/duplicate start refusal, separate real child PIDs, start/status, host membership/port, burst counters, leave/rejoin, stop, sample-disabled send refusal, missing-executable error/stale status and refresh recovery. It also produces non-empty ImGui draw data in a headless 320x240 context.

Local verification command (adjust the cached dependency paths on other machines):

```sh
I=<SCRATCH>/mock-rc2-build/_deps/imgui-cb613f0dd1de5f8d9b199142f3122c122e44226c7e4edba758e011e79fafba62/imgui-1.92.8-docking
clang++ -std=c++20 -pthread -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror \
  -Iinclude -Isrc -Itests -isystem "$I" tests/test_native_net_sandbox.cpp \
  engine/src/ui/net_sandbox.cpp src/hostd/net_sim/broker.cpp src/hostd/net_sim/process.cpp \
  src/hostd/json.cpp src/hostd/handlers.cpp "$I/imgui.cpp" "$I/imgui_draw.cpp" \
  "$I/imgui_tables.cpp" "$I/imgui_widgets.cpp" \
  -o <SCRATCH>/native-net-test
<SCRATCH>/native-net-test <SCRATCH>/e12-b/scuba-hostd
```

`scuba-hostd` above is a pre-existing local test artifact, not rebuilt in this slice. Broker/process/JSON/registry and the dock were compiled from this scratch checkout. Windows D3D12 interactive rendering, input clicks and packaged executable discovery still require native acceptance. Headless draw output does not prove layout/clipping or Windows operation.
