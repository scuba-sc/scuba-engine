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
and other workloads can affect results. It is opt-in and is not added as a gate
to the released RC2 profile. It uses the selected host-source SDK socket API;
the optional receive-buffer diagnostic is absent in RC2 and printed as such.

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

The runner alternates baseline/candidate serially and records every result,
binary/archive hash, embedded SDK source identity, timestamp and full log. It
requires matching declared SDK compatibility coordinates and the same regression
binary hash in both inputs. Use `--test-binary /path/to/test_net_sim` to explicitly
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

The fixed kit is a development artifact, not a published RC3 release. Default
published RC2 consumption is unchanged. Windows/macOS, clamped-low-rmem kernels,
forced-buffer privilege and real network/game behavior were not exercised by
this investigation. Six successes on one Linux host do not establish universal
stability. No core or offline source was edited or published.
