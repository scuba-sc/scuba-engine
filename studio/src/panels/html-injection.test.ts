// @vitest-environment happy-dom
// Regression tests for scuba-engine #3: plugin, widget and asset-catalogue strings must render
// as inert text. happy-dom has a real HTML parser, so any string that reached an HTML sink
// would create an <img>/<script> element here and fail the element checks below.
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { HostdBridge } from '../bridge';
import { mountAssetLab } from './assets/panel';
import { renderWidgets } from './netsim/widgets';
import type { WidgetPanel as NetsimPanel, WidgetNode as NetsimNode } from './netsim/types';
import { isSafeAttr, toDom } from './widgets/dom';
import type { WidgetNode, WidgetPanel } from './widgets/types';
import { renderPanelView } from './widgets/view';

const PAYLOADS = [
  '<img src=x onerror="window.__pwned=1">',
  '"><svg onload="window.__pwned=1">',
  '<script>window.__pwned=1</script>',
  "'><iframe srcdoc='<script>parent.__pwned=1</script>'>",
];
const ALL = PAYLOADS.join(' | ');

declare global { interface Window { __pwned?: number } }

function assertInert(root: Element): void {
  expect(root.querySelectorAll('img, svg, script, iframe').length).toBe(0);
  for (const el of [root, ...root.querySelectorAll('*')]) {
    for (const attr of Array.from(el.attributes)) expect(attr.name).not.toMatch(/^on/i);
  }
  expect(window.__pwned).toBeUndefined();
}

afterEach(() => { document.body.replaceChildren(); delete window.__pwned; });

function widgetNode(id: number, kind: WidgetNode['kind'], fields: Partial<WidgetNode> = {}): WidgetNode {
  return {
    id, parent: 0, kind, kind_code: 0, disabled: false, password: false, label: ALL, text: ALL,
    value: 0, minimum: 0, maximum: 1, index: 0, items: PAYLOADS, columns: 2, extended: false,
    severity: 'warning', tooltip: ALL, first_row: 0, visible_rows: 0, ...fields,
  };
}

describe('Widgets panel (plugin widget trees)', () => {
  it('renders every payload-bearing node kind as text', () => {
    const nodes = [
      widgetNode(1, 'label'), widgetNode(2, 'button'), widgetNode(3, 'toggle'), widgetNode(4, 'slider'),
      widgetNode(5, 'combo'), widgetNode(6, 'list'), widgetNode(7, 'text_input'), widgetNode(8, 'table'),
    ];
    const panel: WidgetPanel = { id: 'demo.main', title: ALL, owner: ALL, revision: 1, extended: false, nodes };
    const root = toDom(renderPanelView(panel), document, () => {});
    document.body.append(root);
    assertInert(root);
    for (const payload of PAYLOADS) expect(root.textContent).toContain(payload);
    expect(root.querySelector<HTMLInputElement>('input[type="text"]')!.value).toBe(ALL);
    expect(root.querySelector('[title]')!.getAttribute('title')).toBe(ALL);
  });

  it('never sets event-handler or URL attributes from a VNode', () => {
    for (const name of ['onerror', 'ONCLICK', 'onload', 'href', 'src', 'srcdoc', 'formaction', 'style']) {
      expect(isSafeAttr(name)).toBe(false);
    }
    for (const name of ['class', 'title', 'aria-label', 'value', 'data-node-id', 'disabled']) expect(isSafeAttr(name)).toBe(true);
    const el = toDom({ tag: 'a', attrs: { onerror: 'window.__pwned=1', href: 'javascript:window.__pwned=1', title: 'ok' } }, document, () => {});
    expect(el.getAttribute('onerror')).toBeNull();
    expect(el.getAttribute('href')).toBeNull();
    expect(el.getAttribute('title')).toBe('ok');
  });
});

describe('NetSandbox peer widgets', () => {
  it('renders plugin labels, items and tooltips as text', () => {
    const base: Omit<NetsimNode, 'id' | 'kind'> = {
      parent: 0, flags: 0, label: ALL, text: ALL, value: 0, minimum: 0, maximum: 1, index: 0, columns: 2,
      items: PAYLOADS, tooltip: ALL, severity: 2, firstRow: 0, visibleRows: 0,
    };
    const panel: NetsimPanel = { id: 'demo.peer', title: ALL, owner: ALL, nodes: [1, 2, 3, 4, 5, 6, 7, 8].map((kind) => ({ ...base, id: kind, kind })) };
    const root = renderWidgets(panel, () => {});
    document.body.append(root);
    assertInert(root);
    for (const payload of PAYLOADS) expect(root.textContent).toContain(payload);
  });
});

describe('Asset Lab (catalogue data from a local database)', () => {
  const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

  function fakeBridge(fail = false): HostdBridge {
    const responses: Record<string, unknown> = {
      'assets.open': { kind: 'dcb', pack: 'format = 1\n' },
      'assets.records': { total: 1, items: [{ name: ALL, struct: ALL, guid: PAYLOADS[0] }] },
      'assets.fields': [{ path: ALL, type: ALL, value: ALL, reference: { name: ALL, guid: 'g' } }],
      'assets.structs': [{ name: ALL, properties: [{ name: ALL, type: ALL, conversion: 0 }] }],
    };
    return {
      call: vi.fn(async (method: string) => {
        if (fail) throw new Error(ALL);
        return responses[method];
      }),
    } as unknown as HostdBridge;
  }

  it('builds its static layout without HTML parsing', () => {
    const host = document.createElement('div');
    document.body.append(host);
    const spy = vi.spyOn(Element.prototype, 'innerHTML', 'set');
    const dispose = mountAssetLab(host, fakeBridge());
    expect(spy).not.toHaveBeenCalled();
    spy.mockRestore();
    expect(host.querySelector('h1')?.textContent).toBe('Asset Lab');
    expect(host.querySelector('textarea')?.value).toBe('format = 1\natomic = true\n');
    expect(host.querySelector<HTMLButtonElement>('[data-action="glb"]')?.disabled).toBe(true);
    dispose();
  });

  it('renders record names, fields, references and structs as text', async () => {
    const host = document.createElement('div');
    document.body.append(host);
    const dispose = mountAssetLab(host, fakeBridge());
    host.querySelector<HTMLInputElement>('input[name="database"]')!.value = 'local.dcb';
    host.querySelector<HTMLFormElement>('.asset-source')!.dispatchEvent(new Event('submit', { cancelable: true }));
    await settle();
    host.querySelector<HTMLButtonElement>('.asset-records details button')!.click();
    await settle();
    assertInert(host);
    expect(host.querySelector('.asset-records')!.textContent).toContain(ALL);
    expect(host.querySelector('.asset-fields')!.textContent).toContain(`Open ${ALL}`);
    host.querySelector<HTMLButtonElement>('[data-action="structs"]')!.click();
    await settle();
    assertInert(host);
    expect(host.querySelector('.asset-fields summary')!.textContent).toBe(ALL);
    dispose();
  });

  it('renders host error messages as text', async () => {
    const host = document.createElement('div');
    document.body.append(host);
    const dispose = mountAssetLab(host, fakeBridge(true));
    host.querySelector<HTMLFormElement>('.asset-source')!.dispatchEvent(new Event('submit', { cancelable: true }));
    await settle();
    assertInert(host);
    expect(host.querySelector('.asset-status')!.textContent).toBe(ALL);
    dispose();
  });
});
