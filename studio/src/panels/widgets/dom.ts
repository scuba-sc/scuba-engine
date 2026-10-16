import type { WidgetEventParams } from './types';
import type { VNode } from './view';

// Widget trees carry plugin-supplied strings. Event handlers and URL-bearing attributes could
// turn one into script, so they are never set from a VNode; listeners only come from `on`.
const BLOCKED_ATTR = /^(on|formaction$|action$|href$|src$|srcdoc$|xlink:href$|style$)/i;

export function isSafeAttr(name: string): boolean {
  return !BLOCKED_ATTR.test(name);
}

export function toDom(v: VNode, doc: Document, send: (e: WidgetEventParams) => void): HTMLElement {
  const el = doc.createElement(v.tag);
  for (const [k, value] of Object.entries(v.attrs ?? {})) if (isSafeAttr(k)) el.setAttribute(k, value);
  if (v.text !== undefined) el.textContent = v.text;
  if (el instanceof HTMLInputElement) {
    if (v.attrs?.value !== undefined) el.value = v.attrs.value;
    el.checked = v.attrs?.checked !== undefined;
  }
  for (const child of v.children ?? []) el.append(toDom(child, doc, send));
  if (el instanceof HTMLSelectElement) {
    const selected = v.children?.findIndex((c) => c.attrs?.selected !== undefined) ?? -1;
    if (selected >= 0) el.selectedIndex = selected;
  }
  for (const [type, handler] of Object.entries(v.on ?? {})) {
    if (!handler) continue;
    el.addEventListener(type, () => {
      const control = el as HTMLInputElement;
      const params = handler({ value: control.value ?? '', checked: Boolean(control.checked) });
      if (params) send(params);
    });
  }
  return el;
}
