// SPDX-License-Identifier: GPL-3.0-only
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { readFile } from 'node:fs/promises';
import { isAbsolute } from 'node:path';

export const DEVELOPER_TOOLS = ['build', 'check', 'test'].map(action => ({
  name: 'project_' + action,
  description: `Run the operator-configured local ${action}; no shell, supplied command or extra arguments.`,
  inputSchema: { type: 'object', properties: { project: { type: 'string', minLength: 1, maxLength: 64 } },
    required: ['project'], additionalProperties: false },
  annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
}));

export async function loadProjects(path) {
  const config = JSON.parse(await readFile(path, 'utf8'));
  if (!config || config.version !== 1 || !Array.isArray(config.projects) || config.projects.length > 16)
    throw new Error('Expected version 1 and at most 16 configured projects');
  const projects = new Map();
  for (const p of config.projects) {
    if (!p || !/^[a-z0-9_-]{1,64}$/.test(p.id) || projects.has(p.id) || !isAbsolute(p.cwd) ||
        !Number.isInteger(p.timeout_ms) || p.timeout_ms < 100 || p.timeout_ms > 1800000)
      throw new Error('Invalid project identity, absolute cwd or bounded timeout');
    for (const action of ['build', 'check', 'test']) {
      const cmd = p[action];
      if (!Array.isArray(cmd) || !cmd.length || cmd.length > 64 || !isAbsolute(cmd[0]) ||
          cmd.some(v => typeof v !== 'string' || v.includes('\0') || v.length > 4096))
        throw new Error('Each project action must be an absolute executable and fixed argument array');
    }
    projects.set(p.id, p);
  }
  return projects;
}

export async function runProject(projects, action, args) {
  if (!args || Object.keys(args).length !== 1 || typeof args.project !== 'string' ||
      !projects.has(args.project) || !['build', 'check', 'test'].includes(action))
    throw new Error('Unknown configured project or invalid arguments');
  const p = projects.get(args.project);
  const child = spawn(p[action][0], p[action].slice(1), { cwd: p.cwd, shell: false,
    windowsHide: true, detached: process.platform !== 'win32', stdio: ['ignore', 'pipe', 'pipe'] });
  let captured = Buffer.alloc(0), bytes = 0, truncated = false, timedOut = false;
  const capture = chunk => {
    bytes += chunk.length;
    const room = 65536 - captured.length;
    if (room > 0) captured = Buffer.concat([captured, chunk.subarray(0, room)]);
    if (bytes > 65536) truncated = true;
  };
  child.stdout.on('data', capture); child.stderr.on('data', capture);
  const closed = once(child, 'close');
  let killed;
  const timer = setTimeout(() => {
    timedOut = true;
    if (process.platform === 'win32') {
      const kill = spawn('C:/Windows/System32/taskkill.exe', ['/PID', String(child.pid), '/T', '/F'],
        { windowsHide: true, stdio: 'ignore' });
      killed = once(kill, 'close');
    } else process.kill(-child.pid, 'SIGKILL');
  }, p.timeout_ms);
  try {
    const [exitCode, signal] = await closed;
    if (killed) await killed;
    const output = new TextDecoder('utf-8', { fatal: false }).decode(captured, { stream: truncated });
    return { project: p.id, action, exit_code: exitCode, signal, timed_out: timedOut,
      output, output_truncated: truncated, success: exitCode === 0 && !timedOut,
      diagnostics: output.split(/\r?\n/).filter(l => /error|warning|failed|failure/i.test(l)).slice(0, 40),
      next: timedOut ? 'Inspect the configured timeout and local build logs; no retry was attempted.' :
        exitCode === 0 ? 'Completed locally; this does not certify game behavior.' : 'Fix the reported local diagnostics before retrying.' };
  } finally { clearTimeout(timer); }
}
