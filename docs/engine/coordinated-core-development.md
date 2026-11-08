# Coordinated Core development profile

This is the current development integration path, not a release or game-runtime acceptance. It consumes an unpublished `2.4.1-dev.1` host-source SDK built from the selected clean coordinated Core commit. Its filename is `sco-sdk-host-source-sc-2.4.1-dev.1.zip`. The version names development bytes and does not imply a GitHub release tag. The final archive and its SHA-256 have not yet been produced in this workspace; do not reuse the published `2.4.0` digest or the historical bf56 rehearsal digest.

With a full toolchain, package the final clean, exact Core checkout using its development mode, preserving Git executable modes on all platforms:

```sh
git -C /path/to/coordinated-core status --porcelain
# Verify HEAD is the selected exact Core commit and the checkout is clean.
python tools/package_dev_sdk.py /path/to/coordinated-core /path/to/assets --development
```

Record the generated archive's SHA-256 and full source commit as part of the same trusted build handoff. Verify the hash and the embedded source, version, and member inventory before configuring. Set the three values from that handoff; values below are placeholders to be replaced, not archive identities:

```sh
export SCUBA_DEV_SDK_ARCHIVE_DIR=/path/to/assets
export SCUBA_DEV_SDK_HOST_SHA256=<verified-host-archive-sha256>
export SCUBA_DEV_CORE_COMMIT=<selected-full-core-commit>
python tools/verify_dev_sdk.py "$SCUBA_DEV_SDK_ARCHIVE_DIR/sco-sdk-host-source-sc-2.4.1-dev.1.zip" "$SCUBA_DEV_SDK_HOST_SHA256" "$SCUBA_DEV_CORE_COMMIT"
cmake --preset coordinated-core-development
cmake --build --preset coordinated-core-development
ctest --preset coordinated-core-development --output-junit full-ctest.xml
```

The preset requires the local directory, a full SHA-256 and source commit, checked lifecycle enabled, and matching lifecycle/archive hashes. Missing or inconsistent inputs fail closed; there is no development release download. Configure rechecks the archive hash, metadata and complete member inventory and tests the checked-lifecycle API. Use a fresh build tree for each coordinated input and rebuild every C++ consumer together: Core's `Platform` and `HostReport` layouts have changed even though the plugin C ABI is frozen. A configuration or a syntax-only check is not a linked build, runtime verification, or evidence of safe operation in a game.

The default plain CMake command still selects the published 2.4.0 stable profile for historical release workflows. The separate `core-main-bf56-rehearsal` preset and its workflow retain their original inputs and evidence; neither is this development candidate. A full-toolchain local rebuild and applicable test suites are still required against the final archive on each supported platform.
