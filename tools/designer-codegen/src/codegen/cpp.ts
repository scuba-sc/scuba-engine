import { flatten, isContainer, validate, type Design, type Widget } from '../model';
import { builderLines, finish, header, nativePlugin, number, parseBuilder, quote } from './shared';

function tail(n: Widget): string {
  return (n.disabled ? '.Disabled()' : '') + (n.password ? '.Password()' : '') +
    (n.tooltip ? `.Tooltip(${quote(n.tooltip)})` : '') + (n.severity ? `.Severity(${n.severity}u)` : '') +
    (n.visibleRows ? `.Paging(${n.firstRow}u, ${n.visibleRows}u)` : '');
}
export function generate(d: Design): string {
  validate(d);
  const bindings = flatten(d.nodes).map(({ node: n, id }) => {
    if (n.kind === 'Toggle') return `static bool v${id} = ${n.value === 1};`;
    if (n.kind === 'Slider') return `static double v${id} = ${number(n.value)};`;
    if (n.kind === 'Combo' || n.kind === 'List') return `static int v${id} = ${n.index};`;
    if (n.kind === 'TextInput') return `static std::string v${id} = ${quote(n.text)};`;
    return '';
  }).filter(Boolean).join('\n');
  const body = builderLines(d, (n, id, indent, children) => {
    if (isContainer(n)) return `${indent}panel.${n.kind}([&] {\n${children}\n${indent}})${tail(n)};`;
    let args = quote(n.label);
    if (n.kind === 'Toggle' || n.kind === 'TextInput') args += `, &v${id}`;
    if (n.kind === 'Slider') args += `, &v${id}, ${number(n.minimum)}, ${number(n.maximum)}`;
    if (n.kind === 'Combo' || n.kind === 'List') args += `, &v${id}, {${n.items.map(quote).join(', ')}}`;
    if (n.kind === 'Table') args += `, ${n.columns}u, {${n.items.map(quote).join(', ')}}`;
    return `${indent}panel.${n.kind}(${args})${tail(n)};`;
  });
  return header(d) + nativePlugin(d, 'cpp', `${bindings}
static scosdk::PanelTree BuildDesignedPanel() {
    scosdk::PanelBuilder panel;
${body}
    return panel.Build();
}
`);
}
export function parse(source: string): Design {
  const d = parseBuilder(source, 'cpp');
  return finish(d, source, generate(d));
}
