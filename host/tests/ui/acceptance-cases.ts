// 验收用例登记表，逐条对应 docs/specs/desktop-dashboard-v1/02-desktop-dashboard-acceptance-test-spec.md。
// 未被自动化覆盖的条目在报告中保留 NOT_RUN 或 MISSING_EVIDENCE，绝不代填 PASS。

export type Layer = 'UNIT' | 'API' | 'BROWSER' | 'VIS' | 'GPU' | 'LAN';

export type AcceptanceCase = {
  id: string;
  layer: Layer;
  precondition: string;
  input: string;
  expected: string;
};

export const ACCEPTANCE_CASES: AcceptanceCase[] = [
  { id: 'DD-SYS-01', layer: 'UNIT', precondition: '干净依赖安装', input: 'lint、类型检查、生产构建', expected: '三项退出码为 0，产物只引用本地资源' },
  { id: 'DD-SYS-02', layer: 'BROWSER', precondition: '服务已启动', input: '访问 /dashboard/ 与 /dashboard-v2/ 并刷新深链接', expected: '旧页与新页均 200，默认入口仍指向旧页，新页深链接可恢复' },
  { id: 'DD-SYS-03', layer: 'BROWSER', precondition: '断开外网', input: '加载 /dashboard-v2/ 全部路由', expected: '无外部字体、脚本、图片请求，骨架与关键文字可用' },
  { id: 'DD-SYS-04', layer: 'BROWSER', precondition: '已构建产物', input: '校验 GLB、许可证与绑定配置', expected: '模型 SHA-256 与冻结清单一致，许可证存在，绑定 schema 通过且组合唯一' },
  { id: 'DD-SYS-05', layer: 'BROWSER', precondition: '新页已部署', input: '四条路由首次加载、硬刷新、前进后退', expected: '无错误绝对路径与 404，路由与页面标题一致' },
  { id: 'DD-API-01', layer: 'API', precondition: '空库、仅遥测、活动事件、恢复未确认、过期事件', input: '请求状态与汇总接口', expected: '只使用服务端状态，NO_DATA/STALE/UNKNOWN 不显示健康结论' },
  { id: 'DD-API-02', layer: 'API', precondition: '服务端可用', input: '乱序 revision、缺字段、非法数值、未知枚举', expected: 'schema 校验拒绝或降级为数据不可用，不抛未处理异常' },
  { id: 'DD-API-03', layer: 'BROWSER', precondition: '设备 A 与设备 B 均有数据（冻结绑定只有一台 verified 设备，用例经总览接口注入第二台已验证设备构造 A→B）', input: '先请求设备 A 再快速切到 B，然后延迟返回 A', expected: 'B 页面不被 A 的状态、趋势或事件覆盖' },
  { id: 'DD-API-04', layer: 'BROWSER', precondition: '存在活动未确认事件', input: '确认后依次模拟 PENDING/APPLIED/REJECTED/EXPIRED', expected: '202 只显示已提交，只有 APPLIED 显示已应用，拒绝与过期保留重试解释' },
  { id: 'DD-API-05', layer: 'BROWSER', precondition: '存在事件', input: '留证请求分别返回无证据、CRC 失败、合格元数据、401', expected: '只有合格元数据显示下载，401 提示认证，其余不得称为原始数据已获取' },
  { id: 'DD-API-06', layer: 'BROWSER', precondition: 'SSE 连通', input: '连续发送 20 条通知后关闭 SSE', expected: '250 ms 去抖刷新，断流后 10 秒轮询恢复，无刷新风暴' },
  { id: 'DD-API-07', layer: 'API', precondition: '0/29/30/31 秒前的遥测与无绑定资产', input: '请求工厂汇总', expected: '在线状态严格符合冻结阈值，无绑定资产为 UNKNOWN 且不计入在线' },
  { id: 'DD-3D-01', layer: 'BROWSER', precondition: '打开工厂页', input: '网络正常与模拟 GLB 失败各一次', expected: '正常显示进度与场景，失败显示降级说明且不阻塞事件入口' },
  { id: 'DD-3D-02', layer: 'BROWSER', precondition: '工厂模型已加载', input: '点击已绑定实例、未绑定实例与空白区域', expected: '绑定项选择正确，未绑定项明确标识，空白行为一致' },
  { id: 'DD-3D-03', layer: 'BROWSER', precondition: '选择 B 区电机', input: '依次置为在线、活动报警、恢复未确认、离线', expected: '高亮与侧栏文案与服务端状态一致，不出现未经验证的部件定位' },
  { id: 'DD-3D-04', layer: 'BROWSER', precondition: '工厂模型已加载', input: '旋转、缩放、分区聚焦、复位、键盘操作', expected: '相机保持可用范围，模型可复位，焦点与文字替代可辨识' },
  { id: 'DD-3D-05', layer: 'BROWSER', precondition: '工厂页可加载', input: '与其他页面来回 20 次并记录资源计数', expected: '退出后资源基线恢复，第 20 次相对第 2 次无持续增长' },
  { id: 'DD-3D-06', layer: 'GPU', precondition: '验收机硬件加速', input: '重复加载 10 次记录首个可用画面时间', expected: 'P95 首个可用画面不超过 8 秒，否则按实测值报告' },
  { id: 'DD-UI-01', layer: 'VIS', precondition: '三种分辨率', input: '四条路由的加载、无数据、告警、服务故障状态截图', expected: '无裁切重叠，底部导航不遮挡图表，中文可读' },
  { id: 'DD-UI-02', layer: 'BROWSER', precondition: '页面已加载', input: 'Tab/Shift+Tab/Enter/Space 操作导航、筛选、确认与留证', expected: '焦点可见顺序合理，关键操作可完成，状态由文字与 ARIA live 表达' },
  { id: 'DD-UI-03', layer: 'UNIT', precondition: '趋势数据', input: '正常、缺失、单点、恒定值、报警点与超长序列', expected: '单位 g，缺失为断点，恒定值无 NaN，报警点只来自真实字段' },
  { id: 'DD-UI-04', layer: 'BROWSER', precondition: '事件列表', input: '0、1、50 条事件与长 ID、中文、特殊字符', expected: '条目可读，ID 被转义，无 XSS，不把 50 条称为全厂总量' },
  { id: 'DD-UI-05', layer: 'BROWSER', precondition: '信号分析页', input: '无分析结果、可用特征、可用 EDV2、损坏证据', expected: '无分析说明原因，频谱只来自校验结果，损坏证据不可视为有效' },
  { id: 'DD-UI-06', layer: 'UNIT', precondition: '分析数据', input: '208 Hz 与其他采样率', expected: '横轴不超过奈奎斯特频率，208 Hz 不出现 104 Hz 以上峰值' },
  { id: 'DD-UI-07', layer: 'UNIT', precondition: '源代码与产物', input: '搜索随机图形与样例诊断内容', expected: '没有 Math.random、固定 42.3 Hz、96.8% 健康度或轴承异常兜底' },
  { id: 'DD-SEC-01', layer: 'BROWSER', precondition: '页面已加载', input: '在事件名称、ID、备注与 API 错误中注入 HTML/脚本', expected: '以文本显示，不执行脚本，无 innerHTML 注入路径' },
  { id: 'DD-SEC-02', layer: 'BROWSER', precondition: '已构建产物', input: '搜索源码、产物、URL、存储与网络日志', expected: '不含令牌、MQTT 密码、私钥或真实凭据' },
  { id: 'DD-SEC-03', layer: 'BROWSER', precondition: '页面已加载', input: '401、403、429、500、超时与错误 JSON', expected: '每种状态有可理解错误与恢复操作，不白屏不无限重试' },
  { id: 'DD-A11Y-01', layer: 'BROWSER', precondition: '页面已加载', input: '可访问性扫描与手工键盘检查', expected: '无关键严重违规，状态颜色配合图标文字' },
  { id: 'DD-PERF-01', layer: 'BROWSER', precondition: '稳定场景', input: '连续操作 60 秒记录 Long Task、帧时间与内存', expected: '无连续超过 1 秒 UI 冻结，指标写入 metrics' },
];

export const CASE_INDEX = new Map(ACCEPTANCE_CASES.map((item) => [item.id, item]));
