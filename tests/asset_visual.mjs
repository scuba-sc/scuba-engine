// Run with Bun on macOS against the built host from the worktree root.
import { mkdir, rm } from 'node:fs/promises';
import { resolve } from 'node:path';

const artifacts = process.argv[2];
if (!artifacts) throw new Error('usage: bun tests/asset_visual.mjs <artifact-directory>');
await mkdir(artifacts, { recursive: true });
const base = 'build/tests/assets-fixtures/';
let captures = 0;
async function ready(view, expression) {
  const result = await view.evaluate(`new Promise((resolve, reject) => {
    const check = () => { if (${expression}) { observer.disconnect(); clearTimeout(timer); resolve(true); } };
    const observer = new MutationObserver(check);
    const timer = setTimeout(() => { observer.disconnect(); reject(new Error("Asset Lab state timeout")); }, 10000);
    observer.observe(document.body, { subtree: true, childList: true, characterData: true, attributes: true });
    check();
  })`);
  if (result !== true) throw new Error('Browser did not observe expected state');
}
async function fill(view, name, value) {
  await view.evaluate(`document.querySelector('[name="${name}"]').value = ${JSON.stringify(value)}`);
}
async function capture(view, suffix) {
  await Bun.write(resolve(artifacts, `eng6-${suffix}.png`), await view.screenshot());
  captures++;
}
async function openDatabase(view) {
  await fill(view, 'database', `${base}tiny.dcb`);
  await view.click('.asset-source button');
  await ready(view, 'document.querySelector(".asset-records button") && !document.querySelector(".asset-source button").disabled');
  await view.evaluate('document.querySelector(".asset-records button").scrollIntoView({ block: "center" })');
  await view.click('.asset-records button');
  await ready(view, 'document.querySelector(".asset-fields").innerText.includes("TriangleB")');
}
async function loadModel(view, file) {
  await fill(view, 'model', base + file);
  await view.evaluate('document.querySelector(".asset-model button").scrollIntoView({ block: "center" })');
  await view.click('.asset-model button');
  await ready(view, '!document.querySelector(".asset-model button").disabled && !document.querySelector("[data-action=glb]").disabled');
  const error = await view.evaluate('document.querySelector(".asset-status").dataset.error === "true"');
  if (error) throw new Error(await view.evaluate('document.querySelector(".asset-model-info").textContent'));
}

const desktop = new Bun.WebView({ width: 1440, height: 1100 });
try {
  await desktop.navigate('http://127.0.0.1:17966/#assets');
  await ready(desktop, 'document.querySelector(".asset-source button")');
  await capture(desktop, 'empty-desktop');
  await openDatabase(desktop);
  await loadModel(desktop, 'triangle.cgf');
  await capture(desktop, 'records-mesh-desktop');
  await desktop.evaluate('(() => { for (let n = 0; n < 40; n++) document.querySelector("canvas").dispatchEvent(new KeyboardEvent("keydown", { code: "ArrowRight", ctrlKey: true, bubbles: true, cancelable: true })); return true; })()');
  await capture(desktop, 'mesh-orbit-side');
  await desktop.evaluate('(() => { for (let n = 0; n < 25; n++) document.querySelector("canvas").dispatchEvent(new KeyboardEvent("keydown", { code: "ArrowUp", ctrlKey: true, bubbles: true, cancelable: true })); return true; })()');
  await capture(desktop, 'mesh-orbit-top');
  await desktop.click('.asset-fields button');
  await ready(desktop, 'document.querySelector("textarea").value.includes("[[set]]")');
  await fill(desktop, 'pack', 'format = 1\n[[set]]\nrecord = "TriangleA"\nfield = "speed"\nvalue = 42.5\n');
  await desktop.click('[data-action=check]');
  await ready(desktop, 'document.querySelector(".asset-status").textContent.includes("validated against")');
  await desktop.click('[data-action=structs]');
  await ready(desktop, 'document.querySelector("[data-selection]").textContent === "Struct definitions" && !document.querySelector("[data-action=structs]").disabled');
  await desktop.click('.asset-fields summary');
  await capture(desktop, 'structs-desktop');
  await loadModel(desktop, 'red.dds');
  await capture(desktop, 'dds-desktop');
  await fill(desktop, 'model', `${base}triangle.obj`);
  await desktop.click('.asset-import summary');
  await fill(desktop, 'destination', `${base}visual-pack`);
  await rm(resolve(`${base}visual-pack`), { recursive: true, force: true });
  await desktop.evaluate('document.querySelector(".asset-import button").scrollIntoView({ block: "center" })');
  await desktop.click('.asset-import button');
  await ready(desktop, 'document.querySelector(".asset-status").textContent.includes("Pack created")');
  await capture(desktop, 'import-desktop');
  await fill(desktop, 'model', `${base}escape.gltf`);
  await desktop.evaluate('document.querySelector(".asset-model button").scrollIntoView({ block: "center" })');
  await desktop.click('.asset-model button');
  await ready(desktop, 'document.querySelector(".asset-status").dataset.error === "true"');
  await capture(desktop, 'error-desktop');
  const overflow = await desktop.evaluate('document.documentElement.scrollWidth > innerWidth');
  if (overflow) throw new Error('Desktop horizontal overflow');
} finally {
  desktop.close();
}
const phone = new Bun.WebView({ width: 390, height: 844 });
try {
  await phone.navigate('http://127.0.0.1:17966/#assets');
  await ready(phone, 'document.querySelector(".asset-source button")');
  await openDatabase(phone);
  await capture(phone, 'records-mobile');
  await loadModel(phone, 'triangle.cgf');
  await phone.evaluate('document.querySelector(".asset-preview-section").scrollIntoView()');
  await capture(phone, 'mesh-mobile');
  if (await phone.evaluate('document.documentElement.scrollWidth > innerWidth')) throw new Error('Mobile horizontal overflow');
} finally {
  phone.close();
}
console.log(`ASSET_VISUAL_PASS captures=${captures}; DCB, GUID fields, typed override, structs, Cry mesh, BC1 DDS, OBJ pack import, traversal error, desktop/mobile`);
