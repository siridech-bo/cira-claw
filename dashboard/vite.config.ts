import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

export default defineConfig({
  plugins: [vue()],
  server: {
    port: 18790,
    strictPort: true,
    proxy: {
      // Proxy runtime APIs to the C runtime on port 8080
      '/api/nodes': 'http://localhost:8080',
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
