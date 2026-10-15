export function element<K extends keyof HTMLElementTagNameMap>(tag: K, text = '', className = ''): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  node.textContent = text;
  node.className = className;
  return node;
}
export function button(text: string, action: () => void, className = ''): HTMLButtonElement {
  const node = element('button', text, `designer-button ${className}`);
  node.type = 'button';
  node.addEventListener('click', action);
  return node;
}
export function field(label: string, value: string, change: (value: string) => void, type = 'text'): HTMLLabelElement {
  const wrapper = element('label', '', 'designer-field');
  const input = element('input');
  input.type = type;
  input.value = value;
  input.addEventListener('input', () => change(input.value));
  wrapper.append(element('span', label), input);
  return wrapper;
}
export function select(label: string, options: { value: string; label: string }[], value: string,
  change: (value: string) => void): HTMLLabelElement {
  const wrapper = element('label', '', 'designer-field');
  const input = element('select');
  input.append(...options.map(o => {
    const option = element('option', o.label);
    option.value = o.value;
    return option;
  }));
  input.value = value;
  input.addEventListener('change', () => change(input.value));
  wrapper.append(element('span', label), input);
  return wrapper;
}
