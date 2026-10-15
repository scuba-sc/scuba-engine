export const kinds = ['Column', 'Row', 'Label', 'Button', 'Toggle', 'Slider', 'Combo', 'List', 'TextInput', 'Table'] as const;
export type Kind = typeof kinds[number];
export type Language = 'cpp' | 'csharp' | 'lua' | 'c';
export interface Widget {
  key: string;
  kind: Kind;
  label: string;
  disabled: boolean;
  password: boolean;
  value: number;
  minimum: number;
  maximum: number;
  index: number;
  text: string;
  items: string[];
  columns: number;
  tooltip: string;
  severity: number;
  firstRow: number;
  visibleRows: number;
  children: Widget[];
}
export interface Design { id: string; title: string; nodes: Widget[] }
export const isContainer = (n: Widget) => n.kind === 'Column' || n.kind === 'Row';
export function widget(kind: Kind, key: string): Widget {
  return { key, kind, label: isLayout(kind) ? '' : kind, disabled: false, password: false,
    value: kind === 'Slider' ? 0.5 : 0, minimum: 0, maximum: 1, index: -1,
    text: '', items: kind === 'Table' ? ['Item', 'Status', 'Probe', 'Ready'] :
      ['Combo', 'List'].includes(kind) ? ['Alpha', 'Beta', 'Gamma'] : [],
    columns: 2, tooltip: '', severity: 0, firstRow: 0, visibleRows: 0, children: [] };
}
const isLayout = (kind: Kind) => kind === 'Column' || kind === 'Row';
export function flatten(nodes: Widget[], parent = 0): { node: Widget; id: number; parent: number }[] {
  const out: { node: Widget; id: number; parent: number }[] = [];
  const visit = (entries: Widget[], owner: number) => {
    for (const node of entries) {
      const id = out.length + 1;
      out.push({ node, id, parent: owner });
      visit(node.children, id);
    }
  };
  visit(nodes, parent);
  return out;
}
export function validate(d: Design): void {
  const fail = (message: string): never => { throw new Error(message); };
  const text = (s: string, max = 255) => {
    if (typeof s !== 'string' || s.includes('\0') ||
      new TextDecoder().decode(new TextEncoder().encode(s)) !== s || new TextEncoder().encode(s).length > max)
      fail(`Text must be valid UTF-8, without NUL, at most ${max} bytes`);
  };
  if (!/^[a-z0-9_]{1,31}$/.test(d.id) || ['sco', 'host', 'menu', 'game'].includes(d.id)) fail('Invalid or reserved plugin id');
  text(d.title, 63);
  if (!d.title.trim()) fail('Panel title is required');
  const rows = flatten(d.nodes);
  if (!rows.length || rows.length > 256) fail('A panel needs 1..256 nodes');
  const keys = new Set<string>();
  for (const { node: n } of rows) {
    if (!kinds.includes(n.kind) || keys.has(n.key)) fail('Unknown widget kind or duplicate key');
    keys.add(n.key);
    text(n.label); text(n.text); text(n.tooltip);
    n.items.forEach(s => text(s));
    if (n.items.length > 256) fail('At most 256 items');
    if (!isContainer(n) && n.children.length) fail('Only Row and Column can have children');
    if (!['Column', 'Row', 'Label', 'Table'].includes(n.kind) && !n.label) fail('Interactive widgets need a label');
    if (isContainer(n) && n.label) fail('Builder containers do not have labels');
    if (n.kind === 'Slider' && (![n.value, n.minimum, n.maximum].every(Number.isFinite) ||
      n.value < n.minimum || n.value > n.maximum || n.minimum > n.maximum)) fail('Invalid slider range');
    if (n.kind === 'Toggle' && n.value !== 0 && n.value !== 1) fail('Toggle must be zero or one');
    if (n.kind === 'Table' && (!Number.isInteger(n.columns) || n.columns < 1 || n.columns > 16 || n.items.length % n.columns))
      fail('Table cells must form complete rows with 1..16 columns');
    if (['List', 'Combo'].includes(n.kind) && (!Number.isInteger(n.index) || n.index < -1 || n.index >= n.items.length))
      fail('Selection must be -1 or an item index');
    if (!Number.isInteger(n.severity) || n.severity < 0 || n.severity > 3) fail('Severity must be 0..3');
    const count = n.kind === 'Table' ? n.items.length / n.columns : n.items.length;
    if (![n.firstRow, n.visibleRows].every(v => Number.isInteger(v) && v >= 0) || n.visibleRows > 256 ||
      (!['List', 'Table'].includes(n.kind) && (n.firstRow || n.visibleRows)) ||
      (!n.visibleRows && n.firstRow) || (n.firstRow && n.firstRow >= count)) fail('Invalid paging range');
    if (n.password && n.kind !== 'TextInput') fail('Password is only valid on TextInput');
  }
}
export function assertBuilderSupport(d: Design, language: 'csharp' | 'lua'): void {
  validate(d);
  for (const { node: n } of flatten(d.nodes)) {
    if (!['Column', 'Row', 'Button', 'Toggle', 'Slider'].includes(n.kind) ||
      n.tooltip || n.severity || n.firstRow || n.visibleRows || n.password)
      throw new Error(`SDK 2.2.0 ${language} PanelBuilder cannot represent ${n.kind} or its 1.1 attributes. Use C++ or C.`);
  }
}
export function sampleDesigns(): Design[] {
  const basic = widget('Column', '1');
  basic.children = [widget('Button', '2'), widget('Toggle', '3'), widget('Slider', '4')];
  basic.children[0].label = 'Run diagnostics';
  basic.children[1].label = 'Telemetry';
  basic.children[2].label = 'Gain';
  const extended = structuredClone(basic);
  extended.children[0].tooltip = 'Synthetic diagnostics only';
  extended.children[0].severity = 1;
  const row = widget('Row', '5');
  row.children = [widget('Combo', '6'), widget('TextInput', '7')];
  extended.children.push(row);
  const list = widget('List', '8');
  list.visibleRows = 2;
  const table = widget('Table', '9');
  table.visibleRows = 1;
  const full = structuredClone(extended);
  full.children.push(widget('Label', '10'), list, table);
  full.children[4].label = 'Synthetic instrument panel';
  return [
    { id: 'instrument', title: 'Instrument control', nodes: [basic] },
    { id: 'telemetry', title: 'Telemetry console', nodes: [extended] },
    { id: 'workbench', title: 'Widget workbench', nodes: [full] },
  ];
}
