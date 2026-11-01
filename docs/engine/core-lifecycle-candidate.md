# Checked host-kernel lifecycle candidate

The checked lifecycle integration is an opt-in development build for the engine's existing
`Kernel` and editor host adapter. It consumes the exact host-source kit generated from sco-core
commit `349911d37a2fe5bb4acbafc972615578b9da82a0`; the archive contains 986 members and has
SHA-256 `3c152ccbf9b8881edf7ac472603f46905ed3c51556ddeea0a5b3a15083683844`. The dedicated
`checked Kernel lifecycle candidate` workflow regenerates that archive from the pinned clean
source checkout, verifies its inventory and embedded source identity, then builds and tests on
Linux and Windows.

Core main later advanced to `6106fa3639cbcc28f78a1ded8de2707de019f4d3` to finalize the
`sdk-v2.4.0` candidate designation. The source diff from `349911d` changes only the changelog and
candidate-tag metadata; the final host-source package and manifest for `6106fa3` are still pending.
The receipts in this document therefore qualify the exact `349911d` kit only, not the later
release-candidate archive. Once the finalized package is published, its exact manifest, hash and
source identity must be verified before updating this profile.

The normal profile remains the released RC2 kit configured in `cmake/ScubaSdk.cmake`. A local
candidate configure must opt in and provide all three coordinated source values:

```sh
cmake -S . -B build-kernel-lifecycle -G Ninja \
  -DSCUBA_CORE_LIFECYCLE_CANDIDATE=ON \
  -DSCUBA_CORE_LIFECYCLE_SOURCE_COMMIT=349911d37a2fe5bb4acbafc972615578b9da82a0 \
  -DSCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256=3c152ccbf9b8881edf7ac472603f46905ed3c51556ddeea0a5b3a15083683844 \
  -DSCUBA_SDK_ARCHIVE_DIR=/path/to/the-verified-kit \
  -DSCUBA_SDK_HOST_SOURCE_SHA256=3c152ccbf9b8881edf7ac472603f46905ed3c51556ddeea0a5b3a15083683844
cmake --build build-kernel-lifecycle
ctest --test-dir build-kernel-lifecycle --output-on-failure
```

The candidate Kernel calls `sco::app::StartDetailed` and classifies its return before the caller
can treat startup as successful. Validation or staging failure before the core call cleans up and
releases the local startup reservation. A core refusal describes only that invocation; because
the API does not prove that another direct core owner has no shared mock state, the consumer keeps
its provider configuration, staged snapshot and process reservation until exit. A startup result
of `RetainDependencies` has the same process-lifetime behavior. Neither state permits restart.
Current consumers pass process-lifetime log functions; a caller must not pass code from an
unloadable module or a callback whose code lifetime ends before the process.
After `StartDetailed` succeeds, the checked `StopDetailed` path propagates core `Stop()` results.
Only `Ok`, with no module pins and successful stage removal, clears the mock provider and local
reservation. Wrong-thread or unavailable stop is a refusal that the app shutdown path treats as
fail-stop; a failed stop retains the process root.

The hostd entrypoint and engine editor keep their owning callback and UI stacks alive while
attempting checked stop. On a retained or uncertain result, they terminate the process before
those stacks are destroyed. Normal teardown stops and joins the independent NetSandbox worker
first. Each peer closes its input stream, runs core Stop on its game thread, and exits; the parent
records the actual child exit status and waits for process termination before removing peer data.
Forced termination or a nonzero child exit is reported as an unclean stop, but the parent has
confirmed the child is gone before returning. Failed temporary-directory removal leaves the path
recorded in the live broker so its destructor can make one final cleanup attempt. A failed staged
plugin-snapshot removal is retained by the process root and is not retried before process exit.
The released RC2 profile still has a void core stop API; its child exit status proves process exit,
not a checked core-stop result. Only the candidate profile exercises result-returning shutdown.

The lifecycle tests use separate processes for clean start/stop/restart, wrong-thread stop,
preflight refusal, core startup refusal, retained startup after provider publication, and failed
dependent stop. They assert staged files and provider state remain live for retained outcomes.
The existing native NetSandbox test covers loopback peers and orderly child retirement. These are
synthetic consumer tests; they do not test an installed game, Windows game ABI, or native provider
activation. Core's native-reader exclusion remains unavailable, and this profile does not enable
it. The candidate option is not a release pin or production default.
