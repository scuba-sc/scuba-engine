# NetSandbox

NetSandbox runs 2-8 child processes of `scuba-hostd`. Each child starts the
sco-core kernel, loads its own plugins, and participates in a real localhost UDP
session through `sco.net`. Peer 1 hosts; all other peers join its ephemeral port.
The SDK's star topology relays joiner messages through Peer 1. No game process,
game files, or proprietary fixtures are used.

## Run

```sh
cmake -S . -B build
cmake --build build
cd studio
npm ci
npm run build
cd ..
./build/scuba-hostd --studio studio/dist
```

The foundation downloads the published `sdk-v2.4.0-rc.1` host-source kit (SDK
2.4.0, release candidate 1) and verifies
its pinned SHA-256. For offline work, set `SCUBA_SDK_ARCHIVE_DIR` at configure
time to a directory containing the release archive; the same hash check applies.

Open the studio and select **NetSandbox**. Select a peer count and choose
**Start sandbox**. The default synthetic chat plugin publishes a widget panel
in every peer. Enter a message, then press **Send to peers**; the other panels
display the sender and message. **Send 16 + 16** sends 16 reliable and 16
unreliable synthetic messages from that peer. The traffic strip shows UDP
datagrams, retransmissions, queue drops, quota refusals, and sample message
counts. Unreliable delivery is not guaranteed by UDP; the loopback tests report
actual loss and require zero.

An optional plugin folder is loaded independently in every child, alongside the
sample when selected. Turn off **Synthetic chat plugin** to run only user
plugins. Lua and native plugins use the kit's normal loader. Refused plugins
are shown with their reasons. Widget controls are routed through
`sco::widgets::RenderPanel`, including the SDK's validation and guarded plugin
callbacks. Labels and table cells are plain text, not HTML.

**Leave** stops a joiner. **Join peer N** starts a fresh process in that slot.
The slot is a studio identifier, not a network peer id: SDK peer ids are not
reused on rejoin. Peer 1 cannot leave independently; use **Stop all**.
The session may temporarily have only its host after joiners leave.

## Isolation and lifecycle

- Processes share no kernel globals. Each receives a separate storage directory
  under a randomly named temporary session directory; stopping deletes that data.
  Plugin source folders are shared read-only by the sandbox itself. A native
  plugin is ordinary local code, not an operating-system security sandbox.
- A first host-owned built-in, `netsim_isolation`, restarts `sco.ipc` with a
  session-and-slot-specific prefix before any user plugin loads. Prefixes retain
  the SDK-required `/SCO_` or `Local\SCO_` base. Plugin code still uses only
  `sco_ipc_v1`; hardcoded default mapping names in external programs will not
  attach. `ipcPrefix` in the snapshot exposes the selected prefix.
- UDP binds and joins are strictly loopback. Each session has an ephemeral host
  port and a random passphrase kept off the studio wire and logs. The SDK's
  authentication and default key derivation are unchanged.
- Children tick the application kernel every 16 ms and when processing control
  requests. Their network threads run independently of studio polling.
- Hostd launches its own executable directly, without a shell. The private
  control transport is inherited stdin/stdout with one JSON object per line.
  Kernel logs go to stderr. Arbitrary plugin writes to stdout can invalidate
  that peer's control stream; the studio then reports the peer as failed.
- macOS uses close-on-exec-default spawn semantics; Linux closes inherited
  descriptors via `/proc/self/fd` before starting its kernel. Windows uses a
  handle allowlist. Closing the parent's pipe causes graceful kernel shutdown;
  normal stop reaps each process, with forced termination after a 3 s grace
  period if a plugin prevents shutdown. Joiners stop before the session host.
- Commands are serialized on hostd's game thread. Starting/joining waits up to
  10 s for membership. A child response has a 15 s bound and a 4 MiB line cap.
  Timeout invalidates that control stream rather than reusing a late response.
  A failed peer remains visible until it leaves or the session is stopped.
- Membership is taken from dispatched `net.state` and `net.peer` events.
  The SDK can signal delivery before its polling snapshot has been published;
  using the events avoids a stale `get_peers` read followed by a missed wake.

## Bridge contract

All calls use the foundation envelope `{id, method, params}` and
`{id, result}` / `{id, error:{code,message}}` on `/ws`.

| Method | Params | Result |
| --- | --- | --- |
| `netsim.start` | `{count:2..8, pluginRoot?:string, sample?:boolean}` | Sandbox snapshot; refuses an already running sandbox |
| `netsim.status` | `{}` | Sandbox snapshot |
| `netsim.stop` | `{}` | Empty sandbox snapshot |
| `netsim.leave` | `{slot:2..8}` | Sandbox snapshot after membership converges |
| `netsim.join` | `{slot:2..8}` | Snapshot after a vacant slot rejoins |
| `netsim.send` | `{slot:1..8, count:1..100}` | Snapshot after queuing both sample channels |
| `netsim.input` | `{slot,panel,node,kind,text?,value?,index?,row?}` | Snapshot after SDK-validated widget input |

`netsim.send` requires the synthetic plugin. Counts mean queued sends, not
acknowledged delivery. `netsim.input` uses SDK node/event discriminants
(1-10, with paging event 256), integer node ids, text at most 255 UTF-8 bytes,
and finite numeric values. The SDK ignores events invalid for a node.
Boundary errors use -32602; runtime failures use the foundation's -32603
exception mapping. Failed peers are represented in snapshots rather than
failing the entire status call.

Every successful status or mutation call also broadcasts
`{event:"netsim.snapshot",data:<snapshot>}`. There is no unsolicited timer in
hostd: the mounted studio requests status every 400 ms, never overlaps its
own polls, and serializes mutations. Hiding the panel stops polling, not the
peer processes; hostd exit stops all peers.

```text
Sandbox = {peers: Peer[]}
Peer = {
  slot:number, pid:string, status:"running"|"failed",
  error?:string, active?:boolean, self?:string, members?:number,
  port?:number, reason?:string, ipcPrefix?:string,
  traffic?:{datagramsIn,datagramsOut,retransmits,sendDropped,inboxDropped,quotaRefused},
  probe?:{reliableSent,unreliableSent,reliableReceived,unreliableReceived},
  plugins?:[{id,state,reason}],
  panels?:[{id,title,owner,nodes:[{
    id,parent,kind,flags,label,text,value,minimum,maximum,index,columns,
    items:string[],tooltip,severity,firstRow,visibleRows
  }]}]
}
```

Network peer ids and process ids are strings to avoid JavaScript integer
precision loss. Other counters are JSON numbers.

## Integration points

- `src/hostd/net_sim/broker.h`: `Broker::{Start,Snapshot,Leave,Join,Peer,Stop}`
  and `RegisterNetSim(Registry&, Broadcaster&, executablePath)`. The broker owns
  process lifetimes. `main.cpp` registers it once and dispatches the private
  `--net-peer` mode before booting the parent kernel.
- `src/hostd/net_sim/peer.cpp`: `PeerMain()` owns a child kernel and the
  host-side network session. `sco/net/session.h` is used only here; the sample
  plugin includes the public C service headers.
- `src/hostd/net_sim/probe_plugin.cpp`: a small built-in synthetic test/sample
  plugin. Its `netsim_probe.metrics` service is defined in `probe_api.h`.
  It registers `netsim_probe.reliable` and `netsim_probe.unreliable` before
  session startup, and publishes `netsim_probe.chat` via `sco.widgets`.
- `studio/src/panels/netsim/index.ts`: `netSandboxPanel`, registered through the
  foundation's `PanelModule` convention. Wire types are in `netsim/types.ts`.
  `widgets.ts` renders widget kinds 1-10 and paging events.

The UI preserves the foundation's dark system-font appearance: two columns on
desktop, one below 800 px, neutral surfaces and borders, high-contrast action
buttons, visible labels and keyboard focus rings. Inputs are not replaced while
being edited. This is a scoped addition to the existing UI, not a redesign.

## Verification

```sh
ctest --test-dir build --output-on-failure
```

`netsim_two_peer_delivery` checks 32 messages per reliability in both
directions and a widget-driven chat message. `netsim_four_peer_join_leave`
checks four-way membership, leave/rejoin, and 96 received messages per
reliability per receiver after rejoin. `netsim_eight_peer_bounds` rejects
out-of-range initial counts, starts eight distinct processes, and checks
224 messages per reliability per receiver. All use real UDP, the real kernel,
and the public-API-only synthetic plugin. Waits use SDK delivery notifications
with bounded deadlines, never sleeps. Each prints `unreliable_loss=0` only
after comparing delivered counts with the expected counts.

These are standalone host tests, not in-game verification. Linux GCC and
Windows MSVC need their first platform compile. No raw `sco.ui` overlay or
shared-memory peer bridge is implemented by this panel; the retained
`sco.widgets` panels and private `sco.ipc` namespaces are available to plugins.
