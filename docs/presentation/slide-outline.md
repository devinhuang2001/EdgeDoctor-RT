# Slide Outline

## Meta
- Topic: EdgeDoctor-RT —— 面向智能工厂旋转设备的边缘 AI 预测性维护系统
- Scenario: 比赛现场答辩讲解（评委席大屏投影，讲者口头阐述，需要"重点突出、便于念稿"）
- Content Source: provided（Edgi_Talk_M55_EdgeDoctor 工程 + EdgeDoctor-RT 前端截图 + 六分区数字孪生工厂 3D 素材）
- Style: 极简灰黑商务（模板 00213）— #F2F2F2 浅灰底 + #111111 纯黑无衬线大字 + 细线胶囊标签 + 直角图片；彩色只由产品实际截图提供，界面本身保持单色调
- Slide Count: 13
- Generated At: 2026-09-16T22:50:00+08:00

## Source Materials
- 工程代码：`Edgi_Talk_M55_EdgeDoctor`（applications/edgedoctor、firmware/common）——IMU 采集、DSP、板载 MLP、MQTT 证据链、事件日志、基线管理、回放
- PC 大屏前端截图 4 张（1600px 宽）：工厂总览 / 实时监测 / 信号分析 / 事件中心
- PC 前端补充截图：六页面单页图（工厂总览、旋转设备区、实时监测、信号分析、事件告警表、健康趋势）
- 移动端 H5 截图 4 张（1086×1448）：连接与同步 / 设备与基线 / 振动趋势 / 事件中心
- 设备端工程：LVGL 六标签页 UI（总览·信号·事件·趋势·设备·连接）
- 六分区数字孪生工厂 3D 渲染：整体鸟瞰、电机总装、电机爆炸图、ZONE A–F
- 硬件实拍：Edgi-Talk 开发板（PSOC Edge E84 / M33 安全核 + M33 非安全核 + M55 应用核）

## Slide-by-Slide Outline
1. **Slide 1 — Cover** — EdgeDoctor-RT：让设备更健康，让生产更可靠 | L02 | Image: yes（3D 工厂/电机） | Chart: no
2. **Slide 2 — 行业痛点** — 传统巡检三个断点：看不见、算不准、留不住 | L07 | Image: no | Chart: no
3. **Slide 3 — 产品定位 + 目标用户** — 一台边缘盒子 + 一套证据链，服务三类角色 | L08 | Image: yes（移动端截图） | Chart: no
4. **Slide 4 — 系统架构** — 端—边—云三段式，数据不出厂区即可闭环 | L13 | Image: no | Chart: no
5. **Slide 5 — 核心功能总览** — 六大能力一屏看清 | L15 | Image: yes（PC 总览缩略） | Chart: no
6. **Slide 6 — 能力①：实时监测** — 208 Hz 板载采集，实时状态一眼可读 | L05 | Image: yes（PC 实时监测） | Chart: no
7. **Slide 7 — 能力②：信号分析 + AI 诊断** — 从波形到频谱，再到五分类结论 | L06 | Image: yes（PC 信号分析） | Chart: yes
8. **Slide 8 — 能力③：事件中心闭环** — 检测→报警→确认→恢复，全生命周期可追溯 | L05 | Image: yes（PC 事件中心） | Chart: no
9. **Slide 9 — 能力④：多端协同** — 设备端 / 移动端 / PC 端，同一套状态三种触点 | L16 | Image: yes（移动 4 图 + 硬件） | Chart: no
10. **Slide 10 — 应用场景** — 六分区数字孪生工厂，一厂六区统一监测 | L14 | Image: yes（3D 工厂鸟瞰） | Chart: no
11. **Slide 11 — 技术亮点** — 边缘推理、自适应基线、可验证证据链 | L07 | Image: no | Chart: no
12. **Slide 12 — 项目价值** — 三个关键数字说明价值 | L11 | Image: no | Chart: yes
13. **Slide 13 — Closing** — 让设备更健康 · 让生产更可靠 | L20 | Image: yes（电机渲染） | Chart: no

## Visual Rhythm Notes
- Chart-bearing slides at positions: 7, 12
- 深色/反白强调页建议位置: 1、13（首尾形成呼应），保留 L14 满幅图页 10 作为视觉高点
- Slides are viewed on large screens, often from a distance. Small text is unreadable. Every text element must be sized generously and fill its container space rather than floating in emptiness.
- Slides should feel visually rich and layered, not like text pasted on rectangles. 产品真实截图是本套 PPT 的"彩色来源"与核心说服力，需要大尺寸、直角、带细边框处置，形成"屏幕阵列"节奏。
- 图片统一直角 + 1px 细边框；胶囊标签作为章节标记贯穿全篇。
