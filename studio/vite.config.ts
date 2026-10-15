import { defineConfig } from 'vitest/config';

// `npm run dev` proxies /ws to a scuba-hostd on 127.0.0.1:7878. Start hostd with
// --allow-origin http://localhost:5173 so it accepts the dev server's origin.
export default defineConfig({
  server: {
    proxy: {
      '/ws': { target: 'ws://127.0.0.1:7878', ws: true, changeOrigin: true },
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    chunkSizeWarningLimit: 600,
  },
  test: {
    environment: 'node',
    include: ['src/**/*.test.ts'],
  },
});
