# designer-codegen

Headless source generators for widget designs: a design tree (`src/model.ts`) in, plugin source out
for C, C++, C# and Lua (`src/codegen/`). No DOM, no bundler, no dependencies; it runs under Bun.

It exists so the generated-plugin check does not depend on the browser studio. This is a copy of
`studio/src/panels/designer/model.ts` and `studio/src/codegen/`; the studio keeps its own copy for
now, so a change to one belongs in the other until the studio's copy is removed.

```sh
cd tools/designer-codegen
bun test
```

`tests/designer-plugins.mjs` imports from here. ctest runs it as `designer_generated_plugins`
(it builds each generated plugin against the SDK and runs the SDK checker).
