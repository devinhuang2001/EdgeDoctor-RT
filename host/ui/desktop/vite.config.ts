import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  base: '/dashboard-v2/',
  build: {
    outDir: '../../server/web-v2',
    emptyOutDir: true,
    rollupOptions: {
      output: {
        // 第三方库单独成块：既缩小首屏 chunk，也让验收脚本能区分
        // “我们自己写的代码”和 React / three 内部实现。
        manualChunks(id: string) {
          if (/node_modules[\\/](react|react-dom|scheduler)[\\/]/.test(id)) return 'vendor-react';
          if (/node_modules[\\/]three[\\/]/.test(id)) return 'vendor-three';
          return undefined;
        },
      },
    },
  },
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.ts'],
  },
});
