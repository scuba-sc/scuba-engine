# Local mock coverage follow-up

This batch follows `e031d916421394e5fdc0984eec5ba24ff30f7f3a` without replacing
its receipt or build evidence. The new build directory is
`build-mock-coverage-core-main`; evidence is under
`/workspace/engine-mock-coverage-evidence`. No core packaging, SDK archive,
published dependency pin, remote branch or capability default was changed.

Inputs remain core main `bf56d2879393b2b5a603256347fc1d3cf9a1a751`:

| Archive | SHA-256 |
| --- | --- |
| Host-source-sc | `d41724ce46e34fa05a3ebfebc89ec3c16e6a2996061a183281dd17bcaa7fe0a8` |
| Combined plugin SDK | `f7347ad09104ba64a697870c6245ff598949d3fb37cb84df43fe7e7138cba0e5` |

## ASOP selection behavior

The fixed-document mock now treats a class as busy when a scene vehicle has a
case-insensitive matching class. It remains busy until that vehicle is erased,
including while unstreamed or pending removal. This is a conservative synthetic
scene policy, not a native fleet provider or its lifetime proof.

Both file and catalogue rows expose the SDK busy flag. Accepted edits recheck
removed classes immediately before the mock commit. A busy removal produces
EDIT_CONFLICT / REASON_BUSY, retaining file contents, digest, revision and semantic
revision. Retaining the class with changed case/order is permitted. Clearing the
vehicle permits a new request to commit; retrying the old request still returns
its original conflict receipt. Revoking the lease before Tick preserves the file.

With older headers lacking the additive constants, rows omit the unavailable
busy flag and status uses the existing generic conflict reason. Removal is still
refused; no success callback or invented numeric constant substitutes for it.
Isolated Release tests compiled and passed with both RC2 and current headers.
Full busy flag/reason conformance is established only with the current SDK.

The new regression failed eight checks against the original mock and passed
against the update. The saved pre-change source was verified byte-identical to
e031's source. Existing Release contract assertions were already enabled by
`-UNDEBUG`; this was a behavior/test-case gap, not disabled assertion execution.

## Actor-stats packaged fixture and actual provider tests

In bf56 `sdk/components.json`, all four actor_stats_probe example components
declare only `game-sc`. The combined `legacy` profile includes that component
set; `host-source-sc` includes `core-source` and omits this example. The archive
contents match those declarations. This establishes profile selection, not an
intent to deny consumer support or proof of a native provider.

The existing local `SCUBA_ACTOR_STATS_SDK_DIR` override also accepts the extracted
combined SDK: its public headers and `examples/actor_stats_probe` are present.
The new fixture test checks both archive hashes, complete checksum inventories,
compatibility/source coordinates, matching public header bytes, and extracted
probe source/ini before the override is used. It rejects an incorrect expected
source; an additional wrong-archive input was rejected by the hash gate. No
example code was copied into the engine and no private core checkout is used.

Enabling the previously omitted mock_actor_stats test uncovered a stale test:
ID 10 was expected to be invalid, but IDs 10–29 are SDK 1.1 read-only vocabulary.
The provider already handled them correctly. The test now queries version 1.1
before checking every extended synthetic read and missing-record behavior,
rejects LAST_1_1+1, and checks that every extended ID refuses adjust/set/mode
without changing its value even with writes enabled and offline admission.
The original 1.0 probe/query path is preserved.

The actual packaged probe is loaded into the synthetic host. Existing tests
exercise nine original reads, deterministic drift, writes OFF by default,
explicit mock write enablement, disarmed probe refusal, offline admission
refusal, authoritative/local-only/discarded modes, and copied readback. These
tests establish working mock behavior, beyond the earlier static requirements
report. They do not establish game values, native actor lifetime or real writes.

## Reproduce

After verifying/extracting the archives as in the prior rehearsal:

```sh
. /workspace/.cloud-setup/activate.sh
python tests/test_actor_stats_fixture.py \
  --host-archive /workspace/engine-core-main-kit-bf56/sco-sdk-host-source-sc-2.4.0.zip \
  --plugin-archive /workspace/engine-core-main-kit-bf56/sco-sdk-2.4.0.zip \
  --plugin-root /workspace/engine-main-plugin-sdk-bf56/sco-sdk-2.4.0 \
  --source bf56d2879393b2b5a603256347fc1d3cf9a1a751
SCUBA_DEV_SDK_ARCHIVE_DIR=/workspace/engine-core-main-kit-bf56 \
  cmake --preset core-main-bf56-rehearsal -B build-mock-coverage-core-main \
  -DSCUBA_ACTOR_STATS_SDK_DIR=/workspace/engine-main-plugin-sdk-bf56/sco-sdk-2.4.0
cmake --build build-mock-coverage-core-main --parallel 4
ctest --test-dir build-mock-coverage-core-main --output-on-failure
```

The first full run was **85/86**, with the stale stat-ID expectation as its only
failure; its log/XML is retained separately. After the test correction, the
focused actor-stats case passed. Final full-suite evidence is recorded in the
follow-up receipt. The host-source-only configuration still omits this fixture;
the explicit matching plugin archive closes coverage for this composed profile.

Remaining gates from e031 remain separate: the full SDK installed-header audit's
OS symlink issue, exact-batch Windows validation, and native/game qualification.
This work neither changes those guards nor promotes a dependency or release.
