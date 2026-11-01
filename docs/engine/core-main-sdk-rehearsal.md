# Local current-main SDK upgrade rehearsal

This records a development-input rehearsal, not a published SDK upgrade or
release acceptance. The original local batch used engine main
`f005372bd74967e53fdb8ed4cf3f4707ab3b20fa` and UDP diagnostic commit
`5a992e932a9a1948b6557f9a6844eb2ac2446d64`; it was subsequently merged into engine
main in #51. The preset remains opt-in and the published RC2 default pin is unchanged.

## Exact selected inputs

At the time of this rehearsal, core remote main was verified as
`bf56d2879393b2b5a603256347fc1d3cf9a1a751`.
It contains upstream #289's receive-buffer fix. Relative to `1b6782b`, the
changes are nine requirements-checker, packaging, documentation and CI files;
`include/`, `src/` and contract files are unchanged. Thus bf56 was the reviewed
development baseline for this rehearsal; older archive bytes are not reused
under its identity. Held core #305 is not part of this rehearsal.

| Profile generated from clean bf56 source | SHA-256 | Members |
| --- | --- | ---: |
| `sco-sdk-host-source-sc-2.4.0.zip` | `d41724ce46e34fa05a3ebfebc89ec3c16e6a2996061a183281dd17bcaa7fe0a8` | 926 |
| Combined plugin SDK `sco-sdk-2.4.0.zip` (`legacy` profile name) | `f7347ad09104ba64a697870c6245ff598949d3fb37cb84df43fe7e7138cba0e5` | 354 |

Both descriptors identify bf56 source. All archive member checksums were
verified before use. The catalogue SHA-256 is
`66362fa7d207d586e43136d61418d18d80daea4ae7a187d8eead06800528ed49`.
The host-source archive has 925 checksummed payload entries plus SHA256SUMS;
the combined archive has 353 plus SHA256SUMS. These are unpublished artifacts,
even though their SDK version strings remain 2.4.0.

## Reproduce the isolated build

```sh
. /workspace/.cloud-setup/activate.sh
export SCUBA_DEV_SDK_ARCHIVE_DIR=/workspace/engine-core-main-kit-bf56
python tools/verify_dev_sdk.py "$SCUBA_DEV_SDK_ARCHIVE_DIR/sco-sdk-host-source-sc-2.4.0.zip" \
  d41724ce46e34fa05a3ebfebc89ec3c16e6a2996061a183281dd17bcaa7fe0a8 \
  bf56d2879393b2b5a603256347fc1d3cf9a1a751
cmake --preset core-main-bf56-rehearsal
cmake --build --preset core-main-bf56-rehearsal
ctest --preset core-main-bf56-rehearsal --output-junit full-ctest.xml
```

The preset is disabled unless the explicit local archive directory is supplied;
it pins a verified hash and an unpublished source label, uses a separate build
directory, enables the engine/tests and warnings as errors, and executes CTest
serially. It does not fetch a similarly named release when the directory is set.

For the separate plugin-archive boundary check, verify/extract the combined zip
to `/workspace/engine-main-plugin-sdk-bf56`, then:

```sh
cmake -S tests/sdk_archive_plugins -B build-archive-plugins -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSCUBA_PLUGIN_SDK_ROOT=/workspace/engine-main-plugin-sdk-bf56/sco-sdk-2.4.0
cmake --build build-archive-plugins
ctest --test-dir build-archive-plugins --output-on-failure
```

This test configuration builds the actual four engine plugins using only the
extracted SDK's public headers/helper/checker and the plugins' own source files.
It is independent of the standalone plugin CMake entrypoint documented in
[plugin integration](plugin-integration.md#build-the-plugins-with-the-published-plugin-sdk-only).
Resolved compiler dependencies are recorded in the evidence directory. The
24-item requirements declaration is a selected C ABI subset, not an exhaustive
host-source C++ API census. Run it with the extracted SDK tool, catalogue and
registry; its exit-zero report describes static method/language mappings only.

## Observed Linux results

- Clean Clang 19 Release engine build, warnings as errors: passed.
- Full applicable CTest: **85/85 passed**, including hostd/mock registry,
  profiles/products, plugin surface, portable native tools, assets, SDK plugin
  checkers/simulators, network and MCP integrations.
- Studio: **62 passed, 8 skipped**, production TypeScript/Vite build passed.
  The dependency lockfile matched the existing cloud installation; a temporary
  node_modules symlink was removed after validation.
- Four plugins built from the combined plugin archive; **8/8** full/short-table
  checker runs passed. All 24 declared requirements are supported/direct;
  runtime/game remain `not_run`, provider/release remain `unestablished`.
- Six paired eight-peer runs: current main **6/6 passed**, all peers 224/224
  reliable and unreliable, kernel receive-buffer errors zero. RC2 **6/6 failed**,
  first receiver unreliable 111–219/224 and reliable 224/224; namespace aggregate
  receive-buffer error deltas ranged 11–200. Failures were retained.
- Six paired socket-only bursts: RC2 received 256/384 every time; current main
  received 384/384 every time and reported 4,194,304-byte receive capacity.
  The existing zero-loss assertions and send behavior were not relaxed.

Raw receipts/logs are under `/workspace/engine-sdk-rehearsal-evidence` and
`/workspace/engine-sdk-main-bf56-udp-pairs`. Namespace UDP counters are aggregate,
not individual-socket attribution. These runs do not prove universal stability.
Unlike the earlier 1b rehearsal, this candidate was freshly built in another
source/build directory and its test executable hash differs from the archived
RC2 executable. The test source/assertions were unchanged in those earlier runs; these paired results
qualify the complete current-main consumer profile rather than isolate one
binary delta. The separate socket burst and source comparison support the
receive-buffer diagnosis.
The stricter comparison runner now refuses different regression executables by
default. An explicit shared `--test-binary` can be used for a new controlled
comparison of those hostd builds. The earlier raw profile observations remain
preserved; they are not relabelled as identical-binary evidence.
Subsequent review added CheckExit to the network test so recorded CHECK failures
also produce a failing exit status in new builds. The assertion predicates were
not relaxed, and the older receipts remain evidence for their original sources.

## Package gate, compatibility gaps and remaining evidence

The full SDK `sdk/test-zip.sh` check **failed** in installed-header dependency
proof: the OS header `/usr/include/x86_64-linux-gnu/asm/bitsperlong.h` resolves
through a system symlink to `/usr/lib/linux/uapi/x86/asm/bitsperlong.h`, outside
CMake's declared implicit include roots. A command-local `-isystem` attempt
also failed because the proof deliberately strips ambient CFLAGS/CXXFLAGS.
Both failures are preserved; no checker policy, symlink or system setting was
changed. This requires a supported toolchain/container or core-owner review of
OS symlink handling. The 8/8 engine plugin checks do not repair that SDK gate.

The original consumer inventory found no frozen C ABI/table-coordinate change (48 catalogue
tables, 231 method mappings match RC2). Its original coverage gaps and subsequent work are:

- The original 85/85 run did not exercise ASOP selection busy rows and commit conflicts,
  or the actor-stats probe omitted from the host-source archive. The later
  [mock coverage follow-up](mock-coverage-followup.md) adds those synthetic tests using
  the matching combined archive. The main product ASOP plugin uses game.asop rather
  than game.asop.selection.
- Host-source app/mock/ImGui adapters are separate from plugin ABI checks. The
  plugin archive check does not establish the legacy host's ownership closure.
- The original local batch had no Windows/MSVC or macOS execution. The later published
  #51 hosted checks ran the bf56 development profile on Linux and Windows; that does not
  make the original Linux receipt a Windows result or establish game acceptance.
- Core main also has a failed release-candidate readiness workflow
  `41195084699`; no release eligibility or release approval is inferred here.

## Rollback and subsequent publication

Unset SCUBA_DEV_SDK_ARCHIVE_DIR and configure a fresh ordinary build directory
to return to the unchanged published RC2 path. Keep separate build directories;
do not mix objects/headers or hot-swap live SDK instances across profiles.
No release/tag restoration or remote pin rollback is needed because none changed.

The original local batch was subsequently published and merged in #51 as an **opt-in
development profile**, with the follow-up mock coverage and hosted checks. Those later
results do not replace the original failed local SDK gate documented above. Default
dependency promotion and release acceptance remain separate decisions; development
archive bytes are not a published SDK release.
