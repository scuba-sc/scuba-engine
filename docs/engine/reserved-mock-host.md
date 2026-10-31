# Curated reserved mock host candidate

`scuba-reserved-hostd` is a development host for copied mock scene operations
through scuba-engine's existing JSON registry. It accepts one JSON request per
stdin line and writes one response per stdout line. EOF closes admission and
retires the session. This is an incremental consumer slice toward the core
roadmap's milestone 2, products and studio on the SDK. It uses the held core
host-source contract, not the released plugin SDK ABI.

The candidate is pinned to core `ebb147a6d4b9b44498c087f7474c399a6cf22267`,
bootstrap 3, mock table 5, graph 1. Its host-source archive has 979 members and
SHA-256 `44201578423e8a714c400aa108288620fbf272f14a0ee5bdee966a0eebff3c98`.
Build from engine source based on main `f005372bd74967e53fdb8ed4cf3f4707ab3b20fa`:

```sh
cmake -S . -B build-curated -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSCUBA_RESERVED_MOCK_HOST_ONLY=ON \
  -DSCUBA_RESERVED_CORE_COMMIT=ebb147a6d4b9b44498c087f7474c399a6cf22267 \
  -DSCUBA_SDK_ARCHIVE_DIR=/workspace/engine-rc3-kit-ebb147 \
  -DSCUBA_SDK_HOST_SOURCE_SHA256=44201578423e8a714c400aa108288620fbf272f14a0ee5bdee966a0eebff3c98
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

Remaining adoption gates: the core owner's qualification of the exact held
contract/composition, Windows compile/runtime and artifact audit, editor transport
and scene integration, complete mock services, reviewed plugin/UI callback
ownership and logging/worker roots. The existing `Kernel::Start` uses legacy app
startup, which reserved sessions deliberately refuse; it is not migrated by this
candidate. Native/PAK/game activation remains unavailable pending product-owned
reader and instruction exclusion proof. No gameplay or visual acceptance is
established here. No core implementation is changed by this consumer.
