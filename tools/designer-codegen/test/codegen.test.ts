import { describe, expect, it } from 'bun:test';
import { flatten, sampleDesigns, validate, widget } from '../src/model';
import * as cpp from '../src/codegen/cpp';
import * as c from '../src/codegen/c';
import * as csharp from '../src/codegen/csharp';
import * as lua from '../src/codegen/lua';

describe.each([['cpp', cpp], ['c', c]] as const)('%s full widgets 1.1', (_language, generator) => {
  it.each(sampleDesigns())('snapshots and parses $id', design => {
    const source = generator.generate(design);
    expect(source).toMatchSnapshot();
    expect(generator.generate(generator.parse(source))).toBe(source);
  });
  it('roundtrips edited source values, attributes and escaped strings', () => {
    const design = sampleDesigns()[2];
    const source = generator.generate(design).replaceAll('Run diagnostics', 'Run "probe"'.replaceAll('"', '\\"'));
    const parsed = generator.parse(source);
    expect(flatten(parsed.nodes)[1].node.label).toBe('Run "probe"');
    expect(generator.generate(parsed)).toBe(source);
    for (const text of ['\\012', 'quote " and slash \\', 'line\nbreak\t', '\u0001', '\u6e29\u5ea6 \ud83d\ude80', 'literal \\u0001']) {
      design.nodes[0].children[0].label = text;
      const emitted = generator.generate(design);
      expect(flatten(generator.parse(emitted).nodes)[1].node.label).toBe(text);
      expect(generator.generate(generator.parse(emitted))).toBe(emitted);
    }
  });
  it('rejects extra statements instead of discarding them', () => {
    expect(() => generator.parse(generator.generate(sampleDesigns()[0]) + '\nunknown();')).toThrow(/syntax/);
  });
  it('preserves text bindings containing semicolons and large floating values', () => {
    const design = sampleDesigns()[2];
    const nodes = flatten(design.nodes);
    nodes[3].node.minimum = -1e30;
    nodes[3].node.maximum = 1e30;
    nodes[3].node.value = 1e25;
    nodes[6].node.text = 'a;b; "quoted"';
    const source = generator.generate(design);
    const parsed = flatten(generator.parse(source).nodes);
    expect(parsed[3].node.value).toBe(1e25);
    expect(parsed[6].node.text).toBe('a;b; "quoted"');
    expect(source).not.toContain('e+25.0');
  });
  it('preserves comment terminators and Unicode line separators as data', () => {
    const design = sampleDesigns()[0];
    design.title = 'Panel */ \u2028 title';
    design.nodes[0].children[0].label = 'Line\u2028separator';
    const source = generator.generate(design);
    const parsed = generator.parse(source);
    expect(parsed.title).toBe(design.title);
    expect(flatten(parsed.nodes)[1].node.label).toBe(design.nodes[0].children[0].label);
  });
});
describe.each([['csharp', csharp], ['lua', lua]] as const)('%s published builder subset', (_language, generator) => {
  it('snapshots and roundtrips actual supported calls', () => {
    const d = sampleDesigns()[0];
    const nested = widget('Row', '5');
    nested.disabled = true;
    nested.children = [widget('Button', '6')];
    d.nodes[0].children.push(nested);
    const source = generator.generate(d);
    expect(source).toMatchSnapshot();
    expect(generator.generate(generator.parse(source))).toBe(source);
    const changed = source.replace('Run diagnostics', 'Edited command');
    expect(flatten(generator.parse(changed).nodes)[1].node.label).toBe('Edited command');
  });
  it('rejects unsupported kinds and 1.1 attributes explicitly', () => {
    expect(() => generator.generate(sampleDesigns()[2])).toThrow(/cannot represent/);
    const d = sampleDesigns()[0];
    d.nodes[0].children[0].tooltip = 'not supported';
    expect(() => generator.generate(d)).toThrow(/cannot represent/);
  });
  it('preserves escaped text without interpreting literal escape sequences', () => {
    for (const text of ['\\012', 'literal \\u0001', 'a\u0001b', 'quotes " and \\', '\u6e29\u5ea6']) {
      const d = sampleDesigns()[0];
      d.nodes[0].children[0].label = text;
      const source = generator.generate(d);
      expect(flatten(generator.parse(source).nodes)[1].node.label).toBe(text);
    }
  });
});
describe('SDK limits and tree constraints', () => {
  it('rejects invalid range, table, parent, paging and UTF-8', () => {
    for (const mutate of [
      (n: ReturnType<typeof widget>) => { n.value = 2; },
      (n: ReturnType<typeof widget>) => { n.children = [widget('Label', 'extra')]; },
      (n: ReturnType<typeof widget>) => { n.tooltip = 'x'.repeat(256); },
      (n: ReturnType<typeof widget>) => { n.label = '\ud800'; },
      (n: ReturnType<typeof widget>) => { n.visibleRows = 1; },
    ]) {
      const d = sampleDesigns()[0];
      mutate(d.nodes[0].children[2]);
      expect(() => validate(d)).toThrow();
    }
    const d = sampleDesigns()[2];
    d.nodes[0].children.at(-1)!.columns = 3;
    expect(() => validate(d)).toThrow(/Table/);
    d.nodes[0].children.at(-1)!.columns = 2;
    d.nodes[0].children.at(-2)!.firstRow = 10;
    expect(() => validate(d)).toThrow(/paging/);
  });
});
