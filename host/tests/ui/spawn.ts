// 验收用例里的子进程调用。
//
// 两个必须由这一层统一承担的问题：
//
// 1. 子进程不能拿 stdin 管道。spawnSync 默认 stdio 是 'pipe'，而在这台验收机上
//    为子进程创建 stdin 管道会直接失败：spawnSync 返回 status=null、
//    error.code='EBUSY'，stdout / stderr 全是 null——于是"子进程根本没起来"被记成
//    一条 0 字节、没有任何信息的失败。实测把 stdio 改成 ['ignore','pipe','pipe']
//    立即恢复正常，'inherit' / 'ignore' 也正常，说明失败点在管道创建，
//    既不是 CreateProcess 也不是 shell（曾经误判为 cmd.exe 的问题，按 shell:true /
//    shell:false 都复现了同一个 EBUSY）。验收子进程本来就不读 stdin，显式忽略。
//
// 2. 退避重试。EBUSY / EAGAIN / ENOBUFS 这类都是瞬时资源错误，重试即可恢复。
//    重试仍失败时，把 spawn 自身的错误码和消息一并写进输出，保证证据文件里
//    永远能看出是"没起来"还是"起来了但断言不过"。
//
// 关于解释器：整轮验收必须用同一个 Python。`python` 在不同 shell 下会解析到
// 不同安装（PATH 顺序不同），因此由 scripts/test-ui.ps1 统一解析后经 DD_PYTHON
// 传进来；这里只做兜底。

import { spawnSync } from 'node:child_process';

export type RunResult = {
  status: number;
  output: string;
  attempts: number;
};

/** 只会瞬时发生、值得重试的 spawn 失败码。 */
const TRANSIENT_CODES = new Set(['EBUSY', 'EAGAIN', 'ENOBUFS', 'EMFILE', 'ENFILE']);

const MAX_ATTEMPTS = 4;

/** 同步 sleep：spawnSync 是同步的，这里不能用 await。 */
function sleepSync(ms: number): void {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

function describe(error: NodeJS.ErrnoException, attempt: number, total: number): string {
  return `\n[spawn error] attempt ${attempt}/${total} ${error.name}: ${error.message} (code=${error.code ?? 'n/a'})`;
}

/**
 * 直连可执行文件运行一个子进程；瞬时资源错误会退避重试。
 *
 * @param command 可执行文件（不要传 shell 命令串，本函数不经过 shell）
 * @param args    参数数组
 * @param cwd     工作目录
 * @param env     追加的环境变量
 */
export function run(
  command: string,
  args: string[],
  cwd: string,
  env: Record<string, string> = {},
): RunResult {
  let output = '';
  let status = 1;

  for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt += 1) {
    const result = spawnSync(command, args, {
      cwd,
      encoding: 'utf8',
      env: { ...process.env, ...env },
      shell: false,
      // 关键：不要让子进程拿到 stdin 管道。
      // spawnSync 默认 stdio 是 'pipe'（三路都是管道），而在这台机器上创建
      // 子进程的 stdin 管道会直接失败：spawnSync 返回 status=null 且
      // error.code='EBUSY'，stdout/stderr 全是 null。
      // 实测同一命令改成 ['ignore','pipe','pipe'] 立刻正常，'inherit'/'ignore' 也正常——
      // 失败的是管道创建，不是 CreateProcess，也不是 shell。验收子进程本来就不该读
      // stdin，所以这里显式忽略。
      stdio: ['ignore', 'pipe', 'pipe'],
    });

    const error = result.error as NodeJS.ErrnoException | undefined;
    if (!error) {
      return {
        status: result.status ?? 1,
        output: `${result.stdout ?? ''}${result.stderr ?? ''}`,
        attempts: attempt,
      };
    }

    // 创建失败：stdout / stderr 均为 null，必须靠 error 才能解释这条失败。
    const transient = TRANSIENT_CODES.has(error.code ?? '');
    output += describe(error, attempt, MAX_ATTEMPTS);
    if (!transient) {
      return { status: 1, output, attempts: attempt };
    }
    if (attempt < MAX_ATTEMPTS) {
      sleepSync(300 * attempt);
    }
  }

  return { status, output, attempts: MAX_ATTEMPTS };
}

/** 本机 Node 可执行文件；用它跑 vitest / eslint，避免 npx 的 .cmd 间接层。 */
export const nodeBin = process.execPath;

/** 整轮验收统一使用的 Python 解释器。 */
export const pythonBin = process.env.DD_PYTHON ?? 'python';
