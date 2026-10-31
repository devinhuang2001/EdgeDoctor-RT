# 源代码说明

本目录分两块：`firmware/` 是跑在板子上的 RT-Thread 工程，`host/` 是主机侧的算法、服务端、桌面端与测试工具。

---

## firmware/ — RT-Thread 板端工程

来源：官方 `Edgi_Talk_M55_XiaoZhi` v1.4.0 基线 + EdgeDoctor 应用层，即实际编译出 `06_固件产物/` 里那个 HEX 的同一个工程目录。

```text
firmware/
├── applications/
│   ├── edgedoctor/          ← 本项目新增，全部业务代码在这里
│   │   ├── dsp/             20 ms 分块、1 s 窗口、64 维频带特征
│   │   ├── inference/       INT8 推理与模型权重（编译进固件，不从 SD 卡加载）
│   │   ├── logger/          事件日志与恢复
│   │   ├── profiler/        时延与热样本统计
│   │   ├── protocol/        MQTT 客户端、EDV2 留证封装
│   │   ├── runtime/         monitor 任务、健康基线、命令分发
│   │   ├── sensor/          LSM6DS3TR-C 低频采集、证据抓取
│   │   └── ui/              板端六页 HMI
│   ├── xiaozhi/             官方语音助手应用（基线原样保留）
│   ├── main.c
│   └── SConscript
├── board/                   板级配置
├── packages/                RT-Thread 软件包（cJSON、webnet、mbedtls 等，离线构建需要）
├── edge-impulse/            官方示例残留，但 edge-impulse-sdk/CMSIS/DSP 被 edgedoctor 的 SConscript 引用
├── firmware/                （与 EdgeDoctor-RT 源目录一致的公共 C 核心副本）
├── rtconfig.h / rtconfig.py / SConstruct / SConscript / Kconfig
├── mklinks.bat / mklinks.sh 建立到官方 SDK 的目录链接
└── README_EdgeDoctor.md     板端串口命令速查
```

### 相对完整工程，本包省略了什么

以下目录是官方 SDK 的一部分，体积大且与本次改动无关，已剔除，**构建前需要补齐**：

| 省略项 | 补齐方式 |
| --- | --- |
| `rt-thread/` `libraries/` `libs/` | 由 `mklinks.bat`（或 `mklinks.sh`）指向官方 SDK 1.4.0 的对应目录 |
| `build/` `Debug/` | 构建产物，重新编译即可 |

### 构建步骤

1. 安装 RT-Thread Studio 2.3.1，SDK Manager 装 `PSOC_E84-EDGI-TALK` BSP **v1.4.0**。
2. 把本 `firmware/` 目录放到短路径、无中文的路径下（例如 `D:\work\Edgi_Talk_M55_EdgeDoctor`）。
3. 在该目录执行 `mklinks.bat`，把 `rt-thread`、`libraries`、`libs` 链接到 SDK。
4. 确认 Arm GNU 工具链 13.3 可用（官方 `rtconfig.py` 默认指向 RT-Thread Studio 的安装位置）。
5. 在工程目录执行 `scons -j4`，或直接 `scripts/build-firmware.ps1 -Jobs 4`。
6. 烧录用 KitProg3 / CMSIS-DAP（`DAP / Program-Debug` 口，不是 `USB Device` 应用口）。

产物参考：`06_固件产物/EdgeDoctor-RT_rtthread-fixed_20261002.hex`
SHA-256 `21bad72407808551894845f0500fea915122fd3113e58cac02296383befa838b`，已实板 `Verified OK`，校验 6,637,328 字节。

---

## host/ — 主机侧

```text
host/
├── ai/               KAIST 训练、特征、量化、导出（PyTorch → TFLite → Vela）
├── server/           FastAPI 服务端 + MQTT 接入 + SQLite WAL + EDV2 证据解析
├── ui/desktop/       Vite + React + TypeScript + Three.js 数字孪生控制台
├── tools/            数据打包（EdgePack .edgp）、板端联调脚本
├── scripts/          build-firmware / test / test-ui / prepare-bsp / server-up 等
├── tests/            C 公共核心测试 + Python 数据/服务端 + Playwright 浏览器验收
├── artifacts/        训练产物（曲线、混淆矩阵、量化对比、导出模型）
├── docs/             验收矩阵、数据管线、训练报告、决策日志
└── benchmark/results/v2/  板端冒烟与稳定性原始 JSON
```

### 省略了什么

- `node_modules/`、`.venv-inference/`：依赖目录，`npm ci` / `pip install -r server/requirements.txt` 重建
- `data/`：KAIST 数据集原始文件（约 6 GB），对外分发受 CC BY 4.0 约束，需自行下载
- `benchmark/results/desktop-dashboard/`：约 900 MB 浏览器验收产物，精选结论已放入 `05_验证与证据/`
- `presentation/`：PPT 构建中间产物，成品在 `02_演示PPT/`

### 复现命令

```powershell
# 主机回归（数据管线 + C 公共核心 + 服务端）
cd host
powershell -ExecutionPolicy Bypass -File .\scripts\test.ps1

# 桌面端构建与单测
cd host\ui\desktop
npm ci
npm run build
npm test

# 桌面端完整验收（浏览器 / 视觉 / 安全 / 可访问性四层）
cd host
powershell -ExecutionPolicy Bypass -File .\scripts\test-ui.ps1
```

### 数据集

KAIST Human Lab 旋转机械数据集 v6，DOI `10.17632/ztmf3m7h5x.6`，CC BY 4.0。
放到 `host/data/raw/kaist-v6`，再按 `host/docs/data-pipeline.md` 生成 manifest、记录级划分与 `.edgp` 包。
数据不进仓库是刻意的：先按整条记录划分再切窗，避免同一段运行同时出现在训练和测试里。

---

## 一个提醒

`firmware/firmware/` 与 `host/firmware/` 是同一份公共 C 核心的两处副本。改代码时以 `host/firmware/` 为准，再用 `scripts/prepare-bsp.ps1` 同步进 BSP，不要手改两份。
