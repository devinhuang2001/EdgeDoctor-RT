# EdgeDoctor V2 验收测试规格

版本0.1，2026-09-06。所有项目初始为NOT_RUN；以下是未来验收标准，不是实测结果。

## 1. 放行规则和环境

状态：PASS、FAIL、MISSING_EVIDENCE、NOT_RUN。只有对应原始数据/日志/截图存在且满足阈值才可PASS。测试替身只能通过HOST/SIM门禁，不能通过HIL或手机门禁。

测试层：HOST=PC上的C/Python算法与格式；SIM=真实Broker/数据库配模拟设备；HIL=真实板子/真实IMU/文件系统；PHONE=实际手机；VIS=800×480实机视觉。浏览器原型单独标记PROTOTYPE。

统一前提：冻结固件git SHA或未提交差异hash、工具链版本、SDK v1.4.0、依赖锁、设备WHO_AM_I/配置回读、baseline/policy/profile版本、Broker和服务端版本。板端串口日志和服务日志均带单调时间/UTC映射；精度比较不得把窗口当独立设备。

设备：Edgi-Talk、供电/调试串口、局域网AP、部署主机、至少一台Android手机（iOS单列）。采样率用至少60秒累计计数与独立时间参考；振动幅值标定需要可控激励与参考传感器/已知物理输入，缺少仪器则只通过数值换算测试，幅值绝对精度标MISSING_EVIDENCE。

联机性能环境：同局域网RTT≤20 ms、丢包<0.1%，每设备1 Hz遥测、正常无积压；另用故障注入增加延迟/丢包。手机锁屏条件、通知权限、省电设置须附记录。

证据目录：benchmark/results/v2/{run_id}/，包含manifest.json、serial.log、server.log、metrics.json、case-results.json、pcap/或协议日志、screenshots/、clips/、checksums.txt。metrics每个延迟含起止定义、样本数、P50/P95/P99/max；暖机2分钟后至少200个输出样本。

每项用例结果必须含id、layer、status、preconditions、expected、observed、artifact_paths。允许skip仅转MISSING_EVIDENCE并说明缺什么，不计通过率。

## 2. 基线和资源门禁

| ID | 层 | 操作与输入 | 通过条件/证据 |
|---|---|---|---|
| T-SYS-01 | HOST/BUILD | 执行scripts/test.ps1及build-firmware.ps1，保留完整输出 | 旧16项Python及C测试全部通过，固件退出0，无新增警告；新增测试也须进入总入口 |
| T-SYS-02 | HIL | 上电打印WHO_AM_I、I2C地址、ODR/量程/FIFO/滤波回读、SDK/hash | 和冻结profile完全一致；未知ID不写配置，显示CONFIG_ERROR |
| T-SYS-03 | HIL | 读map、各线程stack high-water；TLS重连+UI切页+留证共同运行 | 新固定RAM≤160KiB，TLS新增峰值≤96KiB，UI新增堆≤160KiB；全系统峰值后≥25%余量，栈最低余量≥25% |
| T-SYS-04 | HIL | 查询挂载容量、预分配文件、随机写/fsync/rename后断电 | 应用配额1MiB且开始可用≥1.25MiB；挂载语义可证明，旧有效文件完整 |
| T-SYS-05 | HIL | 运行30分钟后进行8小时整机测试，含MQTT重连、切页、播报 | 无重启/死锁；稳态最后1小时相对第1小时堆占用增长≤4KiB且无持续增长趋势；正常阶段FIFO overrun=0 |
| T-SYS-06 | HIL | 热路径malloc/free包装计数，运行CAP/DSP/报警10000次 | 采集/DSP/推理/报警判定热路径动态分配次数0；网络TLS不混入该计数 |

## 3. CAP / QLT：采集与质量

| ID | 层 | 操作与输入 | 通过条件/证据 |
|---|---|---|---|
| T-CAP-01 | HIL | 208Hz三轴连续30分钟，同时正常联网与UI刷新 | 60秒区间实测采样率误差≤2%，known drops=0、unknown gaps=0、FIFO overrun=0；提交P99≤2ms |
| T-CAP-02 | HOST | 固定ADC向量、冻结灵敏度和轴映射，计算g/m/s² | C与Python绝对误差≤1e-5g，轴交换/符号符合profile |
| T-CAP-03 | HIL | 六面静置，检查重力向量；参考激励覆盖5/10/20/40Hz | 静态重力幅值误差≤5%；动态幅值误差≤10%（指定参考装置），否则该子项缺证据 |
| T-CAP-04 | HOST/HIL | 主动暂停消费直到16块池满，然后恢复 | 不覆盖正在读块；丢弃计数增加并设GAP；下一输出必须先重新预热208样本 |
| T-CAP-05 | HIL | 模拟总线读超时/设备NACK，保留实际板上不可热拔条件 | I2C事务≤10ms返回；200ms无数据判STALE，1秒故障判DISCONNECTED；UI/音频不中断 |
| T-QLT-01 | HOST | 输入静止、低幅噪声、正常振动、饱和1%和0.5%窗口 | 静止10秒触发idle而非断线；1%饱和无机械判定，0.5%按有效规则；所有结果无NaN |
| T-QLT-02 | HOST | 序号跳跃、FIFO overrun、部分块、重复块、时间回退 | gap可见；重复块拒绝；坏窗口不参与基线/连续计数；unknown gap不伪造具体丢样数 |
| T-QLT-03 | HOST/HIL | 活动机械报警期间停采/恢复，持续2秒有效信号 | 无效数据不能自动恢复机械事件；质量恢复后重新积累机械恢复窗口 |
| T-QLT-04 | HOST | 缺基线、安装revision改变、工况不匹配 | NO_BASELINE/BASELINE_MISMATCH；物理数值仍显示，分类和健康百分比不出现 |

数值边界必须覆盖count=0/32/33、int16极值、队列空/满、计数器跨32位、系统UTC倒拨1小时。count超界应拒绝且不访问越界内存。

## 4. BAS / DIA / EXP：基线与诊断

| ID | 层 | 操作与输入 | 通过条件/证据 |
|---|---|---|---|
| T-BAS-01 | HOST/HIL | 健康工况确认，2秒预热后60个有效不重叠窗口 | 约62秒完成采集；至少60有效窗口才允许保存；baseline绑定所有profile/安装/工况键 |
| T-BAS-02 | HOST | 学习期间插入>5秒idle、饱和、120秒超时或RMS组均值比>1.15 | 拒绝保存，原因可见，旧基线保持有效 |
| T-BAS-03 | HIL | 保存基线时在写入/fsync/rename各阶段断电，总20次 | 重启只选择旧或完整新版本；CRC失败不使用；没有半份基线 |
| T-DIA-01 | HOST | 5/10/20/40Hz正弦、双频、脉冲、噪声，固定种子20260906 | C/Python RMS误差≤0.1%，频带能量相对误差≤1%（能量≥1e-6g²），小能量绝对误差≤1e-8g² |
| T-DIA-02 | HOST | 构造偏离序列[5.9,6,6,6]和[10,10,10]，恢复20个<3窗口 | 恰在第3个满足值触发/升级、第20个恢复；5.9/3边界和间断窗口不误计 |
| T-DIA-03 | HOST/HIL | 真实IMU数据源切换及KAIST回放 | LIVE_IMU不调用旧6.4kHz分类器；所有实时异常known_class/confidence为null；回放隔离 |
| T-DIA-04 | HOST | 固定分类概率，max=0.69或margin=0.14 | 返回UNCERTAIN；不能称为开放集准确率测试 |
| T-DIA-05 | HIL | 冻结基线后，同安装/工况健康运行2小时，再做10次可控振动变化 | 健康运行机械报警≤1事件/小时；10次中≥9次检测到偏离，端到端检测≤3秒；仅证明本工况异常监测 |
| T-EXP-01 | HOST/VIS | 固定特征偏离排序及基线曲线 | 最高3项与数值排序一致；曲线同单位同刻度；“类型未确定”与规则阈值完整显示 |

T-DIA-05的10次变化来自同设备、已知可控幅值/安装激励，不人为损坏机器；不能改名成“10种故障识别”。若要现场五分类，需要另立数据集/模型/独立测试门禁，本版本不预设PASS。

## 5. EVT / EVD：报警和留证

| ID | 层 | 操作与输入 | 通过条件/证据 |
|---|---|---|---|
| T-EVT-01 | HOST | 相同异常连续1200个0.5秒输出；第10个输出确认 | 全程一个event_id；确认后所有帧保持ACK；仅允许定义的升级/提醒/恢复消息 |
| T-EVT-02 | HOST | 分别走触发→确认→恢复、触发→恢复→确认 | 四种组合正确；确认不改变ACTIVE，恢复不删除未确认标志 |
| T-EVT-03 | HOST/SIM | 本地/远程同时确认，旧revision、相同command_id重试100次 | 一次durable确认；冲突/重复结果明确；不同正文复用command_id拒绝 |
| T-EVT-04 | HIL | 活动事件时重启并再触发新事件 | 原事件ID恢复，新事件ID不重复；质量预热阶段不自动恢复旧机械事件 |
| T-EVD-01 | HOST/HIL | 固定递增样本序号，在第10秒触发，采集到15秒 | 完整pre=1040、post=1040三轴样本，载荷12480B；触发边界定义为触发输出末样本后一位置，无重复/丢失 |
| T-EVD-02 | HOST/HIL | 触发后2秒断线，或预录仅2秒，或3事件并发 | 实际长度/gap明确；PARTIAL或NO_SLOT；不伪造满10秒文件 |
| T-EVD-03 | HOST | 截断头/元数据/载荷，翻转单字节，超长长度，错误CRC | 全部拒绝为完整证据；无越界，UI提示“文件损坏” |
| T-EVD-04 | HIL | 实时监测中查看/重算证据，正常/异常样本轮换 | 实时采集继续无丢样，回放不产生正式事件/通知，不覆盖实时snapshot |
| T-EVD-05 | HIL | 填满40份不可淘汰证据，再触发 | 新事件正常创建、证据NO_STORAGE、计数可见；旧片段及Wi-Fi资源hash不变 |

## 6. MQTT / 服务端 / 手机

| ID | 层 | 操作与输入 | 通过条件/证据 |
|---|---|---|---|
| T-NET-01 | SIM/HIL | 使用正确证书、错误CA、错误主机名、过期证书、错误密码连接 | 正确连接成功；其余拒绝，不降低TLS校验；TIME_NOT_READY时本地继续监测 |
| T-NET-02 | SIM | 设备A发布/订阅设备B的topic与测试/生产交叉topic | Broker拒绝；错误计数/服务端审计存在，B状态无变化 |
| T-NET-03 | SIM | 对同事件revision重复发送100次，按3/1/2乱序发送 | DB每revision仅一条，current=max revision；确定成功的通知任务不重复创建 |
| T-NET-04 | SIM/HIL | Broker正常但告警服务停机，产生事件；服务恢复 | PUBACK后板子仍待同步；应用receipt后才已同步；业务重试最终落库 |
| T-NET-05 | SIM/HIL | 断Wi-Fi30分钟，产生20事件×3revision，设备中途重启，恢复网络 | 60条revision全部落库去重；2分钟内补齐（正常LAN）；原发生时间不变 |
| T-NET-06 | HOST/HIL | 停服务生成超过128条待发revision | 无内存越界，OUTBOX_FULL/lost_event_revisions可见；采集与本地报警继续；不静默覆盖已有未同步条目 |
| T-NET-07 | SIM/HIL | 发送旧boot/旧challenge/不存在事件/过大JSON/非法UTF8命令 | 全部拒绝，状态不变；合法命令在正常LAN下P95≤2秒完成设备确认（≥30次） |
| T-NET-08 | SIM | 重连后先收到新online，再注入旧connection的LWT | 保持新连接在线；60秒无state才offline；Broker故障显示服务异常 |
| T-NET-09 | SIM/HIL | 16KiB证据分片乱序、重复、丢片、重连、坏CRC | 缺片重传且文件CRC一致，服务端完整落盘后才stored；正常LAN传输≤15秒且不阻塞事件 |
| T-NET-10 | HIL | 200次连接失败/恢复循环，与采集/语音并行 | 重连退避在规格范围，无自旋；FIFO丢样0，音频新增underrun=0，堆无持续增长 |
| T-SRV-01 | SIM | 10模拟设备×1Hz运行1小时，每台生成20事件 | DB/页面事件一致；SSE落库到显示P95≤500ms，API查询P95≤300ms，服务RSS≤512MiB |
| T-SRV-02 | SIM | DB只读/磁盘接近配额/进程COMMIT前后kill | 未COMMIT不回receipt；恢复后不丢已确认提交记录；无ready假阳性 |
| T-SRV-03 | SIM | 创建档案/维修/事件后备份，空环境恢复 | 事件数、revision、证据CRC、权限与档案一致，提供恢复日志 |
| T-NTF-01 | PHONE | 同LAN Android锁屏，发送30个间隔10秒独立报警 | ≥29条在10秒内显示，其余≤30秒或明确失败；记录触发/服务收/渠道接收/手机显示四个时间 |
| T-NTF-02 | SIM/PHONE | 通道返回500/超时、明确成功、成功但响应丢失 | 失败可重试，明确成功不重发；未知送达显式记录DELIVERY_UNKNOWN，不把测试当exactly-once证明 |
| T-NTF-03 | SIM | 补传过去20个已恢复事件 | 一事件一份离线摘要，数据库仍有60条revision；没有40次连续触发/恢复播报 |
| T-NTF-04 | PHONE | 若选择iOS/移动网络，锁屏30次通知 | 独立记录HTTPS可达/APNS上游/权限；未配置为MISSING_EVIDENCE，不能用前台网页代替 |

## 7. 历史、语音、UI

| ID | 层 | 操作与输入 | 通过条件/证据 |
|---|---|---|---|
| T-HIS-01 | HOST/SIM | 生成25小时已知数列，中间断网30分钟/断采2分钟 | 板端保留最新1440分钟；服务端补传去重；缺口/valid_seconds准确，没有零填正常 |
| T-HIS-02 | SIM/VIS | 比较同工况与不同工况的维修前后数据 | 同工况各≥30有效分钟才比较；不匹配给原因，不输出误导百分比 |
| T-VOI-01 | HOST/HIL | status/event/guidance工具各20次，含无基线、过期、未知、回放 | 所有事实字段来自相同snapshot/event；工具P95≤100ms；无虚构五类/RPM/寿命 |
| T-VOI-02 | HIL | 播报开/关各10组，录音/振动并行，断XiaoZhi云 | 播报受音频仲裁，污染窗口不计基线/机械连续数；断云屏幕仍可用，提示语音离线 |
| T-UI-01 | VIS | 800×480六个主页面、抽屉、基线向导、5种场景截图 | 无裁切/重叠/缺字；中文正文≥16px、主触摸区≥44×44，颜色和状态符合04 |
| T-UI-02 | HIL | 200次主导航切换、100次详情进出和确认 | 无对象持续增长；触摸反馈P95≤100ms、切页P95≤200ms；不改变报警状态 |
| T-UI-03 | HIL | 无数据、断线、MQTT离线、存储满、未知异常、恢复未确认 | 状态文案准确；没有“设备正常/数据在线/日志正常”硬编码掩盖错误 |
| T-UI-04 | VIS/HIL | 报警出现时正在基线向导/触摸按钮；处理后返回 | 横幅可见但不劫持正在操作的页面；操作焦点不丢；确认结果跨切页保持 |
| T-UI-05 | VIS | 390×844手机和1440×900网页，键盘与触摸 | 无横向内容溢出；移动版事件详情/确认可用；桌面设备列表可容纳10台 |
| T-UI-06 | PROTOTYPE | 点击HTML六页、异常/断网/缺基线/静止场景、确认、回放和基线向导 | 仅验证视觉和交互样例；不能替代以上HIL，控制台无JS异常 |

## 8. 阶段验收映射

| 阶段 | 必须通过 | 不得用什么代替 |
|---|---|---|
| M0 | T-SYS-01..04 | 主机成功不能代替实机资源/存储 |
| M1 | T-CAP-*、T-QLT-*、T-BAS-*、T-DIA-01..05、T-EVT-*、T-EVD-01..03 | 摇板演示不能冒称五分类诊断 |
| M2 | T-NET-*、T-SRV-*、T-NTF-01..03 | Broker PUBACK不能代替服务端持久化/手机显示 |
| M3 | T-EVD-04..05、T-HIS-*、T-EXP-01、T-UI-01..05 | HTML截图不能代替LVGL实机截图 |
| M4 | T-VOI-*、T-SYS-05..06、全套回归 | 短时smoke不能代替8小时长稳 |

可执行入口计划：scripts/test.ps1保留旧行为并纳入新增host用例；scripts/test-mqtt.ps1启动隔离Broker/SIM；scripts/test-board-v2.ps1收集串口与采集统计；scripts/validate-v2-evidence.py核对ID与文件hash。后两者不得自动写PASS，必须依据观测结果判定。

总报告必须列出没有仪器、手机或硬件条件的项目。V2完成定义是用户选择的板载低频+Android/网页通知范围全部必需项通过；iOS扩展若未选择则报告OUT_OF_SCOPE，若选择但缺依赖则MISSING_EVIDENCE。
