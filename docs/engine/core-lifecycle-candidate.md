# Checked host-kernel lifecycle integration

The checked lifecycle integration is enabled by default for the engine's `Kernel` and editor
host adapter. It consumes the published sco-core `sdk-v2.4.0` host-source kit from commit
`6106fa3639cbcc28f78a1ded8de2707de019f4d3`; the archive contains 986 members and has SHA-256
`a0aaaba07e3d4eb0d8c264912cd6f2ab23f0d237a99fa07816005790a76efc4c`. The dedicated
`checked Kernel lifecycle candidate` workflow downloads this immutable release archive, verifies
its checksum inventory and embedded source identity, then builds and tests on Linux and Windows.

The RC2 compatibility build remains available by explicitly disabling
`SCUBA_CORE_LIFECYCLE_CANDIDATE`; that older SDK exposes only void shutdown and cannot provide the
checked stop result used by the default profile. The unchecked profile is accepted only with the
exact RC2 tag and host-source archive hash below. Disabling checked lifecycle while selecting the
stable `sdk-v2.4.0` kit is a configure error, so the switch cannot silently discard the stable
SDK's shutdown result.

To select RC2 in a separate compatibility build, pin its archived bytes explicitly:

```sh
cmake -S . -B build-rc2 -G Ninja \
  -DSCUBA_CORE_LIFECYCLE_CANDIDATE=OFF \
  -DSCUBA_SDK_RELEASE_TAG=sdk-v2.4.0-rc.2 \
  -DSCUBA_SDK_HOST_SOURCE_SHA256=3d711463f7dff3dcd372ffb70b09951ecf21f1d0e2c5f0a899cccbb9977b8201
```

The default profile uses the released kit configured in `cmake/ScubaSdk.cmake`. A fresh build
verifies its source identity and archive checksum during configure. Existing build directories
may keep cached RC2 values; use a new build directory when moving them to the release. To validate
another exact coordinated kit, override the source and archive values explicitly:

```sh
cmake -S . -B build-kernel-lifecycle -G Ninja \
  -DSCUBA_CORE_LIFECYCLE_SOURCE_COMMIT=6106fa3639cbcc28f78a1ded8de2707de019f4d3 \
  -DSCUBA_CORE_LIFECYCLE_ARCHIVE_SHA256=a0aaaba07e3d4eb0d8c264912cd6f2ab23f0d237a99fa07816005790a76efc4c \
  -DSCUBA_SDK_ARCHIVE_DIR=/path/to/the-verified-kit \
  -DSCUBA_SDK_HOST_SOURCE_SHA256=a0aaaba07e3d4eb0d8c264912cd6f2ab23f0d237a99fa07816005790a76efc4c
cmake --build build-kernel-lifecycle
ctest --test-dir build-kernel-lifecycle --output-on-failure
```

The separate unpublished `bf56` issue-37 rehearsal also uses an unchecked core API. It is accepted
only through the `core-main-bf56-rehearsal` preset with an explicitly supplied local archive
directory, exact unpublished tag and archive hash, and `SCUBA_ALLOW_UNCHECKED_DEV_REHEARSAL=ON`.
This exception is only for that offline development rehearsal; it does not relax the stable
profile or provide checked shutdown semantics.

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
those stacks are destroyed. Hostd explicitly stops and reaps the NetSandbox broker before core
shutdown; the editor stops and joins its independent NetSandbox worker before destroying UI
owners. Each peer closes its input stream, runs core Stop on its game thread, and exits; the parent
records the actual child exit status and waits for process termination before removing peer data.
Forced termination or a nonzero child exit is reported as an unclean stop, but the parent has
confirmed the child is gone before returning. A failed temporary-directory removal gets one
cleanup retry before core stop; if it still fails, the path can remain on disk after process exit.
A failed staged plugin-snapshot removal is retained by the process root and is not retried before
process exit.
The released RC2 compatibility profile still has a void core stop API; a child exit status proves
process exit, not a checked core-stop result. The default `sdk-v2.4.0` profile exercises
result-returning shutdown.

The lifecycle tests use separate processes for clean start/stop/restart, wrong-thread stop,
preflight refusal, core startup refusal, retained startup after provider publication, and failed
dependent stop. They assert staged files and provider state remain live for retained outcomes.
The existing native NetSandbox test covers loopback peers and orderly child retirement. These are
synthetic consumer tests; they do not test an installed game, Windows game ABI, or native provider
activation. Core's native-reader exclusion remains unavailable, and this profile does not enable
it. The checked lifecycle profile does not enable native providers or claim game execution.
