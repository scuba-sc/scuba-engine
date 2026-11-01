# Curated reserved mock host candidate

`scuba-reserved-hostd` is a development host for copied mock scene operations
through scuba-engine's existing JSON registry. It accepts one JSON request per
stdin line and writes one response per stdout line. EOF closes admission and
retires the session. This is an incremental consumer slice toward the core
roadmap's milestone 2, products and studio on the SDK. It uses the held core
host-source contract, not the released plugin SDK ABI.

This local rehearsal uses held core #315 source
`e4ba511c5be0f1f901e1ca958a1fd28839e30fdc`, bootstrap 3, mock table 5,
graph 1. Its host-source archive has 985 members and SHA-256
`090c27a051b25df6b78a620b9d84de64325a96bbd77a852f650b3efe25e5660b`.
Build from engine source based on main `b569c3d5f9e5598d2c1071124d1cc68a21fdac0a`:

```sh
cmake -S . -B build-curated -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSCUBA_RESERVED_MOCK_HOST_ONLY=ON \
  -DSCUBA_RESERVED_CORE_COMMIT=e4ba511c5be0f1f901e1ca958a1fd28839e30fdc \
  -DSCUBA_SDK_ARCHIVE_DIR=/workspace/engine-core-kit-e4ba \
  -DSCUBA_SDK_HOST_SOURCE_SHA256=090c27a051b25df6b78a620b9d84de64325a96bbd77a852f650b3efe25e5660b
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

Remaining adoption gates: the core owner's qualification of the exact held
contract/composition, Windows compile/runtime and artifact audit, editor transport
and scene integration, complete mock services, reviewed plugin/UI callback
ownership and logging/worker roots. The existing `Kernel::Start` uses legacy app
startup, which reserved sessions deliberately refuse; it is not migrated by this
candidate. Native/PAK/game activation remains unavailable pending product-owned
reader and instruction exclusion proof. No gameplay or visual acceptance is
established here. No core implementation is changed by this consumer.

The held `e4ba511` input was exercised locally on Linux; the earlier hosted
Windows result for `0bac7b` does not qualify this new source and archive.
The Linux Release build passed 9/9 tests, including its binary-composition
audit. ASan/UBSan passed the eight runtime tests with leak detection enabled;
the sanitizer-instrumented binary failed the unchanged composition audit because
the Clang sanitizer runtime adds `libresolv`. The audit was not waived.
Hostd's legacy startup, its peer process and editor ownership remain separate
adoption work. This rehearsal does not qualify them to use core #315.

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

The engine must also preserve process-lifetime state after
`RetainDependencies` or a non-`Ok` stop, refuse restart in its bounded first
implementation, and treat `RefusedBeforeExposure` as applying only to that
invocation. Hostd logging and server contexts, mock scene/TLS state, peer
shutdown acknowledgement and staged files require explicit ownership. A
reserved retained verdict is not a successful `app::Stop`.
