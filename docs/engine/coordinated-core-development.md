# Coordinated Core development profile

This is the current development integration path, not a release or game-runtime acceptance. It consumes an unpublished `2.4.1-dev.1` host-source SDK built from clean coordinated Core commit `38185eb8b00fab4b7d6c1652536839bb97d1141d`. Its filename is `sco-sdk-host-source-sc-2.4.1-dev.1.zip`. The version names development bytes and does not imply a GitHub release tag. The final archive and its SHA-256 have not yet been produced in this workspace; do not reuse the published `2.4.0` digest or the historical bf56 rehearsal digest.

With a full toolchain, package the final clean, exact Core checkout using its development mode, preserving Git executable modes on all platforms:

```sh
set -eu
git -C /path/to/coordinated-core status --porcelain
test "$(git -C /path/to/coordinated-core rev-parse HEAD)" = 38185eb8b00fab4b7d6c1652536839bb97d1141d
test -z "$(git -C /path/to/coordinated-core status --porcelain --untracked-files=all)"
python tools/package_dev_sdk.py /path/to/coordinated-core /path/to/assets --development
python /path/to/coordinated-core/tools/ip_audit.py zip --development --profile host-source-sc \
  /path/to/assets/sco-sdk-host-source-sc-2.4.1-dev.1.zip
```

Record the generated archive's SHA-256 and full source commit as part of the same trusted build handoff. Verify the hash and the embedded source, version, and member inventory before configuring. Set the directory and verified host archive hash from that handoff; the values below are placeholders to be replaced, not archive identities. The preset fixes the source commit to the coordinated candidate:

```sh
export SCUBA_DEV_SDK_ARCHIVE_DIR=/path/to/assets
export SCUBA_DEV_SDK_HOST_SHA256=<verified-host-archive-sha256>
export SCUBA_DEV_CORE_COMMIT=38185eb8b00fab4b7d6c1652536839bb97d1141d
python tools/verify_dev_sdk.py "$SCUBA_DEV_SDK_ARCHIVE_DIR/sco-sdk-host-source-sc-2.4.1-dev.1.zip" "$SCUBA_DEV_SDK_HOST_SHA256" "$SCUBA_DEV_CORE_COMMIT"
cmake --preset coordinated-core-development
cmake --build --preset coordinated-core-development
ctest --preset coordinated-core-development --output-junit full-ctest.xml
```

The preset fixes the full Core source commit and requires the local directory, a full SHA-256, checked lifecycle enabled, and matching lifecycle/archive hashes. Missing or inconsistent inputs fail closed; there is no development release download. Configure rechecks the archive hash, metadata and complete member inventory and tests the checked-lifecycle API. Use a fresh build tree for each coordinated input and rebuild every C++ consumer together: Core's `Platform` and `HostReport` layouts have changed even though the plugin C ABI is frozen. A configuration or a syntax-only check is not a linked build, runtime verification, or evidence of safe operation in a game.

The default plain CMake command still selects the published 2.4.0 stable profile for historical release workflows. The separate `core-main-bf56-rehearsal` preset and its workflow retain their original inputs and evidence; neither is this development candidate. A full-toolchain local rebuild and applicable test suites are still required against the final archive on each supported platform.
