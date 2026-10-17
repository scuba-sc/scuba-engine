import { assertBuilderSupport, isContainer, type Design } from '../model';
import { builderLines, finish, header, number, parseBuilder } from './shared';

// Lua accepts decimal byte escapes, not JSON's \uXXXX escapes.
const quoteLua = (s: string) => `"${s.replace(/["\\\u0000-\u001f]/g, c =>
  c === '"' || c === '\\' ? `\\${c}` : `\\${c.charCodeAt(0).toString().padStart(3, '0')}`)}"`;
export function generate(d: Design): string {
  assertBuilderSupport(d, 'lua');
  const body = builderLines(d, (n, _id, indent, children) => {
    if (isContainer(n)) return `${indent}panel:${n.kind.toLowerCase()}(function(panel)\n${children}\n${indent}end, { disabled = ${n.disabled} })`;
    let args = `label = ${quoteLua(n.label)}, disabled = ${n.disabled}`;
    if (n.kind === 'Toggle') args += `, value = ${n.value === 1}`;
    if (n.kind === 'Slider') args += `, value = ${number(n.value)}, minimum = ${number(n.minimum)}, maximum = ${number(n.maximum)}`;
    return `${indent}panel:${n.kind.toLowerCase()}({ ${args} })`;
  });
  return header(d, '--') + `local panel = sco.panel_builder.new()
${body}
local nodes, dispatch = panel:build()
return nodes, dispatch
`;
}
export function parse(source: string): Design {
  const jsonCompatible = source.replace(/"(?:[^"\\]|\\.)*"/g, token => {
    if (!/\\\d{3}/.test(token)) return token;
    return token.replace(/\\(?:(\d{3})|.)/g, (escape, digits: string | undefined) =>
      digits === undefined ? escape : `\\u${Number(digits).toString(16).padStart(4, '0')}`);
  });
  const d = parseBuilder(jsonCompatible, 'lua');
  return finish(d, source, generate(d));
}
