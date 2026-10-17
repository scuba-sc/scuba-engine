import { flatten, isContainer, validate, widget, type Design, type Kind, type Widget } from '../model';

export const quoteJson = (s: string) => JSON.stringify(s).replace(/\u2028/g, '\\u2028').replace(/\u2029/g, '\\u2029');
export const quote = (s: string) => quoteJson(s).replace(/\\(?:u00([0-1][0-9a-f])|.)/g,
  (token, hex: string | undefined) => hex === undefined ? token : `\\${parseInt(hex, 16).toString(8).padStart(3, '0')}`);
export const parseString = (s: string): string => JSON.parse(s.replace(/\\(?:([0-7]{3})|.)/g,
  (token, octal: string | undefined) => octal === undefined ? token : `\\u${parseInt(octal, 8).toString(16).padStart(4, '0')}`)) as string;
export const number = (n: number) => /[.eE]/.test(String(n)) ? String(n) : `${n}.0`;
export const kindConstant = (kind: Kind) => `SCO_WIDGET_${kind === 'TextInput' ? 'TEXT_INPUT' : kind.toUpperCase()}`;
export function header(d: Design, prefix = '//'): string {
  const identity = JSON.stringify({ id: d.id, title: d.title }).replace(/\u2028/g, '\\u2028').replace(/\u2029/g, '\\u2029');
  return `${prefix} scuba-panel ${identity}\n`;
}
export function readHeader(source: string): Design {
  const match = source.match(/^(?:\/\/|--|\/\*) scuba-panel (.+?)(?: \*\/)?\r?$/m);
  if (!match) throw new Error('Missing scuba-panel identity header');
  const value: unknown = JSON.parse(match[1]);
  if (!value || typeof value !== 'object' || !('id' in value) || !('title' in value) ||
    typeof value.id !== 'string' || typeof value.title !== 'string') throw new Error('Invalid panel identity');
  return { id: value.id, title: value.title, nodes: [] };
}
export function strings(source: string): string[] {
  return [...source.matchAll(/"(?:[^"\\]|\\.)*"/g)].map(m => parseString(m[0]));
}
export function splitArgs(source: string): string[] {
  const result: string[] = [];
  let start = 0, depth = 0, inString = false, escape = false;
  for (let i = 0; i < source.length; i++) {
    const c = source[i];
    if (inString) {
      if (escape) escape = false;
      else if (c === '\\') escape = true;
      else if (c === '"') inString = false;
    } else if (c === '"') inString = true;
    else if ('{(['.includes(c)) depth++;
    else if ('})]'.includes(c)) depth--;
    else if (c === ',' && depth === 0) { result.push(source.slice(start, i).trim()); start = i + 1; }
  }
  result.push(source.slice(start).trim());
  return result;
}
export function canonical(source: string): string {
  return source.replace(/"(?:[^"\\]|\\.)*"|\s+/g, token => token.startsWith('"') ? token : '');
}
export function finish(d: Design, source: string, generated: string): Design {
  validate(d);
  if (canonical(source) !== canonical(generated))
    throw new Error('Only the generated declarative syntax is supported; no statements were discarded');
  return d;
}
export function parseBuilder(source: string, language: 'cpp' | 'csharp' | 'lua'): Design {
  const d = readHeader(source);
  const stack: Widget[][] = [d.nodes];
  const bindings = new Map<string, string>();
  for (const m of source.matchAll(/static (?:bool|double|int|std::string) (v\d+) = ("(?:[^"\\]|\\.)*"|[^;]+);/g))
    bindings.set(m[1], m[2]);
  let last: Widget | undefined;
  const append = (kind: Kind) => {
    const n = widget(kind, String(flatten(d.nodes).length + 1));
    stack[stack.length - 1].push(n);
    last = n;
    return n;
  };
  for (const raw of source.split('\n')) {
    const line = raw.trim();
    const container = language === 'lua' ? line.match(/^panel:(column|row)\(function\(panel\)$/) :
      line.match(/^panel\.(Column|Row)\((?:\[&\]|[0-9]+u?, scope[0-9]+ =>) ?\{$/);
    if (container) {
      const n = append((container[1][0].toUpperCase() + container[1].slice(1)) as Kind);
      stack.push(n.children);
      continue;
    }
    const end = language === 'lua' ? line.match(/^end, \{ disabled = (true|false) \}\)$/) :
      language === 'csharp' ? line.match(/^\}, disabled: (true|false)\);$/) : line.match(/^\}\)(.*);$/);
    if (end && stack.length > 1) {
      stack.pop();
      last = stack[stack.length - 1].at(-1);
      if (last) {
        if (language === 'cpp') attributes(last, end[1]);
        else last.disabled = end[1] === 'true';
      }
      continue;
    }
    if (language === 'lua') {
      const call = line.match(/^panel:(button|toggle|slider)\(\{ (.+) \}\)$/s);
      if (!call) continue;
      const n = append((call[1][0].toUpperCase() + call[1].slice(1)) as Kind);
      const a = Object.fromEntries(splitArgs(call[2]).map(s => {
        const at = s.indexOf(' = ');
        return [s.slice(0, at), s.slice(at + 3)];
      }));
      n.label = JSON.parse(a.label) as string;
      n.disabled = a.disabled === 'true';
      if (n.kind === 'Toggle') n.value = a.value === 'true' ? 1 : 0;
      if (n.kind === 'Slider') { n.value = Number(a.value); n.minimum = Number(a.minimum); n.maximum = Number(a.maximum); }
      continue;
    }
    const call = builderCall(line);
    if (!call) continue;
    const n = append(call[1] as Kind);
    let a = splitArgs(call[2]);
    if (language === 'csharp') a = a.slice(1);
    n.label = parseString(a[0]);
    const bound = (s: string) => {
      const v = bindings.get(s.replace(/^&/, ''));
      if (v === undefined) throw new Error('Unknown generated binding');
      return v;
    };
    if (n.kind === 'Toggle') n.value = (language === 'cpp' ? bound(a[1]) : a[1]) === 'true' ? 1 : 0;
    if (n.kind === 'Slider') {
      n.value = Number(language === 'cpp' ? bound(a[1]) : a[1]);
      n.minimum = Number(a[2]); n.maximum = Number(a[3]);
    }
    if (['Combo', 'List'].includes(n.kind)) { n.index = Number(bound(a[1])); n.items = strings(a[2]); }
    if (n.kind === 'TextInput') n.text = parseString(bound(a[1]));
    if (n.kind === 'Table') { n.columns = Number(a[1].replace(/u$/, '')); n.items = strings(a[2]); }
    if (language === 'cpp') attributes(n, call[3]);
    else n.disabled = a.at(-1) === 'disabled: true';
  }
  if (stack.length !== 1) throw new Error('Unclosed layout');
  return d;
}
function builderCall(line: string): string[] | null {
  const match = line.match(/^panel\.(Label|Button|Toggle|Slider|Combo|List|TextInput|Table)\(/);
  if (!match) return null;
  let depth = 1, quoted = false, escape = false;
  for (let i = match[0].length; i < line.length; i++) {
    const c = line[i];
    if (quoted) {
      if (escape) escape = false;
      else if (c === '\\') escape = true;
      else if (c === '"') quoted = false;
    } else if (c === '"') quoted = true;
    else if (c === '(') depth++;
    else if (c === ')' && --depth === 0) return [line, match[1], line.slice(match[0].length, i), line.slice(i + 1, -1)];
  }
  throw new Error('Unclosed builder call');
}
function attributes(n: Widget, suffix: string): void {
  n.disabled = suffix.includes('.Disabled()');
  n.password = suffix.includes('.Password()');
  const tooltip = suffix.match(/\.Tooltip\(("(?:[^"\\]|\\.)*")\)/);
  if (tooltip) n.tooltip = parseString(tooltip[1]);
  const severity = suffix.match(/\.Severity\((\d+)u\)/);
  if (severity) n.severity = Number(severity[1]);
  const paging = suffix.match(/\.Paging\((\d+)u, (\d+)u\)/);
  if (paging) { n.firstRow = Number(paging[1]); n.visibleRows = Number(paging[2]); }
}
export function nativePlugin(d: Design, language: 'c' | 'cpp', panel: string): string {
  const id = quote(`${d.id}.main`), title = quote(d.title);
  const preamble = `#include "sco_api.h"\n#include "sco_widgets.h"\n#include <stddef.h>\n`;
  if (language === 'cpp') return `${preamble}#include "scosdk/scosdk.hpp"
#include "scosdk/menu_builder.hpp"
${panel}
namespace {
class DesignedPlugin final : public sco::sdk::Plugin {
public:
    sco_result OnLoad() override {
        if (widgets_.Open(*this) != SCO_OK) return SCO_OK;
        auto next = BuildDesignedPanel();
        if (!next) return next.Result();
        const auto result = next.Submit(widgets_, ${id}, ${title});
        if (result == SCO_OK) tree_ = std::move(next);
        return result;
    }
    void OnUnload() override {
        if (widgets_) (void)widgets_.RemovePanel(${id});
    }
private:
    sco::sdk::Widgets widgets_;
    scosdk::PanelTree tree_;
};
}
SCO_PLUGIN(DesignedPlugin, ${quote(d.id)}, "0.1.0", "the maintainer");
`;
  return `${preamble}${panel}
static const sco_widgets_v1* designed_widgets;
static sco_plugin* designed_self;
static const sco_plugin_info designed_info = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, ${quote(d.id)}, "0.1.0", "the maintainer"
};
SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) { return &designed_info; }
SCO_EXPORT sco_result sco_plugin_load(const sco_api* api, sco_plugin* self) {
    const void* table = NULL;
    designed_self = self;
    if (api->size < offsetof(sco_api, query_service) + sizeof(api->query_service) || !api->query_service)
        return SCO_OK;
    if (api->query_service(SCO_WIDGETS_NAME, SCO_WIDGETS_VERSION_1_0, &table) != SCO_OK || !table)
        return SCO_OK;
    designed_widgets = (const sco_widgets_v1*)table;
    if (designed_widgets->size < offsetof(sco_widgets_v1, remove_panel) + sizeof(designed_widgets->remove_panel) ||
        !designed_widgets->set_panel || !designed_widgets->remove_panel) {
        designed_widgets = NULL;
        return SCO_OK;
    }
    return designed_widgets->set_panel(self, ${id}, ${title}, &designed_nodes[0].base,
        ${flatten(d.nodes).length}u, sizeof(designed_nodes[0]), NULL, NULL);
}
SCO_EXPORT void sco_plugin_unload(void) {
    if (designed_widgets) (void)designed_widgets->remove_panel(designed_self, ${id});
    designed_widgets = NULL;
}
`;
}
export function builderLines(d: Design, render: (n: Widget, id: number, indent: string, children: string) => string): string {
  let id = 0;
  const visit = (nodes: Widget[], depth: number): string => nodes.map(n => {
    const current = ++id;
    return render(n, current, '    '.repeat(depth), isContainer(n) ? visit(n.children, depth + 1) : '');
  }).join('\n');
  return visit(d.nodes, 1);
}
