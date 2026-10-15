import { describe, expect, it, vi } from 'vitest';
import { mountAssetLab } from './panel';
import type { HostdBridge } from '../../bridge';

class MockElement {
  tagName: string;
  className = '';
  textContent = '';
  name = '';
  placeholder = '';
  spellcheck = false;
  required = false;
  type = '';
  value = '';
  disabled = false;
  id = '';
  private _dataset: Record<string, string> = {};
  attributes = new Map<string, string>();
  children: MockElement[] = [];
  parentNode: MockElement | null = null;

  constructor(tag: string) {
    this.tagName = tag.toUpperCase();
    const self = this;
    this._dataset = new Proxy<Record<string, string>>({}, {
      set(target, prop: string, val: string) {
        target[prop] = val;
        const attr = 'data-' + prop.replace(/([A-Z])/g, '-$1').toLowerCase();
        self.attributes.set(attr, val);
        return true;
      },
      get(target, prop: string) {
        return target[prop];
      }
    });
  }

  get dataset() {
    return this._dataset;
  }

  setAttribute(k: string, v: string) {
    this.attributes.set(k, v);
    if (k.startsWith('data-')) {
      const prop = k.slice(5).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
      this._dataset[prop] = v;
    }
  }

  getAttribute(k: string) {
    return this.attributes.get(k) ?? null;
  }

  append(...nodes: (MockElement | string)[]) {
    for (const node of nodes) {
      if (typeof node === 'string') {
        const textNode = new MockElement('#text');
        textNode.textContent = node;
        this.children.push(textNode);
      } else {
        node.parentNode = this;
        this.children.push(node);
      }
    }
  }

  remove() {
    if (this.parentNode) {
      const idx = this.parentNode.children.indexOf(this);
      if (idx !== -1) this.parentNode.children.splice(idx, 1);
      this.parentNode = null;
    }
  }

  private matchesSingle(selector: string): boolean {
    let sel = selector;
    let tag = '';

    const tagMatch = sel.match(/^([a-zA-Z0-9]+)/);
    if (tagMatch) {
      tag = tagMatch[1].toUpperCase();
      sel = sel.slice(tagMatch[1].length);
      if (this.tagName !== tag) return false;
    }

    if (!sel) return true;

    if (sel.startsWith('.')) {
      return this.className.split(/\s+/).includes(sel.slice(1));
    }

    if (sel.startsWith('#')) {
      return this.id === sel.slice(1);
    }

    const attrValMatch = sel.match(/^\[([a-zA-Z0-9-]+)="([^"]+)"\]$/);
    if (attrValMatch) {
      const [, attrName, attrVal] = attrValMatch;
      if (attrName === 'name') return this.name === attrVal;
      return this.attributes.get(attrName) === attrVal;
    }

    const attrHasMatch = sel.match(/^\[([a-zA-Z0-9-]+)\]$/);
    if (attrHasMatch) {
      const [, attrName] = attrHasMatch;
      return this.attributes.has(attrName);
    }

    return false;
  }

  querySelector<T = MockElement>(selector: string): T | null {
    return this.querySelectorAll<T>(selector)[0] ?? null;
  }

  querySelectorAll<T = MockElement>(selector: string): T[] {
    const parts = selector.trim().split(/\s+/);
    if (parts.length > 1) {
      const firstPart = parts[0];
      const rest = parts.slice(1).join(' ');
      const parents = this.querySelectorAll(firstPart);
      const results: MockElement[] = [];
      for (const parent of parents) {
        results.push(...parent.querySelectorAll(rest));
      }
      return results as unknown as T[];
    }

    const results: MockElement[] = [];
    const walk = (el: MockElement) => {
      if (el.matchesSingle(selector)) results.push(el);
      for (const child of el.children) walk(child);
    };

    walk(this);
    return results as unknown as T[];
  }
}

function setupMockDocument() {
  const origDoc = globalThis.document;
  Object.defineProperty(globalThis, 'document', {
    value: {
      createElement(tag: string) {
        return new MockElement(tag);
      },
    },
    writable: true,
    configurable: true,
  });
  return () => {
    Object.defineProperty(globalThis, 'document', {
      value: origDoc,
      writable: true,
      configurable: true,
    });
  };
}

describe('mountAssetLab', () => {
  it('mounts asset lab DOM structure without innerHTML', () => {
    const restoreDoc = setupMockDocument();
    try {
      const container = document.createElement('div') as unknown as HTMLElement;
      const mockBridge: HostdBridge = {
        call: vi.fn().mockResolvedValue({}),
        on: vi.fn(),
      } as unknown as HostdBridge;

      const unmount = mountAssetLab(container, mockBridge);

      const mockContainer = container as unknown as MockElement;
      const root = mockContainer.querySelector('.asset-lab');
      expect(root).not.toBeNull();

      // Verify key headers, forms, inputs, buttons
      expect(mockContainer.querySelector('.asset-lab')?.querySelector('h1')?.textContent).toBe('Asset Lab');
      expect(mockContainer.querySelector('input[name="database"]')).not.toBeNull();
      expect(mockContainer.querySelector('button[data-action="structs"]')).not.toBeNull();
      expect(mockContainer.querySelector('textarea[name="pack"]')).not.toBeNull();
      expect(mockContainer.querySelector('input[name="model"]')).not.toBeNull();
      expect(mockContainer.querySelector('button[data-action="glb"]')).not.toBeNull();

      // Verify cleanup
      unmount();
      expect(mockContainer.querySelector('.asset-lab')).toBeNull();
    } finally {
      restoreDoc();
    }
  });
});
