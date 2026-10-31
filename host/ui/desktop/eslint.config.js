import js from '@eslint/js';
import globals from 'globals';
import reactHooks from 'eslint-plugin-react-hooks';
import tseslint from 'typescript-eslint';

export default tseslint.config(
  { ignores: ['node_modules*/**', 'scripts/**', 'eslint-report.json', '*.tsbuildinfo'] },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    files: ['src/**/*.{ts,tsx}'],
    languageOptions: {
      ecmaVersion: 2022,
      globals: { ...globals.browser },
    },
    plugins: { 'react-hooks': reactHooks },
    rules: {
      ...reactHooks.configs.recommended.rules,
      '@typescript-eslint/no-explicit-any': 'error',
      '@typescript-eslint/no-unused-vars': ['error', { argsIgnorePattern: '^_' }],
      'no-console': 'error',
      // 规格 §2 / DD-UI-07：不得用随机数或示例诊断结论填充无数据状态。
      'no-restricted-properties': ['error', {
        object: 'Math',
        property: 'random',
        message: '禁用 Math.random()：无数据状态必须显示“不可用”，不得生成模拟图形。',
      }],
      'no-restricted-syntax': ['error', {
        selector: "Literal[value=/42\\.3|96\\.8|轴承异常|Bearing Anomaly/]",
        message: '禁用示例频谱/健康度/故障结论常量，这些值必须来自服务端。',
      }, {
        selector: 'JSXAttribute[name.name="dangerouslySetInnerHTML"]',
        message: '禁止使用 dangerouslySetInnerHTML：渲染文本一律走 DOM 文本节点。',
      }],
    },
  },
);
