import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile, readdir, mkdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { resolve, extname } from 'node:path';

// The driver is supplied by the verification host, not a studio runtime dependency.
const { chromium } = await import(process.env.SCUBA_BROWSER_MODULE || 'playwright');
const dist = resolve(fileURLToPath(new URL('../../../dist/', import.meta.url)));
const output = process.env.SCUBA_VISUAL_DIR;
assert(output, 'SCUBA_VISUAL_DIR is required');
await mkdir(output, { recursive: true });
const mime = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };
const server = createServer(async (request, response) => {
  const pathname = new URL(request.url || '/', 'http://localhost').pathname;
  if (pathname === '/ws') {
    response.writeHead(503).end();
    return;
  }
  const path = resolve(dist, pathname === '/' ? 'index.html' : `.${pathname}`);
  if (!path.startsWith(`${dist}/`)) {
    response.writeHead(403).end();
    return;
  }
  try {
    const bytes = await readFile(path);
    response.writeHead(200, { 'Content-Type': mime[extname(path)] || 'application/octet-stream' }).end(bytes);
  } catch {
    response.writeHead(404).end();
  }
});
await new Promise((resolveReady) => server.listen(0, '127.0.0.1', resolveReady));
const address = server.address();
assert(address && typeof address !== 'string');
const browser = await chromium.launch({
  headless: true,
  ...(process.env.SCUBA_BROWSER_EXECUTABLE ? { executablePath: process.env.SCUBA_BROWSER_EXECUTABLE } : {}),
});
const files = [];
const results = [];
try {
  for (const width of [1440, 390]) {
    const context = await browser.newContext({ viewport: { width, height: width === 390 ? 844 : 900 }, deviceScaleFactor: 1,
      isMobile: width === 390, hasTouch: width === 390, reducedMotion: 'reduce' });
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', (error) => errors.push(error.message));
    await page.goto(`http://127.0.0.1:${address.port}/#viewport`);
    await page.locator('.diegetic[data-ready="true"]').waitFor();
    const take = async (name) => {
      await page.evaluate(() => new Promise(requestAnimationFrame));
      const path = resolve(output, `eng5-${name}-${width}.png`);
      await page.screenshot({ path });
      files.push(path.split('/').at(-1));
    };
    const range = async (label, value) => {
      await page.getByRole('slider', { name: label, exact: true }).evaluate((input, next) => {
        input.value = String(next);
        input.dispatchEvent(new Event('input', { bubbles: true }));
      }, value);
    };
    for (const [index, id] of ['elevator', 'asop', 'shop', 'engineering', 'cockpit', 'hud'].entries()) {
      await page.locator('.vp-control select').selectOption({ index });
      for (const mode of ['quad', 'billboard']) {
        await page.locator(`button[data-mode="${mode}"]`).click();
        assert.equal(await page.locator('.vp-surface').isVisible(), true, `${id}/${mode}/${width}`);
        const overflow = await page.evaluate(() => document.documentElement.scrollWidth > innerWidth);
        assert.equal(overflow, false, `horizontal overflow ${width}`);
        await take(`${id}-${mode}`);
        const hit = await page.locator('.vp-terminal-actions button').first().evaluate((button) => {
          const wrapper = document.querySelector('.vp-surface');
          const stage = document.querySelector('.vp-stage');
          let x = button.offsetWidth / 2;
          let y = button.offsetHeight / 2;
          let current = button;
          while (current && current !== wrapper) {
            x += current.offsetLeft;
            y += current.offsetTop;
            current = current.offsetParent;
          }
          const matrix = new DOMMatrix(getComputedStyle(wrapper).transform);
          const p = matrix.transformPoint(new DOMPoint(x, y));
          const rect = stage.getBoundingClientRect();
          return { screen: [p.x / p.w + rect.x, p.y / p.w + rect.y], panel: [x, y] };
        });
        await page.mouse.click(...hit.screen);
        const text = await page.locator('.vp-readout [role="status"]').innerText();
        assert.match(text, /Input: .* px \/ #\d+/, `pointer action ${id}/${mode}/${width}`);
        assert(await page.locator('.vp-readout').evaluate((node) => node.getBoundingClientRect().bottom <= innerHeight + 1),
          `readout clipped ${id}/${mode}/${width}`);
        const coords = text.match(/\/ ([\d.]+), ([\d.]+) px/);
        assert(coords);
        assert(Math.abs(Number(coords[1]) - hit.panel[0]) < 1, `local x ${text}`);
        assert(Math.abs(Number(coords[2]) - hit.panel[1]) < 1, `local y ${text}`);
        await page.getByLabel('Simulate occlusion').check();
        assert.equal(await page.locator('.vp-surface').isVisible(), false);
        assert.match(await page.locator('.vp-readout strong').innerText(), /OCCLUDED/);
        if (id === 'asop') await take(`occluded-${mode}`);
        await page.getByLabel('Simulate occlusion').uncheck();
        assert.equal(await page.locator('.vp-surface').isVisible(), true);
        results.push(`${width}/${id}/${mode}: visible, pointer, ray occlusion, overflow PASS`);
      }
    }
    await page.locator('.vp-control select').selectOption({ index: 1 });
    await page.locator('button[data-mode="quad"]').click();
    for (const yaw of [-55, 55]) {
      await range('Yaw', yaw);
      await range('Pitch', yaw < 0 ? -20 : 30);
      await range('FOV', 65);
      assert.equal(await page.locator('.vp-surface').isVisible(), true);
      await take(`angle-${yaw < 0 ? 'left' : 'right'}`);
    }
    await range('Yaw', 0);
    await range('Pitch', 0);
    await range('FOV', 50);
    await page.locator('button[data-mode="billboard"]').click();
    const [first, second] = await page.evaluate(() => {
      const input = document.querySelector('input[aria-label="Distance"]');
      const surface = document.querySelector('.vp-surface');
      return [3.5, 7].map((distance) => {
        input.value = String(distance);
        input.dispatchEvent(new Event('input', { bubbles: true }));
        return new DOMMatrix(getComputedStyle(surface).transform).m11;
      });
    });
    assert(Math.abs(first / second - 2) < 1e-5, `billboard scale: ${first} / ${second}`);
    await page.locator('.vp-terminal-actions button').first().focus();
    await page.keyboard.press('Enter');
    assert.match(await page.locator('.vp-readout [role="status"]').innerText(), /keyboard/);
    await page.getByRole('tab', { name: 'Home', exact: true }).click();
    assert.equal(await page.locator('.vp-stage').count(), 0);
    await page.getByRole('tab', { name: 'Viewport', exact: true }).click();
    await page.locator('.diegetic[data-ready="true"]').waitFor();
    assert.equal(errors.length, 0, errors.join('\n'));
    await context.close();
  }
  const assets = await readdir(resolve(dist, 'assets'));
  assert(assets.some((name) => name.startsWith('stage-') && name.endsWith('.js')));
  console.log(results.join('\n'));
  console.log(`VISUAL PASS: ${files.length} screenshots; pointer coordinates, keyboard, inverse-distance scale, occlusion, remount, zero page errors.`);
  console.log(files.join('\n'));
} finally {
  await browser.close();
  server.closeAllConnections();
  await new Promise((resolveClosed) => server.close(resolveClosed));
}
