// 验收报告生成器：把 Playwright 结果映射回规格 02 的用例表，
// 产出 manifest.json、case-results.json 与 checksums.txt。

import { execSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import type { FullResult, Reporter, TestCase, TestResult } from '@playwright/test/reporter';
import { ACCEPTANCE_CASES, CASE_INDEX } from './acceptance-cases';

const repoRoot = process.cwd();
const evidenceDir = process.env.DD_EVIDENCE_DIR ?? path.join(repoRoot, 'benchmark', 'results', 'desktop-dashboard', 'local');

type Status = 'PASS' | 'FAIL' | 'MISSING_EVIDENCE' | 'OUT_OF_SCOPE' | 'NOT_RUN';

type CaseResult = {
  id: string;
  layer: string;
  precondition: string;
  input: string;
  expected: string;
  observed: string;
  status: Status;
  evidence: string[];
};

function git(args: string): string {
  try {
    // stdio 显式忽略 stdin：默认的 stdin 管道在这台验收机上会以 EBUSY 失败，
    // 而这里的 try/catch 会把失败吞成空字符串，manifest 里的 git 信息就悄悄消失了。
    return execSync(`git ${args}`, { cwd: repoRoot, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }).trim();
  } catch {
    return '';
  }
}

function packageVersion(file: string, key: string): string {
  try {
    const payload = JSON.parse(fs.readFileSync(path.join(repoRoot, file), 'utf8'));
    return payload.dependencies?.[key] ?? payload.devDependencies?.[key] ?? '';
  } catch {
    return '';
  }
}

function collectChecksums(dir: string): string {
  const lines: string[] = [];
  const walk = (current: string) => {
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const full = path.join(current, entry.name);
      if (entry.isDirectory()) {
        walk(full);
        continue;
      }
      if (path.basename(full) === 'checksums.txt') continue;
      const digest = createHash('sha256').update(fs.readFileSync(full)).digest('hex');
      lines.push(`${digest}  ${path.relative(evidenceDir, full).replace(/\\/g, '/')}`);
    }
  };
  if (fs.existsSync(dir)) walk(dir);
  return `${lines.sort().join('\n')}\n`;
}

class AcceptanceReporter implements Reporter {
  private results = new Map<string, CaseResult>();
  private unmapped: string[] = [];
  private startedAt = new Date().toISOString();

  onBegin(): void {
    fs.mkdirSync(evidenceDir, { recursive: true });
    for (const item of ACCEPTANCE_CASES) {
      this.results.set(item.id, {
        id: item.id,
        layer: item.layer,
        precondition: item.precondition,
        input: item.input,
        expected: item.expected,
        observed: '',
        status: 'NOT_RUN',
        evidence: [],
      });
    }
  }

  onTestEnd(test: TestCase, result: TestResult): void {
    const ids = test.title.match(/DD-[A-Z0-9]+-\d+/g) ?? [];
    const missing = result.annotations.filter((annotation) => annotation.type === 'missing-evidence');
    const outOfScope = result.annotations.filter((annotation) => annotation.type === 'out-of-scope');
    const evidence = result.attachments
      .filter((attachment) => attachment.path && attachment.name !== 'evidence-dir')
      .map((attachment) => path.relative(evidenceDir, attachment.path as string).replace(/\\/g, '/'));
    const evidenceDirAttachment = result.attachments.find((attachment) => attachment.name === 'evidence-dir');
    const observed = result.error?.message?.split('\n').slice(0, 4).join(' ') ?? '';

    if (ids.length === 0) {
      this.unmapped.push(`${test.title} → ${result.status}`);
      return;
    }

    for (const id of ids) {
      const entry = this.results.get(id);
      if (!entry) {
        this.unmapped.push(`${id} 不在验收用例表中`);
        continue;
      }
      const status: Status = result.status !== 'passed' && result.status !== 'skipped' ? 'FAIL'
        : missing.length > 0 ? 'MISSING_EVIDENCE'
          : outOfScope.length > 0 ? 'OUT_OF_SCOPE'
            : result.status === 'skipped' ? 'MISSING_EVIDENCE'
              : 'PASS';
      entry.status = status;
      entry.observed = missing.length > 0
        ? `未取得证据：${missing.map((annotation) => annotation.description ?? '').join('；')}`
        : outOfScope.length > 0
          ? `超出本次范围：${outOfScope.map((annotation) => annotation.description ?? '').join('；')}`
          : status === 'PASS' ? '自动化用例通过，见证据文件' : observed;
      entry.evidence = evidence;
      entry.layer = CASE_INDEX.get(id)?.layer ?? entry.layer;
      if (evidenceDirAttachment?.body) {
        entry.evidence.unshift(String(evidenceDirAttachment.body));
      }
    }

    // 增量落盘：每跑完一个用例就把用例表写一次。
    // 只在 onEnd 写是不安全的——Playwright 若在收尾阶段卡住（例如它内部用
    // spawnSync(taskkill) 结束 webServer 失败而一直 await），onEnd 永远不执行，
    // 整轮结果就全部丢失。增量写让证据在任何中断下都还在。
    this.flushCases();
  }

  private flushCases(): void {
    try {
      fs.mkdirSync(evidenceDir, { recursive: true });
      fs.writeFileSync(
        path.join(evidenceDir, 'case-results.json'),
        `${JSON.stringify([...this.results.values()], null, 2)}\n`,
        'utf8',
      );
    } catch {
      // 落盘失败不应改变测试结论；onEnd 还会再写一次完整版。
    }
  }

  onEnd(result: FullResult): void {
    const cases = [...this.results.values()];
    const summary = {
      PASS: cases.filter((item) => item.status === 'PASS').length,
      FAIL: cases.filter((item) => item.status === 'FAIL').length,
      MISSING_EVIDENCE: cases.filter((item) => item.status === 'MISSING_EVIDENCE').length,
      OUT_OF_SCOPE: cases.filter((item) => item.status === 'OUT_OF_SCOPE').length,
      NOT_RUN: cases.filter((item) => item.status === 'NOT_RUN').length,
    };

    const manifest = {
      run_id: process.env.DD_RUN_ID ?? 'local',
      project: 'EdgeDoctor-RT desktop dashboard (spec 01 v0.1)',
      started_at: this.startedAt,
      finished_at: new Date().toISOString(),
      playwright_status: result.status,
      git: {
        head: git('rev-parse HEAD'),
        branch: git('rev-parse --abbrev-ref HEAD'),
        dirty_diff_hash: git('diff HEAD').length
          ? createHash('sha256').update(git('diff HEAD')).digest('hex').slice(0, 16)
          : 'clean',
      },
      versions: {
        node: process.version,
        browser_channel: process.env.DD_BROWSER ?? 'msedge',
        playwright: packageVersion('package.json', '@playwright/test'),
        react: packageVersion('ui/desktop/package.json', 'react'),
        three: packageVersion('ui/desktop/package.json', 'three'),
        vite: packageVersion('ui/desktop/package.json', 'vite'),
        python: (() => { try { return execSync('python --version', { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }).trim(); } catch { return ''; } })(),
      },
      assets: (() => {
        try {
          return JSON.parse(fs.readFileSync(path.join(repoRoot, 'ui', 'desktop', 'assets.lock.json'), 'utf8')).assets;
        } catch {
          return [];
        }
      })(),
      bindings: (() => {
        try {
          const payload = JSON.parse(fs.readFileSync(path.join(repoRoot, 'server', 'asset-bindings.json'), 'utf8'));
          return {
            schema_version: payload.schema_version,
            frozen_at: payload.frozen_at,
            assets: payload.assets.length,
            zones: payload.zones?.length ?? 0,
          };
        } catch {
          return null;
        }
      })(),
      server: {
        base_url: process.env.DD_BASE_URL,
        database: 'tmp/ui-acceptance/acceptance.db（隔离库）',
        bindings_env: 'server/asset-bindings.json（构建期副本）',
      },
      summary,
      cases,
      unmapped_tests: this.unmapped,
      notes: [
        'DD-3D-06（GPU 首屏时间）与 DD-PERF-01 只记录实测值，不因通过测试就提升为硬件验收。',
        'LAN 走查需要真实设备、真实 MQTT 链路与现场网络，本报告不含该证据。',
        'MISSING_EVIDENCE 表示该条在当前环境无法取得证据，不得视为通过。',
      ],
    };

    fs.mkdirSync(evidenceDir, { recursive: true });
    fs.writeFileSync(path.join(evidenceDir, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`, 'utf8');
    fs.writeFileSync(path.join(evidenceDir, 'case-results.json'), `${JSON.stringify(cases, null, 2)}\n`, 'utf8');
    fs.writeFileSync(path.join(evidenceDir, 'checksums.txt'), collectChecksums(evidenceDir), 'utf8');

    // eslint-disable-next-line no-console
    console.log(`[acceptance] 证据目录：${path.relative(repoRoot, evidenceDir).replace(/\\/g, '/')}`);
    // eslint-disable-next-line no-console
    console.log(`[acceptance] PASS ${summary.PASS} / FAIL ${summary.FAIL} / MISSING_EVIDENCE ${summary.MISSING_EVIDENCE} / NOT_RUN ${summary.NOT_RUN}`);
  }
}

export default AcceptanceReporter;
