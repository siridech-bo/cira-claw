import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import path from 'path';

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      // Import shared types from gateway services (socket-registry, etc.)
      '@gateway': path.resolve(__dirname, '../src/services'),
    },
  },
  server: {
    port: 3000,
    strictPort: false,
    proxy: {
      // Proxy node management APIs to gateway (18790) which handles JSON fixing
      '/api/nodes': 'http://localhost:18790',
      '/api/rules': 'http://localhost:18790',
      '/api/composite-rules': 'http://localhost:18790',
      '/api/status': 'http://localhost:18790',
      '/health': 'http://localhost:18790',
      // Proxy chat websocket to gateway
      '/chat': {
        target: 'ws://localhost:18790',
        ws: true,
      },
      // Proxy direct runtime APIs to C++ runtime on port 8080
      '/api/cameras': 'http://localhost:8080',
      '/api/files': 'http://localhost:8080',
      '/api/model': 'http://localhost:8080',
      '/api/models': 'http://localhost:8080',
      '/api/stats': 'http://localhost:8080',
      '/api/results': 'http://localhost:8080',
      '/api/inference': 'http://localhost:8080',
      '/api/camera': 'http://localhost:8080',
      '/snapshot': 'http://localhost:8080',
      '/stream': 'http://localhost:8080',
      '/frame': 'http://localhost:8080',
      '/ws': {
        target: 'ws://localhost:8080',
        ws: true,
      },
    },
  },
  build: {
    outDir: 'dist',
    sourcemap: false,
  },
});
