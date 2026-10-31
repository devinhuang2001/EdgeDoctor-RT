# EdgeDoctor-RT

**面向旋转机械预测性维护的离线 Edge AI 与 RTOS 工业诊断终端**

基于 Infineon PSoC™ Edge E84（Arm Cortex-M55）+ RT-Thread 构建，在 MCU 上完成振动信号采集、频域特征提取、神经网络推理、本地 HMI 显示与诊断留痕，**设备断网时依然能输出诊断结论**。

> 2026 RT-Thread 嵌入式大赛参赛作品 · Edgi-Talk 赛道

| | |
|---|---|
| 演示视频 | <https://devinhuang2001.github.io/EdgeDoctor-RT/> |
| 硬件平台 | RT-Thread Edgi-Talk / Infineon PSoC™ Edge E84 / Cortex-M55 |
| 软件基线 | 官方 `Edgi_Talk_M55_XiaoZhi` v1.4.0（应用层叠加，未改启动/链接/HAL/Wi-Fi 资源） |
| 许可证 | Apache-2.0 |

---

## 1. 这是什么

旋转机械（电机、风机、泵、压缩机、工业传动）的定期巡检容易错过渐进故障，而完全依赖云端会引入网络可用性、上行成本、响应延迟和现场数据边界问题。

EdgeDoctor-RT 把诊断闭环整体下沉到设备侧：每 20 ms 处理一个数据块，用 1 s 滑动窗口提取 64 维频带特征，经 64-32-5 MLP 推理，输出**正常 / 轴承内圈故障 / 轴承外圈故障 / 不对中 / 不平衡**五类状态，并给出置信度、异常分与处置建议。

项目不只展示一个离线训练脚本，而是覆盖数据可信度、板端运行时、实时任务、HMI、日志、性能统计与验证门槛。模型准确率只是系统证据之一。

## 2. 关键指标

| 指标 | 结果 | 口径 |
|---|---|---|
| 数据规模 | 45 条记录 / 1,800 窗口 | 每条记录 40 个 1 s 窗口，先分记录再切窗 |
| FP32 留出记录 | Accuracy 96.50% / Macro-F1 96.44% | 测试集 200 窗口，每类 40 |
| 静态 Q7 板端模型 | Accuracy 96.50% / Macro-F1 96.42% | 与 FP32 预测一致率 99.50% |
| 模型参数体积 | 2,508 B | 仅模型参数，不含 RTOS/LVGL/缓冲区 |
| LOLO 最难工况 | 4 Nm Macro-F1 77.88% | 揭示跨负载分布偏移 |

**证据等级声明**：板端延迟目前只有 3 帧 synthetic smoke 数据（静态 Q7 端到端 P95 967 µs），**不能替代正式性能测量**；当前推理后端为 Reference C，尚未宣称 CMSIS-NN 或 Ethos-U55 加速。

## 3. 功能

- **五类设备状态诊断** —— 每个有效窗口输出类别、置信度、异常分、五类分数、前处理/推理/端到端耗时、丢帧数与后端类型。
- **四页 LVGL 本地 HMI（800×480）** —— 概览（健康度/当前状态/机械示意图）、信号（频谱与输入质量）、诊断（五类概率、处置建议）、系统（P50/P95、丢帧、日志路径、模型信息）。
- **数据回放与实时节拍** —— 原始样本转为 EdgePack，板端 `edgeai replay <path.edgp> [fast]`；实时模式严格按 20 ms 节拍运行。
- **诊断留痕** —— 结果经容量 16 的 RT-Thread 消息队列送入独立 `edge_log` 线程，异步写入 `/flash/edgedoctor.csv`；队列写满只累计计数，不阻塞采集与推理。

## 4. 硬件框架

```
振动采集            主控                      本地显示            留痕存储
6.4 kHz 双轴 IMU → PSoC Edge E84 (M55) → 800×480 LVGL 四页 → /flash CSV
20 ms 分块          RT-Thread 实时内核
```

模型参数编译进片上只读区，运行日志落在 `/flash` 文件系统。固件数据结构同时保留 16 kHz 音频与 6.4 kHz 双轴振动通道，以便后续接入麦克风与加速度传感器。

> **采集口径**：当前公开数据集中声学文件仅 5 条零负载记录且与振动采集时间不一致，因此正式五类准确率**只使用振动**，本项目不把声学通道描述为已完成的融合模型。

## 5. 软件框架

| 执行上下文 | 职责 | 实时性策略 | 资源 |
|---|---|---|---|
| `edge_replay` | 读取并校验 EdgePack，按 20 ms 推送数据块 | 优先级 20；实时 / fast 可选 | 固定 8 KiB 栈 |
| 窗口 + DSP + 推理 | 构造窗口、频域特征、模型推理、发布结果 | 位于回调热路径；无文件 I/O、无动态分配 | SOCMEM 固定状态 |
| `edge_log` | 格式化 CSV 并写入 Flash | 消息队列解耦；写失败重开文件 | 固定 4 KiB 栈，队列 16 |
| LVGL UI 周期 | 读取结果/频谱快照并更新四页界面 | 只读快照，不等待推理线程 | 由 XiaoZhi UI 循环驱动 |

**数据契约**

| | 定义 |
|---|---|
| Chunk | 20 ms；音频 320 个 int16 样本，振动 128 帧 × 2 通道 int16 |
| Window | 1 s，含 50 个 chunk；每 25 个 chunk 输出一次 |
| Feature | 板端频域摘要；正式分类使用 64 维归一化振动频带 |
| Result | 类别、置信度、五类分数、异常分、耗时、丢帧、后端与状态 |
| Snapshot | 代际计数 + 双缓冲/内存屏障，避免 UI 读到半更新数据 |

## 6. 仓库结构

```
EdgeDoctor-RT/
├── firmware/            板端 RT-Thread 工程（应用层 + 软件包，官方 SDK 通过 mklinks 链接）
│   └── applications/edgedoctor/
│       ├── dsp/         20 ms 分块、1 s 窗口、64 维频带特征
│       ├── inference/   INT8 推理与模型权重（编译进固件）
│       ├── logger/      事件日志与恢复
│       ├── profiler/    时延与热样本统计
│       ├── protocol/    MQTT 客户端、EDV2 留证封装
│       ├── runtime/     monitor 任务、健康基线、命令分发
│       ├── sensor/      LSM6DS3TR-C 低频采集、证据抓取
│       └── ui/          板端四页 HMI
├── host/                主机侧：训练量化 / 服务端 / 桌面端 / 测试工具
│   ├── ai/              KAIST 训练、特征、量化、导出（PyTorch → TFLite → Vela）
│   ├── server/          FastAPI 服务端 + MQTT 接入 + SQLite WAL + EDV2 解析
│   ├── ui/desktop/      Vite + React + TypeScript + Three.js 数字孪生控制台
│   ├── tools/           数据校验、切分、EdgePack 打包
│   ├── tests/           C 公共核心 + Python + Playwright 浏览器验收
│   └── docs/            验收矩阵、数据管线、训练报告、决策日志
├── docs/
│   ├── index.html       演示视频与项目介绍页（GitHub Pages）
│   ├── video/           演示视频、字幕、封面
│   ├── verification/    验收结论、桌面端截图、板端证据、训练产物、技术文档
│   └── presentation/    答辩讲稿、PPT 逐页文字稿、视频分镜
├── README_源代码说明.md  构建与移植步骤
└── LICENSE              Apache-2.0
```

## 7. 快速开始

### 固件构建

1. 安装 RT-Thread Studio 2.3.1，SDK Manager 装 `PSOC_E84-EDGI-TALK` BSP **v1.4.0**。
2. 把 `firmware/` 放到短路径、无中文的目录下（例如 `D:\work\Edgi_Talk_M55_EdgeDoctor`）。
3. 执行 `mklinks.bat`（或 `mklinks.sh`），把 `rt-thread`、`libraries`、`libs` 链接到官方 SDK。
4. 确认 Arm GNU 工具链 13.3 可用。
5. `scons -j4`，或 `scripts/build-firmware.ps1 -Jobs 4`。
6. 烧录用 KitProg3 / CMSIS-DAP（`DAP / Program-Debug` 口，不是 `USB Device` 应用口）。

### 主板端命令

```console
msh /> edgeai info
msh /> edgeai replay /flash/synthetic_smoke_2s.edgp
msh /> edgeai replay /flash/synthetic_smoke_2s.edgp fast
```

### 主机回归

```powershell
cd host
.\scripts\test.ps1
```

覆盖数据解析、同步、预处理、切分、包安全与公共 C 核心。

## 8. 数据集与许可证

训练数据使用 KAIST Human Lab 旋转机械数据集 v6（[Mendeley Data](https://data.mendeley.com/datasets/ztmf3m7h5x/6)，DOI 10.17632/ztmf3m7h5x.6，CC BY 4.0），原始数据约 6 GB，**不随本仓库分发**，需自行下载。

本仓库代码以 **Apache License 2.0** 发布，详见 [LICENSE](LICENSE)。

## 9. 已知边界

| 项目 | 状态 | 说明 |
|---|---|---|
| 板端正式性能 | 待补 | 目前仅 3 帧冒烟数据，正式门槛为 ≥200 热样本 + P50/P95 + 丢帧统计 |
| 功耗实测 | 待补 | 需注明电压、电流表型号、场景与统计窗口 |
| 四页实机截图 | 待补 | 代码完成，正式 GUI 证据需实拍与 100 次切换测试 |
| 声学五类融合 | 未纳入 | 公开声学文件仅 5 条零负载且与振动不同步 |
| U55 加速 | 未验证 | 当前 Reference C，未宣称 NPU 覆盖率 |
