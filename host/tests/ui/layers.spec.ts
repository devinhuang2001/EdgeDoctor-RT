// UNIT 与 API 层的验收项：在验收目录里留下可复核的原始输出。
//
// 子进程一律走 spawn.ts：不经 shell、瞬时资源错误重试、失败时保留原因。
// vitest / eslint 用本机 node 直接跑入口文件，避免 npx 的 .cmd 间接层。

import fs from 'node:fs';
import path from 'node:path';
import { expect, test } from './evidence';
import { nodeBin, pythonBin, run } from './spawn';

const repoRoot = process.cwd();
const evidenceRoot = process.env.DD_EVIDENCE_DIR ?? path.join(repoRoot, 'benchmark', 'results', 'desktop-dashboard', 'local');
const desktop = path.join(repoRoot, 'ui', 'desktop');

function pytest(selection: string) {
  return run(
    pythonBin,
    ['-m', 'pytest', 'tests/server/test_dashboard_api.py', '-q', '-k', selection],
    repoRoot,
    { PYTHONPATH: repoRoot },
  );
}

test('DD-API-01 空库、仅遥测、活动事件与恢复未确认下的状态语义', async ({ page, evidence }) => {
  const run1 = pytest('factory_summary or empty_database');
  fs.writeFileSync(path.join(evidenceRoot, 'api.log'), run1.output, 'utf8');
  evidence.writeText('api-summary-tests.log', run1.output);
  expect(run1.status, `API 汇总语义测试失败：\n${run1.output}`).toBe(0);
  expect(run1.output).toMatch(/\d+ passed/);
  void page;
});

test('DD-API-02 乱序 revision、缺字段、非法数值与未知枚举被拒绝', async ({ page, evidence }) => {
  const result = pytest('schema or telemetry_rejects or event_detail');
  fs.appendFileSync(path.join(evidenceRoot, 'api.log'), `\n${result.output}`, 'utf8');
  evidence.writeText('api-schema-tests.log', result.output);
  expect(result.status, `schema 校验测试失败：\n${result.output}`).toBe(0);
  void page;
});

test('DD-API-07 在线状态严格符合冻结阈值且无绑定资产不计入在线', async ({ page, evidence }) => {
  const result = pytest('online or stale or unverified');
  fs.appendFileSync(path.join(evidenceRoot, 'api.log'), `\n${result.output}`, 'utf8');
  evidence.writeText('api-threshold-tests.log', result.output);
  expect(result.status, `阈值语义测试失败：\n${result.output}`).toBe(0);

  // 运行中的服务也要复核一次：6 项绑定、只有 motor-01 为已验证
  const summary = await page.request.get('/api/v1/factory/summary');
  const body = (await summary.json()) as { assets: { asset_id: string; binding_state: string; status: string; device_id: string | null }[]; counts: Record<string, number>; online_threshold_ms: number };
  expect(body.online_threshold_ms).toBe(30_000);
  expect(body.assets).toHaveLength(6);
  const verified = body.assets.filter((asset) => asset.binding_state === 'verified');
  expect(verified.map((asset) => asset.asset_id)).toEqual(['motor-01']);
  const unbound = body.assets.filter((asset) => asset.binding_state !== 'verified');
  unbound.forEach((asset) => expect(asset.status, `${asset.asset_id} 不是已验证绑定，必须为 UNKNOWN`).toBe('UNKNOWN'));
  expect(body.counts.ONLINE).toBeLessThanOrEqual(verified.length);
  evidence.writeJson('factory-summary.json', body);
});

test('DD-UI-03 趋势缺样、单点、恒定值与超长序列不产生 NaN', async ({ evidence }) => {
  const result = run(nodeBin, [path.join('node_modules', 'vitest', 'vitest.mjs'), 'run', 'src/lib/trend.test.ts'], desktop);
  evidence.writeText('unit-trend.log', result.output);
  expect(result.status, `趋势单元测试失败：\n${result.output}`).toBe(0);
  expect(result.output).toMatch(/trend\.test\.ts/);
});

test('DD-UI-06 频谱横轴不超过奈奎斯特频率', async ({ evidence }) => {
  const result = run(nodeBin, [path.join('node_modules', 'vitest', 'vitest.mjs'), 'run', 'src/lib/spectrum.test.ts'], desktop);
  evidence.writeText('unit-spectrum.log', result.output);
  expect(result.status, `频谱规则测试失败：\n${result.output}`).toBe(0);

  // 运行中的构建产物也不得包含 104 Hz 以上的实测结论。
  // vendor-*.js 是 React / three 的第三方实现，单独成块后不参与本次扫描。
  const bundle = path.join(repoRoot, 'server', 'web-v2');
  const offenders: string[] = [];
  const walk = (dir: string) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) { walk(full); continue; }
      if (!/\.(js|css|html)$/.test(entry.name)) continue;
      if (/^vendor-/.test(entry.name)) continue;
      const text = fs.readFileSync(full, 'utf8');
      for (const pattern of ['42.3', '96.8', '轴承异常', 'Math.random']) {
        if (text.includes(pattern)) offenders.push(`${entry.name}:${pattern}`);
      }
    }
  };
  walk(bundle);
  evidence.writeJson('bundle-scan.json', { offenders });
  expect(offenders, '构建产物含示例诊断内容').toHaveLength(0);
});

test('DD-UI-07 源码与产物中没有随机图形或样例诊断兜底', async ({ evidence }) => {
  const patterns = [/Math\.random/, /42\.3/, /96\.8/, /轴承异常/, /dangerouslySetInnerHTML/];
  const offenders: string[] = [];
  const walk = (dir: string, skipVendor: boolean) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      if (entry.name === 'node_modules') continue;
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) { walk(full, skipVendor); continue; }
      if (!/\.(ts|tsx|js|jsx|css|html)$/.test(entry.name)) continue;
      if (/\.test\.(ts|tsx)$/.test(entry.name)) continue;
      if (skipVendor && /^vendor-/.test(entry.name)) continue;
      const text = fs.readFileSync(full, 'utf8');
      for (const pattern of patterns) {
        if (pattern.test(text)) offenders.push(`${path.relative(repoRoot, full)} :: ${pattern}`);
      }
    }
  };
  // 源码严格扫描；构建产物跳过第三方 vendor 块。
  walk(path.join(repoRoot, 'ui', 'desktop', 'src'), false);
  walk(path.join(repoRoot, 'server', 'web-v2'), true);

  const lint = run(nodeBin, [path.join('node_modules', 'eslint', 'bin', 'eslint.js'), '.'], desktop);
  evidence.writeText('lint.log', lint.output);
  evidence.writeJson('forbidden-patterns.json', { offenders });
  expect(lint.status, `eslint 未通过：\n${lint.output}`).toBe(0);
  expect(offenders, `出现禁止的样例内容：${offenders.join(' | ')}`).toHaveLength(0);
});
