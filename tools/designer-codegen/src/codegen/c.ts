import { flatten, validate, widget, kinds, type Design, type Kind, type Widget } from '../model';
import { finish, header, kindConstant, nativePlugin, number, parseString, quote, readHeader, splitArgs, strings } from './shared';

function initializer(n: Widget, id: number, parent: number): string {
  const fields = [
    '.size = sizeof(sco_widget_node_1_1)', `.kind = ${kindConstant(n.kind)}`, `.id = ${id}u`, `.parent = ${parent}u`,
    `.flags = ${(n.disabled ? 1 : 0) | (n.password ? 2 : 0)}u`, `.label = ${quote(n.label)}`,
  ];
  if (n.kind === 'Toggle' || n.kind === 'Slider') fields.push(`.value = ${number(n.value)}`);
  if (n.kind === 'Slider') fields.push(`.minimum = ${number(n.minimum)}`, `.maximum = ${number(n.maximum)}`);
  if (n.kind === 'Combo' || n.kind === 'List') fields.push(`.index = ${n.index}`);
  if (n.kind === 'TextInput') fields.push(`.text = ${quote(n.text)}`);
  if (n.items.length && ['Combo', 'List', 'Table'].includes(n.kind)) fields.push(`.items = items${id}`, `.item_count = ${n.items.length}u`);
  if (n.kind === 'Table') fields.push(`.columns = ${n.columns}u`);
  return `    { .base = { ${fields.join(', ')} }, .tooltip = ${quote(n.tooltip)}, .severity = ${n.severity}u, .first_row = ${n.firstRow}u, .visible_rows = ${n.visibleRows}u },`;
}
export function generate(d: Design): string {
  validate(d);
  const flat = flatten(d.nodes);
  const items = flat.filter(({ node: n }) => n.items.length && ['Combo', 'List', 'Table'].includes(n.kind))
    .map(({ node: n, id }) => `static const char* const items${id}[] = {${n.items.map(quote).join(', ')}};`).join('\n');
  const body = `${items}\nstatic const sco_widget_node_1_1 designed_nodes[] = {\n` +
    flat.map(({ node, id, parent }) => initializer(node, id, parent)).join('\n') + '\n};\n';
  return header(d) + nativePlugin(d, 'c', body);
}
export function parse(source: string): Design {
  const d = readHeader(source);
  const items = new Map<string, string[]>();
  for (const m of source.matchAll(/static const char\* const (items\d+)\[\] = \{(.*)\};/g)) items.set(m[1], strings(m[2]));
  const nodes = new Map<number, Widget>();
  for (const line of source.split('\n')) {
    const m = line.trim().match(/^\{ \.base = \{ (.+) \}, (.+) \},$/);
    if (!m) continue;
    const fields = Object.fromEntries([...splitArgs(m[1]), ...splitArgs(m[2])].map(entry => {
      const field = entry.match(/^\.(\w+) = (.*)$/);
      if (!field) throw new Error('Invalid C field');
      return [field[1], field[2]];
    }));
    const kind = kinds.find(k => kindConstant(k) === fields.kind);
    if (!kind) throw new Error('Unknown C widget kind');
    const num = (key: string, fallback = 0) => fields[key] === undefined ? fallback : Number(fields[key].replace(/u$/, ''));
    const id = num('id');
    if (id !== nodes.size + 1) throw new Error('C ids must remain preorder positions');
    const n = widget(kind as Kind, String(id));
    n.label = parseString(fields.label);
    n.tooltip = parseString(fields.tooltip);
    n.disabled = (num('flags') & 1) !== 0; n.password = (num('flags') & 2) !== 0;
    n.value = num('value'); n.minimum = num('minimum'); n.maximum = num('maximum', 1);
    n.index = num('index', -1); n.text = fields.text ? parseString(fields.text) : '';
    if (fields.items && !items.has(fields.items)) throw new Error('Unknown C item array');
    if (['Combo', 'List', 'Table'].includes(n.kind)) n.items = fields.items ? items.get(fields.items)! : [];
    n.columns = num('columns', 2); n.severity = num('severity');
    n.firstRow = num('first_row'); n.visibleRows = num('visible_rows');
    const parent = num('parent');
    if (parent && !nodes.has(parent)) throw new Error('Parent must precede node');
    nodes.set(id, n);
    (parent ? nodes.get(parent)!.children : d.nodes).push(n);
  }
  return finish(d, source, generate(d));
}
