import { assertBuilderSupport, isContainer, type Design } from '../model';
import { builderLines, finish, header, number, parseBuilder, quoteJson } from './shared';

export function generate(d: Design): string {
  assertBuilderSupport(d, 'csharp');
  const body = builderLines(d, (n, id, indent, children) => {
    if (isContainer(n)) return `${indent}panel.${n.kind}(${id}u, scope${id} => {\n${children}\n${indent}}, disabled: ${n.disabled});`;
    let args = `${id}u, ${quoteJson(n.label)}`;
    if (n.kind === 'Toggle') args += `, ${n.value === 1}`;
    if (n.kind === 'Slider') args += `, ${number(n.value)}, ${number(n.minimum)}, ${number(n.maximum)}`;
    return `${indent}panel.${n.kind}(${args}, disabled: ${n.disabled});`;
  });
  return header(d) + `using Sco.Sdk;

public static class DesignedPanel {
    public static (WidgetNode[] Nodes, PanelBuilder Builder) Build() {
    var panel = new PanelBuilder();
${body}
    return (panel.Build(), panel);
    }
}
`;
}
export function parse(source: string): Design {
  const d = parseBuilder(source, 'csharp');
  return finish(d, source, generate(d));
}
