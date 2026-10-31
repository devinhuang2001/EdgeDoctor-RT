# EdgeDoctor-RT 电脑版数字孪生控制台

`docs/specs/desktop-dashboard-v1/` 的实现。Vite + React + TypeScript + Three.js 构建，
产物写入 `server/web-v2/`，由同一个 FastAPI 进程挂载在 `/dashboard-v2/`。
旧版 `/dashboard/` 保持原样，根路径在切换评审通过前仍指向旧版。

## 构建与运行

```powershell
# 依赖（锁文件已提交：package-lock.json）
npm ci

# 构建期校验资产绑定并生成服务端副本，然后 lint、类型检查、生产构建
npm run validate:bindings   # 也可单独执行，校验失败会让构建中止
npm run lint
npm run typecheck
npm run build               # 产物：..\..\server\web-v2\

# 前端单元测试
npm test
```

运行服务（仓库根目录）：

```powershell
$env:PYTHONPATH='EdgeDoctor-RT'
uvicorn server.app:app --host 127.0.0.1 --port 8080
```

随后访问 `http://127.0.0.1:8080/dashboard-v2/`。

## 目录

```text
src/
  api/        服务端契约类型、运行时 schema 校验、错误分类与请求取消
  state/      运行时状态仓、SSE 去抖与轮询回退、按设备取数
  scene/      工厂与设备三维场景、资源释放工具、验收探针
  components/ 卡片、徽章、图表、事件列表、证据卡等通用组件
  pages/      工厂总览 / 实时监测 / 事件中心 / 信号分析
  config/     冻结的资产绑定配置
  lib/        纯函数：状态映射、趋势几何、频谱规则、格式化
  styles/     设计令牌、布局与组件样式
```

## 数据与边界

- 所有业务数据经 HTTPS/HTTP API 与 SSE；前端不连接 MQTT、不读写数据库。
- 状态、在线判定、诊断字段一律来自服务端；`NO_DATA` 时不展示健康结论。
- 三维场景只保存渲染与选择状态；设备与场景节点的映射来自冻结的绑定配置。
- 频谱横轴上限取自证据采样率且不超过奈奎斯特频率；服务端未提供已校验证据时显示“不可用”，
  不使用随机数、固定主频或固定故障类别填充。
- API Token 只保存在页面内存，不写入存储、URL、日志或构建产物。

## 资产

`public/models/` 下的模型与许可证来自 `EdgeDoctor_SmartFactory_3D_AssetPack`，
SHA-256 冻结在 `assets.lock.json`，构建期与验收期都会校验。
几何资产为 CC0，源脚本为 MIT，许可证原文随资产一并发布。

## 验收

浏览器验收在仓库根目录执行（规格 02 的 BROWSER / VIS / GPU 层）：

```powershell
powershell -ExecutionPolicy Bypass -File ..\..\scripts\test-ui.ps1
```

脚本依次执行绑定校验、lint、类型检查、构建、前端单测、完整主机回归与 Playwright 用例，
证据写入 `benchmark/results/desktop-dashboard/{run_id}/`。
