// SPDX-License-Identifier: GPL-3.0-only
// Private child protocol for the native designer; source generation only.
import { generate } from './src/codegen/c.ts';
import { createInterface } from 'node:readline';
for await (const line of createInterface({ input: process.stdin })) {
  try {
    if (Buffer.byteLength(line) > 1024 * 1024) throw new Error('Design exceeds 1 MiB');
    const request = JSON.parse(line);
    process.stdout.write(JSON.stringify({ source: generate(request.design) }) + '\n');
  } catch (error) {
    process.stdout.write(JSON.stringify({ error: error.message }) + '\n');
  }
}
