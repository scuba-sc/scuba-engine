# Third-party notices

scuba-engine is GPL-3.0-only (see LICENSE). It vendors no third-party source code: the HTTP,
WebSocket (RFC 6455, including SHA-1 and base64), and JSON code under `src/hostd/` is written for
this project.

## sco-core SDK (GPL-3.0-only)

The build downloads `sco-sdk-host-source-sc-<version>.zip` from the scuba-sc/sco-core release
(verified by SHA-256) and links its libraries into `scuba-hostd`. The kit is GPL-3.0-only and
carries its own `THIRD_PARTY_NOTICES.md`, `LICENSES/` and SBOM; the components it builds into
`scuba-hostd` include:

- Lua 5.4 (MIT)
- SQLite (public domain; see the kit's `LICENSES/blessing.txt`)
- toml++ (MIT)

Redistributions of `scuba-hostd` binaries must carry the kit's notices alongside this file.

## Dear ImGui 1.92.8 (docking) (MIT), linked into scuba-engine

Fetched by `cmake/ScubaDeps.cmake` from github.com/ocornut/imgui tag `v1.92.8-docking`
(SHA-256 `cb613f0d…afba62`) and compiled into the native editor; not vendored.

```
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## miniz 3.1.2 (MIT), linked into scuba-engine and scuba-pack

Fetched by `cmake/ScubaDeps.cmake` from the github.com/richgel999/miniz `3.1.2` release
(SHA-256 `f0446d86…140d9a`); used for DEFLATE in zip archives mounted by the VFS. Not vendored.

```
Copyright 2013-2014 RAD Game Tools and Valve Software
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## three.js (MIT), bundled into the studio build

```
The MIT License

Copyright © 2010-2026 three.js authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Build and test tools (not shipped)

Vite, Vitest, TypeScript and @types/three are development dependencies of `studio/` (see
`studio/package-lock.json` for exact versions and licences); they are not part of any built
artifact except Vite's small module-preload helper (MIT) in `studio/dist`.
