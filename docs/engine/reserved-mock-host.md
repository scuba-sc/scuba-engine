# Curated reserved mock host candidate

`scuba-reserved-hostd` is a development host for copied mock scene operations
through scuba-engine's existing JSON registry. It accepts one JSON request per
stdin line and writes one response per stdout line. EOF closes admission and
retires the session. This is an incremental consumer slice toward the core
roadmap's milestone 2, products and studio on the SDK. It uses the held core
host-source contract, not the released plugin SDK ABI.

This rehearsal uses RC3 candidate core source
`44ffd1290e261e12684b38ac862ec0c4f08921df`, bootstrap 3, mock table 5,
graph 1. Its host-source archive has 985 members and SHA-256
`799511bfa08a7bf77a46fcf8262d3cac4a5f0e6a0ef04a28f53e6e624c01e8a0`.
The paired core-source archive has 442 members and SHA-256
`807a322eab2b3d8c25e10c3582008ec3ec50b6b536bc947f2b28cf4b63095c22`;
the engine consumes the host-source profile. Core #313's implementation merged
at `349911d37a2fe5bb4acbafc972615578b9da82a0`; core #317 later finalized the
candidate designation at `6106fa3639cbcc28f78a1ded8de2707de019f4d3`. The latter
changes release metadata only, but its final host-source archive is still pending
qualification. This historical reserved-host rehearsal uses the earlier exact
source and is separate from the checked lifecycle integration documented in
[`core-lifecycle-candidate.md`](core-lifecycle-candidate.md). Build this
reserved-host rehearsal from its PR branch:

```sh
cmake -S . -B build-curated -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSCUBA_RESERVED_MOCK_HOST_ONLY=ON \
  -DSCUBA_RESERVED_CORE_COMMIT=44ffd1290e261e12684b38ac862ec0c4f08921df \
  -DSCUBA_SDK_ARCHIVE_DIR=/path/to/verified-candidate-archives \
  -DSCUBA_SDK_HOST_SOURCE_SHA256=799511bfa08a7bf77a46fcf8262d3cac4a5f0e6a0ef04a28f53e6e624c01e8a0
cmake --build build-curated
ctest --test-dir build-curated --output-on-failure --output-junit ctest.xml
```

Configure verifies the archive digest, complete member checksum inventory and
embedded source commits before extracting and compiling it. The curated target
links one executable-resident `sco_authority` and the engine's JSON/Registry
sources. The legacy app, scanner, plugin loader, native providers and network
server are absent from its link graph. Default engine builds retain their
published RC2 profile. This opt-in does not disable existing tests or features.

Example stdin session:

```json
{"id":1,"method":"mock.entity_put","params":{"id":"11","zone":"1","position":[4,5,6],"flags":0,"name":"synthetic-editor-object"}}
{"id":2,"method":"mock.entity_get","params":{"id":"11"}}
{"id":3,"method":"mock.graph"}
{"id":4,"method":"mock.scalar_tick","params":{"amount":"17"}}
{"id":5,"method":"mock.scalar_snapshot"}
```

Methods are `mock.entity_put`, `mock.entity_get`, `mock.entity_remove`,
`mock.graph`, `mock.scalar_set`, `mock.scalar_tick`, `mock.scalar_snapshot`,
`mock.status`. Status explicitly reports `nativeUnavailable`, `mockOnly`,
`scalarTickOnly`, source commit and negotiated versions.
Entity writes are by-value upserts into an existing zone; world zone 1 is seeded.
IDs and scalar/tick results use decimal strings, preserving all 64 bits. Inputs
also accept exact JSON integers within the JavaScript-safe range. Requests over
65536 bytes are refused without retaining an unbounded line. Graph traversal
copies records sequentially; it is not an atomic multi-record snapshot. Scalar
tick counts are not application Tick, elapsed time or scene simulation.

The process-lifetime root owns the registry, handler closures, binding, session,
and table reference. Reservation precedes handler registration. Each handler
holds admission through dependent reads and construction of independent JSON
values. An unfinished startup faults and retains the session. Closure rejects
cached handlers and table calls. Both retirement verdicts retain backing until
process exit and prevent restart; `QuiescedRetained` never means `app::Stop`.
The executable dispatches on one control thread and loads no modules or scripts.

Acceptance checks cover normal mutation/readback/removal and copied graph/scalar
values, invalid zone refusal, cached-call refusal, startup abandonment,
self-retirement failure and restart refusal, actual stdio requests with exact
64-bit boundaries, oversized-line recovery and EOF retirement. The Linux binary
check verifies one authority definition, absence of named legacy implementations,
no project exports or loader overrides, and reviewed system dependencies. This
bounded check cannot certify arbitrary code, unmanaged modules or Windows.
The dedicated `curated mock host` workflow builds this exact source/kit profile
on Linux and Windows. MSVC emits a production linker map; the Windows composition
check reviews its public code authority symbol and named legacy/test symbols,
PE imports and exports using `dumpbin`. Map symbols and imports do not establish
complete code closure or exclude arbitrary runtime-loaded code. The workflow
preserves CTest XML and the Windows map as evidence; its actual result must be
checked for the consumer commit being qualified.

Four additional fresh-process tests pause the actual entity handler during JSON
name copying. A separate retirement thread must observe a live reader inside the
core authority while admission is closed and remain pending until the handler
releases its pin. The exception case also pauses a destructor during unwind and
requires retirement to remain pending there. Both return data and exception
message/data are inspected after retirement, when they are independently owned.
The two negative controls reset the actual handler pin early: retirement must
then complete while the copy remains blocked, demonstrating the missing
protection. Each case checks retained phase, cached-handler refusal and restart
refusal. Gates and promises establish ordering; wall-clock limits bound test
failure rather than supply the evidence of waiting.

Only the separate test executable compiles the SDK's observation-only
`SCO_RESERVED_DRAIN_TESTING` branch and consumer copy probes. It builds from the
same verified extracted SDK source without changing core files or retirement
verdicts. The normal host uses the uninstrumented authority; its binary check
also rejects reader-observer and consumer-probe symbols. This exercises one
entity handler's copy/unwind path, not a census of every future handler.

This reserved-host rehearsal is a separate executable and does not qualify the
legacy `Kernel` or native editor. The checked lifecycle candidate now adapts that
legacy path in an explicit development profile, but it does not enable native
game services. Complete mock-service coverage, plugin/UI callback ownership,
logging and worker roots, and native reader/instruction exclusion remain separate
gates. No gameplay or visual acceptance is established here. No core
implementation is changed by this consumer.

The 44ffd host-source archive from candidate workflow 41332377573 passed its
checksum, inventory, provenance and SPDX audits. Regenerating it from that
exact clean source with the engine packaging wrapper produced the same hash.
On Linux, the local combined engine branch passed all nine curated tests against
the candidate archive. Check the dedicated workflow for results on the PR's
synthetic merge commit and both hosted platforms; the earlier 4c09 runs do not
establish hosted evidence for 44ffd.

Previously, the 4c09 input was generated from the exact core source. The core-source
archive (442 members) and host-source archive (985 members) passed package and
provenance audits. The engine workflow packaging wrapper reproduced the exact
host-source SHA-256. The Clang 19.1.7 Linux Release consumer build passed all
nine registered tests, including the binary-composition audit. Hosted run
[41288580618](https://github.com/scuba-sc/scuba-engine/actions/runs/41288580618)
passed the same nine tests on Ubuntu and Windows for engine source commit
`0b294e9fcd433b50dd2a06658d2bab4a17d740fb`; both jobs regenerated and verified
the exact 4c09 host-source archive. These results qualify only this bounded
mock-host source and its CI composition checks. The separate e4ba ASan/UBSan
result (eight runtime tests passed with leak detection; the unchanged composition
audit rejected the sanitizer's `libresolv` dependency) applies only to e4ba
and is not evidence for 4c09. Hostd's legacy startup, its peer process and
editor ownership remain separate adoption work. This rehearsal does not qualify
them to use core #315.

The legacy host cannot gain owned-session safety merely by switching to
`app::StartDetailed`: it refuses a non-null shutdown owner, and an active
reserved session blocks legacy app admission. The current owned host capsule
also excludes the builtin and capability callbacks used by the existing mock
providers. Engine-side code alone cannot turn those callbacks and cached legacy
tables into a reserved session. A future supported composition needs a
core-coordinated ownership and admission route before logging or mock
configuration. It must strongly own backing and callback code and hold admission
through provider calls, output copying and unwind. Unknown plugin and native
paths need their own callback and worker lifetime proof before activation.

The legacy engine candidate preserves process-lifetime state after
`RetainDependencies` or a non-`Ok` stop, refuses restart after an uncertain
outcome, and treats `RefusedBeforeExposure` as applying only to that invocation.
Hostd logging and server contexts, mock scene/TLS state, peer exit status and
staged files require explicit ownership. A reserved retained
verdict is not a successful `app::Stop`.
