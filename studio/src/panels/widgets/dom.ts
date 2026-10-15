import type { WidgetEventParams } from './types';
import type { VNode } from './view';

export function toDom(v: VNode, doc: Document, send: (e: WidgetEventParams) => void): HTMLElement {
  const el = doc.createElement(v.tag);
  for (const [k, value] of Object.entries(v.attrs ?? {})) el.setAttribute(k, value);
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
