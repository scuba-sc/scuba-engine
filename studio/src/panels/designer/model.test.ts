import { describe, expect, it } from 'vitest';
import { DesignIndex, flatten, sampleDesigns, widget } from './model';

describe('DesignIndex', () => {
  it('indexes nodes by key, node reference, and id correctly', () => {
    const design = sampleDesigns()[2]; // full design
    const index = new DesignIndex(design.nodes);
    const rows = flatten(design.nodes);

    expect(index.rows.length).toBe(rows.length);

    for (const row of rows) {
      expect(index.byKey.get(row.node.key)).toEqual(row);
      expect(index.byNode.get(row.node)).toEqual(row);
      expect(index.byId.get(row.id)).toEqual(row);
    }
  });

  it('returns correct siblings for root and nested nodes', () => {
    const root1 = widget('Column', '1');
    const child1 = widget('Button', '2');
    const child2 = widget('Toggle', '3');
    root1.children = [child1, child2];

    const root2 = widget('Row', '4');

    const nodes = [root1, root2];
    const index = new DesignIndex(nodes);

    const rootRow = index.byKey.get('1')!;
    const childRow = index.byKey.get('2')!;

    expect(index.getSiblings(rootRow, nodes)).toEqual(nodes);
    expect(index.getSiblings(childRow, nodes)).toEqual([child1, child2]);
  });
});
