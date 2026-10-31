// 验收证据收集：控制台日志、网络 HAR、失败请求、截图。
// 规格 02 §1 要求每条结果都能指回原始证据，因此这里的文件都会作为测试附件登记。

import fs from 'node:fs';
import path from 'node:path';
import { test as base, expect } from '@playwright/test';
import type { Page, TestInfo } from '@playwright/test';

export const evidenceRoot = process.env.DD_EVIDENCE_DIR ?? path.join(process.cwd(), 'benchmark', 'results', 'desktop-dashboard', 'local');
export const baseURL = process.env.DD_BASE_URL ?? 'http://127.0.0.1:8099';

export type NetworkEntry = {
  url: string;
  method: string;
  status: number;
  mimeType: string;
  startedDateTime: string;
  timeMs: number;
  bytes: number;
  failed?: string;
};

export type Evidence = {
  /** 本用例的证据目录（相对仓库根，便于写进 case-results.json）。 */
  readonly relativeDir: string;
  readonly absoluteDir: string;
  consoleErrors: string[];
  consoleAll: string[];
  pageErrors: string[];
  failedRequests: string[];
  notFoundRequests: string[];
  /** 被界面显式处理的 API 404（例如“该事件尚无留证记录”），不算资源缺失。 */
  apiNotFound: string[];
  /** 用例有意制造的失败请求（例如主动 abort 模型文件），不计入控制台噪声判定。 */
  expectedFailures: string[];
  externalRequests: string[];
  network: NetworkEntry[];
  writeJson(name: string, data: unknown): string;
  writeText(name: string, text: string): string;
  screenshot(name: string, selector?: string): Promise<string>;
  /** 断言浏览器控制台没有未处理异常与资源 404（规格 02 §1）。 */
  expectCleanConsole(): void;
  /** 断言没有跨域外部请求（规格 02 §1 的混合内容与 DD-SYS-03）。 */
  expectNoExternalRequests(): void;
};

function slugify(value: string): string {
  return value
    .replace(/[^\p{Letter}\p{Number}]+/gu, '-')
    .replace(/^-|-$/g, '')
    .slice(0, 72) || 'case';
}

function harFile(entries: NetworkEntry[], pageUrl: string) {
  return {
    log: {
      version: '1.2',
      creator: { name: 'EdgeDoctor acceptance harness', version: '0.1.0' },
      pages: [{ startedDateTime: new Date().toISOString(), id: 'page_1', title: pageUrl, pageTimings: {} }],
      entries: entries.map((entry) => ({
        pageref: 'page_1',
        startedDateTime: entry.startedDateTime,
        time: Math.round(entry.timeMs),
        request: {
          method: entry.method,
          url: entry.url,
          httpVersion: 'HTTP/1.1',
          headers: [],
          queryString: [],
          cookies: [],
          headersSize: -1,
          bodySize: -1,
        },
        response: {
          status: entry.status,
          statusText: entry.failed ? entry.failed : '',
          httpVersion: 'HTTP/1.1',
          headers: [],
          cookies: [],
          content: { size: entry.bytes, mimeType: entry.mimeType },
          redirectURL: '',
          headersSize: -1,
          bodySize: entry.bytes,
        },
        cache: {},
        timings: { send: 0, wait: Math.round(entry.timeMs), receive: 0 },
        _failed: entry.failed,
      })),
    },
  };
}

export const test = base.extend<{ evidence: Evidence }>({
  evidence: async ({ page }, use, testInfo: TestInfo) => {
    const caseIds = testInfo.title.match(/DD-[A-Z0-9]+-\d+/g) ?? ['misc'];
    const dir = path.join(evidenceRoot, 'per-case', `${caseIds[0]}-${slugify(testInfo.title)}`);
    fs.mkdirSync(path.join(dir, 'screenshots'), { recursive: true });
    fs.mkdirSync(path.join(evidenceRoot, 'screenshots'), { recursive: true });

    const collector: Evidence = {
      relativeDir: path.relative(process.cwd(), dir).replace(/\\/g, '/'),
      absoluteDir: dir,
      consoleErrors: [],
      consoleAll: [],
      pageErrors: [],
      failedRequests: [],
      notFoundRequests: [],
      apiNotFound: [],
      expectedFailures: [],
      externalRequests: [],
      network: [],
      writeJson(name, data) {
        const file = path.join(dir, name);
        fs.writeFileSync(file, `${JSON.stringify(data, null, 2)}\n`, 'utf8');
        return file;
      },
      writeText(name, text) {
        const file = path.join(dir, name);
        fs.writeFileSync(file, text, 'utf8');
        return file;
      },
      async screenshot(name, selector) {
        // 只保留一份截图：screenshots/ 供报告引用，用例目录通过附件指向同一路径。
        const target = path.join(evidenceRoot, 'screenshots', `${caseIds[0]}-${name}.png`);
        const locator = selector ? page.locator(selector) : page;
        await locator.screenshot({ path: target });
        return path.relative(process.cwd(), target).replace(/\\/g, '/');
      },
      expectCleanConsole() {
        const intentional = (text: string) => collector.expectedFailures.some((marker) => text.includes(marker));
        const pageErrors = collector.pageErrors.filter((text) => !intentional(text));
        const consoleErrors = collector.consoleErrors.filter((text) => !intentional(text));
        const notFound = collector.notFoundRequests.filter((text) => !intentional(text));
        expect(pageErrors, `页面存在未处理异常：${pageErrors.join(' | ')}`).toHaveLength(0);
        expect(consoleErrors, `控制台存在错误：${consoleErrors.join(' | ')}`).toHaveLength(0);
        expect(notFound, `存在资源 404：${notFound.join(' | ')}`).toHaveLength(0);
      },
      expectNoExternalRequests() {
        const allowed = ['127.0.0.1', 'localhost'];
        const external = collector.externalRequests.filter((url) => !allowed.some((host) => url.includes(host)));
        expect(external, `存在外部请求：${external.join(' | ')}`).toHaveLength(0);
      },
    };

    page.on('console', (message) => {
      const text = `[${message.type()}] ${message.text()}`;
      collector.consoleAll.push(text);
      if (message.type() !== 'error') return;
      // 浏览器会为任何 404 打一条控制台错误。对 /api/v1/ 的数据 404，
      // 界面本身已把它渲染成明确状态（例如“尚无留证记录”），因此单独登记，
      // 只有静态资源缺失才算资源 404。
      const url = message.location()?.url ?? '';
      if (/Failed to load resource/.test(message.text()) && url.includes('/api/v1/')) {
        collector.apiNotFound.push(`${url} :: ${message.text()}`);
        return;
      }
      // 资源加载失败的原文不含 URL，必须把 URL 一并记下，用例才能按资源名豁免。
      collector.consoleErrors.push(url ? `${text} @ ${url}` : text);
    });
    page.on('pageerror', (error) => collector.pageErrors.push(String(error)));
    page.on('requestfailed', (request) => {
      collector.failedRequests.push(`${request.method()} ${request.url()} :: ${request.failure()?.errorText ?? 'failed'}`);
    });
    page.on('request', (request) => {
      const url = request.url();
      if (/^https?:\/\//.test(url)) collector.externalRequests.push(url);
    });
    page.on('response', async (response) => {
      const request = response.request();
      const url = response.url();
      const status = response.status();
      if (status === 404 || status === 405) {
        if (url.includes('/api/v1/')) collector.apiNotFound.push(`${status} ${url}`);
        else collector.notFoundRequests.push(`${status} ${url}`);
      }
      let bytes = 0;
      try {
        const headers = await response.allHeaders();
        bytes = Number(headers['content-length'] ?? 0);
      } catch { /* 响应体可能已释放 */ }
      const timing = request.timing();
      collector.network.push({
        url,
        method: request.method(),
        status,
        mimeType: response.headers()['content-type'] ?? '',
        startedDateTime: new Date(timing.startTime || Date.now()).toISOString(),
        timeMs: Math.max(0, (timing.responseEnd || 0) - (timing.requestStart || 0)),
        bytes,
      });
    });

    await use(collector);

    collector.writeText('browser-console.log', collector.consoleAll.join('\n'));
    collector.writeJson('network.har', harFile(collector.network, page.url()));
    collector.writeJson('diagnostics.json', {
      pageErrors: collector.pageErrors,
      consoleErrors: collector.consoleErrors,
      failedRequests: collector.failedRequests,
      notFoundRequests: collector.notFoundRequests,
      apiNotFound: collector.apiNotFound,
      externalRequests: collector.externalRequests,
      finalUrl: page.url(),
    });
    for (const file of ['browser-console.log', 'network.har', 'diagnostics.json']) {
      await testInfo.attach(file, { path: path.join(dir, file), contentType: 'text/plain' });
    }
    await testInfo.attach('evidence-dir', { body: collector.relativeDir, contentType: 'text/plain' });
  },
});

export { expect };

/** 通过服务端 API 播种可复现的测试数据；返回响应体。 */
export async function seed(pathname: string, body: unknown, page: Page): Promise<{ status: number; body: unknown }> {
  const response = await page.request.post(pathname, { data: body as Record<string, unknown> });
  return { status: response.status(), body: await response.json().catch(() => null) };
}

export async function apiGet(pathname: string, page: Page): Promise<{ status: number; body: unknown }> {
  const response = await page.request.get(pathname);
  return { status: response.status(), body: await response.json().catch(() => null) };
}

export const uniqueId = (prefix: string): string => `${prefix}-${Date.now().toString(36)}-${Math.floor(Math.random() * 1e4)}`;
