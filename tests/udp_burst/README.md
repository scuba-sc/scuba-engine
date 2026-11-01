# Local regression for engine issue 37

The engine's existing `netsim_eight_peer_bounds` assertion remains unchanged:
each of eight receivers must receive 224 reliable and 224 unreliable messages.
An RC2 retry passing on a fast runner does not repair receive-buffer overflow.
Issue [37](https://github.com/scuba-sc/scuba-engine/issues/37) identifies the fix
in upstream core [289](https://github.com/scuba-sc/sco-core/pull/289), merged as
`0c977d3229012ab7bdf70e17da24dd2189fc4f40`.

This standalone diagnostic sends 384 distinct 64-byte datagrams to a loopback
SDK socket before reading any. It then requires every datagram exactly once.
There are no send retries or fixed sleeps; the one-second deadline bounds a
failed receive. Its capacity demand is deliberately a regression workload, not
a portable promise that UDP cannot lose data. Socket buffers, kernel accounting
and other workloads can affect results. This separate 384-datagram diagnostic is
opt-in; the normal engine suite retains the eight-peer 224/224 assertion as a
gate against the published SDK. The released SDK 2.4.0 host-source kit includes
the upstream receive-buffer fix; the RC2 profile lacks the optional capacity
diagnostic and prints that fact.

## Reproduce

Activate the cloud toolchain if needed (`. /workspace/.cloud-setup/activate.sh`).
Supply a local, checksum-verified SDK archive directory, then configure each
profile separately:

```sh
cmake -S tests/udp_burst -B build-burst-rc2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSCUBA_SDK_ARCHIVE_DIR=/workspace/engine-integration/build/_sdk \
  -DSCUBA_SDK_HOST_SOURCE_SHA256=3d711463f7dff3dcd372ffb70b09951ecf21f1d0e2c5f0a899cccbb9977b8201
cmake --build build-burst-rc2 --target test_udp_burst
ctest --test-dir build-burst-rc2 --output-on-failure

cmake -S tests/udp_burst -B build-burst-fixed -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSCUBA_SDK_ARCHIVE_DIR=/workspace/engine-core-artifacts-1b6782 \
  -DSCUBA_SDK_HOST_SOURCE_SHA256=9db31895ffc0806e663fca36be4bbad2d25c231cf9ae894008743d345e78ae3e
cmake --build build-burst-fixed --target test_udp_burst
ctest --test-dir build-burst-fixed --output-on-failure
```

The first command's test is expected to fail where the small RC2 buffer
overflows. Both builds retain warnings as errors in the consumer test.

The existing eight-peer test can be compared without changing its source,
deadlines or assertions:

```sh
python tools/compare_udp_bounds.py \
  --baseline-build /workspace/engine-integration/build \
  --candidate-build /workspace/engine-integration/build-core-1b \
  --runs 6 --out /workspace/udp-comparison-new-directory
```

For the published SDK 2.4.0 comparison, use its exact hostd build and one identical test
binary for both profiles:

```sh
python tools/compare_udp_bounds.py \
  --baseline-build /workspace/build-kernel-lifecycle-rc2 \
  --candidate-build /workspace/build-kernel-lifecycle-6106 \
  --test-binary /workspace/build-kernel-lifecycle-6106/tests/test_net_sim \
  --runs 6 --out /workspace/udp-comparison-release-new-directory
```

The runner alternates baseline/candidate serially and records every result,
binary/archive hash, embedded SDK source identity, timestamp and full log. It
requires matching declared SDK compatibility coordinates and the same regression
binary hash in both inputs, and distinct hostd binary hashes. Comparing an
identical runtime with itself is inconclusive even if its two runs differ.
Use `--test-binary /path/to/test_net_sim` to explicitly
run one regression executable with both hostd builds when build-local test
binaries differ. Pair checks do not authenticate arbitrary build provenance;
retain the separate verified archive and build receipts.
Baseline crash, startup/API failure, process timeout or malformed diagnostics are
inconclusive. Reproduced loss requires the expected nonzero exit and a valid
eight-member probe diagnostic with 32 sends, 224 reliable receives, and fewer
than 224 unreliable receives. Candidate success requires all eight unique slot
results with the full counts; exit zero alone does not qualify.
Failed CHECK/assertion output also disqualifies the result, including logs from
older test binaries that returned zero after recording a failed check. The
network test now returns CheckExit so those failures also fail CTest directly.
The no-network synthetic classifier tests exercise crash, mismatch and startup
negative controls; they are not runtime UDP evidence.

The runner
refuses an existing output directory. Its 120-second default matches CTest;
timeouts are marked and cannot establish delivery loss. SIGINT/SIGTERM cleanup
kills the test's process group and records interruption. Linux UDP counter
deltas are optional **aggregate counters in the current network namespace**;
unrelated traffic can contribute. Run no other UDP probes concurrently. A
candidate failure is retained rather than retried to obtain a green result.

## Executed Linux evidence, 2026-11-01

Engine source baseline `f005372bd74967e53fdb8ed4cf3f4707ab3b20fa` has the same
tree as the existing build's `cb31786a4cb265c77ebc194d4add8b3025057fd5`.
The actual eight-peer test binary is byte-identical between the two profiles
(SHA-256 `2111dab2102166943f08df758bbd6b95a7836c463bf108d8716daf3ef402cd5f`).
Only the hostd dependency build changes.

| Input | Embedded core source | Archive SHA-256 |
| --- | --- | --- |
| Published RC2 host-source | `844e136ccb280ad8cb43f73bc43c9447e8f6799d` | `3d711463f7dff3dcd372ffb70b09951ecf21f1d0e2c5f0a899cccbb9977b8201` |
| Fixed development host-source | `1b6782bc5c8b7d189cdff4c83e81957f09af35d7` | `9db31895ffc0806e663fca36be4bbad2d25c231cf9ae894008743d345e78ae3e` |

| Serial workload | RC2, six runs | Fixed SDK, six runs |
| --- | --- | --- |
| Eight-peer regression | 6 failures; first receiver unreliable 110–127/224, reliable 224/224 | 6 passes, all receivers 224/224 for both delivery classes |
| Kernel `RcvbufErrors` delta per eight-peer run | 199–200 | 0 |
| Socket-only burst | 256/384 received every run; 128 missing | 384/384 every run |
| Kernel `RcvbufErrors` delta per socket-only run | 128 | 0 |

The fixed socket reports 4,194,304 bytes of receive capacity (Linux doubles the
requested 2 MiB). This host reports `rmem_default=212992`, `rmem_max=4194304`,
and no effective capabilities. No sysctl, capability or security setting was
changed. The selected POSIX UDP sources differ only in the receive-buffer
request and added diagnostic getter. This supports the upstream receive-buffer
fix, with no separate engine runtime fix justified by these observations.

Full logs/receipts remain at `/workspace/engine-udp-ab-serial-20261101` and
`/workspace/engine-udp-burst-20261101`. The initial paired batch is retained at
`/workspace/engine-udp-ab-20261101`; an independent baseline probe may have
overlapped that batch's aggregate-counter window, so the explicitly serial
batch above is the primary comparison. Earlier six baseline failures remain
at `/workspace/engine-rc2-network-repro`.

The six-run comparison above records the earlier unpublished development input,
not the stable release. A separate paired recheck against the exact published
SDK 2.4.0 package used engine build `/workspace/build-kernel-lifecycle-rc2` as
the RC2 baseline and `/workspace/build-kernel-lifecycle-6106` for the release.
The test executable was identical for both inputs (SHA-256
`70a82df721ccf8be6ccb4d50a37c5eb7bc8b0a4ae17202b81096daea5210f64e`); the
hostd binaries differed (RC2 SHA-256
`1d76708fb6aea6b7253e6afd22468e799107ec79353cbb93e4f342eb950e952e`; 2.4.0
SHA-256 `135424dc2d066e69db00dcebba51360fb4d55f4782ac405d94ff673995f9d528`).
The 2.4.0 archive is source-bound to
`6106fa3639cbcc28f78a1ded8de2707de019f4d3` and SHA-256
`a0aaaba07e3d4eb0d8c264912cd6f2ab23f0d237a99fa07816005790a76efc4c`.
In six alternating runs, RC2 reproduced loss all six times with aggregate
`RcvbufErrors` deltas 64, 70, 77, 100, 75, and 100; the published release
passed all six with zero such errors. The receipt and per-run logs are in
`/workspace/engine-udp-rc2-vs-sdk240-20261101`. The comparison used one exact
test binary and retained the 224/224 assertions. Kernel counters are aggregate
to this namespace and can include unrelated traffic.

The final SDK 2.4.0 package was subsequently published
from core source `6106fa3639cbcc28f78a1ded8de2707de019f4d3`; its host-source ZIP
SHA-256 is
`a0aaaba07e3d4eb0d8c264912cd6f2ab23f0d237a99fa07816005790a76efc4c`. The
The engine now pins this package by default. The current engine branch was
configured from a fresh directory with the default checked lifecycle profile and
the published plugin-SDK archive consumer enabled; its Clang 19 Release build
passed and the full CTest suite passed 97/97, including the exact
`netsim_eight_peer_bounds` 224/224 assertion. Earlier Release builds against the
same exact package passed 92/92 with checked lifecycle enabled and 86/86 with
that consumer option disabled. These are Linux synthetic loopback results, not
a universal UDP guarantee or game/network acceptance. Windows/macOS,
clamped-low-rmem kernels, and forced-buffer privilege were not exercised. The
package was published by the core owner; no core source was edited here.
