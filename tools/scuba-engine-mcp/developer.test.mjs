// SPDX-License-Identifier: GPL-3.0-only
import { test, expect } from 'bun:test';
import { runProject, DEVELOPER_TOOLS } from './developer.mjs';
import { Server, PROTOCOL } from './server.mjs';
const p = (source, timeout_ms = 5000) => new Map([['fixture', { id: 'fixture', cwd: process.cwd(), timeout_ms,
  build: [process.execPath, '-e', source], check: [process.execPath, '-e', source], test: [process.execPath, '-e', source] }]]);

test('runs fixed command and reports bounded output and actionable failure', async () => {
  const result = await runProject(p('console.error("error: fixture");process.exit(7)'), 'build', { project: 'fixture' });
  expect(result.success).toBe(false);
  expect(result.exit_code).toBe(7);
  expect(result.diagnostics).toContain('error: fixture');
  expect(result.next).toContain('Fix');
  const large = await runProject(p('console.log("x".repeat(100000))'), 'check', { project: 'fixture' });
  expect(large.success).toBe(true);
  expect(large.output_truncated).toBe(true);
  expect(Buffer.byteLength(large.output)).toBeLessThanOrEqual(65536);
});

test('rejects unknown project and injected arguments before execution', async () => {
  await expect(runProject(p('process.exit(99)'), 'build', { project: 'missing' })).rejects.toThrow();
  await expect(runProject(p('process.exit(99)'), 'build', { project: 'fixture', args: ['injected'] })).rejects.toThrow();
});

test('timeout terminates bounded process instead of returning a pending child', async () => {
  const result = await runProject(p('while(true){}', 200), 'test', { project: 'fixture' });
  expect(result.timed_out).toBe(true);
  expect(result.success).toBe(false);
});

test('opt-in tools execute through MCP alongside the default surface', async () => {
  for (const projects of [undefined, p('console.log("ok")')]) {
    const server = new Server({}, projects);
    await server.handle({ jsonrpc: '2.0', id: 1, method: 'initialize', params: {
      protocolVersion: PROTOCOL, capabilities: {}, clientInfo: { name: 'fixture', version: '1' } } });
    await server.handle({ jsonrpc: '2.0', method: 'notifications/initialized' });
    const listed = await server.handle({ jsonrpc: '2.0', id: 2, method: 'tools/list' });
    expect(listed.result.tools.length).toBe(projects ? 11 : 8);
    if (projects) {
      for (const tool of DEVELOPER_TOOLS) {
        const result = await server.handle({ jsonrpc: '2.0', id: 3, method: 'tools/call',
          params: { name: tool.name, arguments: { project: 'fixture' } } });
        expect(result.result.isError).toBe(false);
      }
    }
  }
});
