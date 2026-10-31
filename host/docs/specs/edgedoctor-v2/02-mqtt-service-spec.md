# EdgeDoctor V2 MQTT 与服务端规格

版本0.1，2026-09-06；设计目标，尚未部署。需求NET-01（连接）、NET-02（可靠事件）、NET-03（远程确认）、NET-04（证据）、SRV-01（服务端）、NTF-01（手机通知）。

## 1. 技术选型和部署范围

- 板端：MQTT 3.1.1 over TLS 1.2，复用现有SAL/lwIP/mbedTLS。采用Eclipse Paho embedded-c的MQTTClient-C与MQTTPacket，新增RT-Thread Network/Timer适配。单独连接，不复用XiaoZhi WebSocket，也不改Wi-Fi资源。
- 客户端依赖放在应用第三方目录，以固定commit与SHA-256记录到dependency-lock.json；M0选择并验证该commit后冻结，禁止引用浮动master。此时没有安装新库，不能虚构锁定hash。
- 服务端：Docker Compose编排Mosquitto 2.x、Python 3.12告警服务（FastAPI、Paho Python 2.x）、SQLite WAL、静态网页、ntfy 2.x。具体补丁版本和镜像digest在M2首次部署时锁定，不能使用latest进入验收。
- 默认部署在一台局域网电脑，支持1台实机和10台模拟设备；单实例数据库，无HA承诺。Windows可使用Docker Desktop/WSL2；没有容器运行时则先安装或在局域网Linux主机部署，不假设当前已存在。
- 手机同局域网可访问响应式网页。Android ntfy后台通知是默认实机推送验收路线；iOS即时后台推送为单独部署门禁，详见第8节。

## 2. 连接和安全配置

Broker端口8883；设备唯一用户名=device_id，单设备密码；校验CA、证书有效期和主机名，不允许verify=false。TLS时钟由首次串口配置的UTC/RTC建立，若证书时间无法验证则显示TIME_NOT_READY并保持离线监测；不能通过忽略证书时间解决。首次可信校时后进行受限范围校时，记录跳变，不改历史事件时间。

Broker关闭匿名；设备只可发布自己的上行topic、订阅自己的下行topic。服务端账号按角色授予跨设备读取/命令发布。网页不持有Broker密码；网页通过HTTPS服务端会话操作，写操作要求身份与CSRF保护。开发机本机HTTP只用于回环测试，手机联网部署使用受信任HTTPS证书。

板端MQTT参数：client_id=ed-{device_id}；CleanSession=1；KeepAlive=30秒；连接超时5秒、TLS握手10秒、普通socket读片100 ms/写超时2秒、命令等待超时3秒。所有等待只发生在网络线程。每次连接都重订阅；不依赖Broker离线缓存命令。

重连退避1、2、4、8、16、32、60秒，上下20%抖动，稳定在线60秒归零。配置错误/证书错误每60秒重试且显示明确原因。不能在Wi-Fi未ready时自旋连接。

固定TX/RX各4096 B，topic≤160 B、JSON≤2048 B、最大4个QoS1待确认发布。控制/事件优先于证据，证据默认限速4 KiB/s且每批最多一个分片，不能饿死PING/下行命令。

## 3. Topic合同

前缀P=`edgedoctor/v1/{site_id}/{device_id}`；标识符只允许[a-z0-9_-]、最多32字节；event_id采用连字符连接，不包含斜杠。

| Topic（P之后） | 方向 | QoS | Retain | 语义 |
|---|---|---:|---|---|
| /availability | 上 | 1 | 是 | CONNECT后online；LWT为offline，均含boot_id/connection_id |
| /state | 上 | 1 | 是 | 每10秒/重大变化，设备当前快照摘要、采集状态、当前活动事件、命令challenge |
| /telemetry | 上 | 0 | 否 | 每1秒，物理量/基线偏离；不补传逐秒遥测 |
| /events | 上 | 1 | 否 | 事件触发/升级/确认/恢复revision，需应用层回执 |
| /event_receipts | 下 | 1 | 否 | 服务端落库后回执，设备据此完成同步 |
| /commands | 下 | 1 | 否 | 远程ack_event、request_evidence；V2无停机/执行shell/远程重建基线 |
| /command_results | 上 | 1 | 否 | applied/duplicate/conflict/rejected及当前revision |
| /trend_batches | 上 | 1 | 否 | 补传最多10个分钟记录/包，总包≤2048B |
| /trend_receipts | 下 | 1 | 否 | 已提交的批次ID，设备据此推进游标 |
| /evidence/{event_id}/manifest | 上 | 1 | 否 | 长度、样本元信息、文件CRC、chunk_count |
| /evidence/{event_id}/chunks | 上 | 1 | 否 | EDV2文件二进制分片 |
| /evidence_receipts | 下 | 1 | 否 | 缺失分片或完整文件已校验并落盘 |

测试前缀为`edgedoctor-test/v1/...`，独立ACL与通知接收端。DEMO/KAIST_REPLAY/EVIDENCE_REPLAY不得进入正式events；实时遥测的source仍必须校验。Broker retained只用于当前状态，不作事件历史库。

## 4. Payload例子及字段规范

整数时间统一毫秒，板内高精度单调时钟仍使用µs；浮点有限数最多4位小数，未知值使用null，不用0或NaN冒充。UTC ISO8601仅用于UI，协议utc_ms为Unix毫秒/null。

事件例子（示例值，不是当前设备实测）：

```json
{
  "schema_version": 1,
  "device_id": "ed84-001",
  "asset_id": "motor-01",
  "boot_id": "b000042",
  "event_id": "ed84-001-b000042-e000017",
  "revision": 1,
  "source": "LIVE_IMU",
  "kind": "BASELINE_DEVIATION",
  "condition": "ACTIVE",
  "ack": "UNACKNOWLEDGED",
  "severity": "ALARM",
  "occurred_mono_ms": 384250,
  "occurred_utc_ms": null,
  "profile_id": "imu_lf_208_v1",
  "baseline_id": "motor-01-b03",
  "policy_version": 1,
  "quality": "VALID",
  "diagnosis": "UNKNOWN_ANOMALY",
  "known_class": null,
  "confidence": null,
  "rms_vector_g": 0.0842,
  "baseline_deviation": 11.3,
  "evidence": {"status": "CAPTURING", "pre_ms": 5000, "post_ms": 5000}
}
```

之后证据完成通过独立manifest更新，不为了上传进度重复触发用户告警。事件revision包含ack_actor、ack_mono_ms、recovered_mono_ms等可选字段，保留首次发生时间，禁止重连时改成新发生。

state公共字段：schema_version、device_id、asset_id、boot_id、connection_id、state_seq、source、utc_ms/null、mono_ms、sample_age_ms、sensor_profile、quality、idle、baseline_status、diagnosis、rms_vector_g、baseline_deviation/null、active_event_summaries[≤8]、mqtt_state、outbox_pending、lost_event_revisions、storage_status、model_profile、command_challenge。不得包含密码/证书私钥/原始声音。

telemetry采用同一语义子集，含rms_xyz_g[3]、peak_g、crest_factor、effective_hz、gap_known、gap_unknown、quality。UI根据服务端接收时间判断在线，不能仅信任设备UTC。

## 5. NET-02：应用持久化回执、顺序和重试

事件按“内存状态变化 → 存储线程durable journal提交 → 网络发送”推进；本地屏幕可先显示“正在保存”，落盘失败明确显示“未持久化”。MQTT PUBACK只代表该协议交付，不能删除待发业务记录。

服务端收到合法event后，在一个SQLite事务中INSERT event_revisions唯一键(device_id,event_id,revision)、更新events当前态（仅更大revision覆盖）、追加通知任务。COMMIT成功才发布：

```json
{"schema_version":1,"event_id":"ed84-001-b000042-e000017","revision":1,"status":"persisted","server_received_utc_ms":1788660000000}
```

设备收到匹配回执后原子推进该条sync状态。回执丢失则同ID/revision重发；服务端重复插入不再生成通知，但仍重发receipt。收到乱序旧revision只补历史，不将RECOVERED回退ACTIVE。不能用只保存最后revision的方式漏掉恢复/确认历史。

业务回执等待5秒，按1/2/4/8/16/30秒退避重试，连接不可用时暂停重试并保留记录。重连先发state，再按本地事件序号补传。容量上限128条revision；上限内应无静默丢失，超限按01的OUTBOX_FULL规则报告，不能宣称无限离线缓存。

状态明确区分：LOCAL_PENDING、LOCAL_DURABLE、BROKER_ACKED、SERVER_PERSISTED；手机渠道另有NOTIFY_PENDING、CHANNEL_ACCEPTED、NOTIFY_FAILED；人工确认另有ACKNOWLEDGED。CHANNEL_ACCEPTED不等于用户看到，设备UI主状态只简化为“待同步/已同步/同步异常”，详情页解释。

设备软重启后重建待发journal；MQTT packet_id可重新分配，业务唯一键不变。应用级有序/去重是依赖的保障，不依赖客户端自动持久化QoS会话。

## 6. NET-03：远程确认

网页POST确认 → 服务端存commands → publish commands → 设备校验并durable更新事件 → publish command_results与events新revision → 服务端展示成功。网页先显示“待设备确认”，不能HTTP 202就把报警变成已确认。单次30秒未获设备回复显示超时，允许重试相同command_id。

```json
{
  "schema_version": 1,
  "command_id": "cmd-8fdd6a11",
  "action": "ack_event",
  "target_boot_id": "b000042",
  "challenge_nonce": "6f624fa6dcf143a6",
  "event_id": "ed84-001-b000042-e000017",
  "expected_revision": 1,
  "actor": "operator-01"
}
```

设备每30秒生成随机challenge并上报state，记录本地发行单调时间。仅接受当前challenge且年龄≤30秒、boot匹配的命令，避免UTC未同步时依赖错误时间判断TTL；重连立即换challenge，旧命令失效。随机数来自平台已验证RNG，初始化失败禁用远程命令。

保留最近64个command_id/result在持久环中；重复请求返回原结果，不再次更新。未知字段按schema规则拒绝关键行为扩展；越界payload/非UTF8/无效event_id/ACL越权/错误source返回拒绝并计数。相同command_id不同正文返回CONFLICT。远程确认不能应用于DEMO/回放事件，除非处于独立测试namespace。

冲突规则：本地先确认则后续远程返回ALREADY_ACKNOWLEDGED；先恢复未确认则必须用新revision确认恢复事件；每次设备重启后先等待新challenge，不重放旧指令。设备状态权威，服务端仅补充接收时间、通知结果与操作者信息。

## 7. NET-04：证据上传和服务端API

证据仅在事件后或操作者请求时上传，正常持续原始流不上传。manifest提供transfer_id、event_id、file_bytes≤16384、file_crc32、chunk_bytes=1024、chunk_count=ceil(file_bytes/1024)。二进制包头20字节：magic="EVCH"(4)、transfer_id_u32、chunk_index_u16、payload_len_u16、file_crc32_u32、chunk_crc32_u32；随后≤1024字节载荷，小端。

服务端随机顺序写入临时文件，校验每片与完整EDV2；相同片幂等，冲突片拒绝。收到manifest+任意片后，每5秒或接收末片回evidence_receipts，列missing_indices[≤16]；全部校验并fsync+rename后才给status=stored。设备重连先重发manifest，服务端回复missing_indices，不能从零无限重复。

SQLite表：devices、assets、baselines、event_revisions、events、commands、notification_jobs、telemetry、minute_trends、maintenance、evidence、audit_log。event_revisions唯一键(device_id,event_id,revision)；commands唯一command_id；minute_trends唯一(device_id,boot_id,period_seq)；notification_jobs唯一(event_id,notification_kind,revision)。schema migration带版本，不直接覆盖数据库。

| HTTP API | 行为 |
|---|---|
| GET /api/v1/devices | 在线/离线、质量、活动事件，最多10设备分页 |
| GET /api/v1/assets/{id} | 档案、基线、维修记录 |
| GET /api/v1/events?asset_id=&cursor=&limit= | 按服务端接收时间和ID稳定分页，limit≤50 |
| GET /api/v1/events/{id} | 所有revision、留证和通知轨迹 |
| POST /api/v1/events/{id}/ack | expected_revision必填，返回202+command_id或409冲突 |
| GET /api/v1/commands/{id} | pending/applied/rejected/timeout |
| POST /api/v1/events/{id}/evidence-request | 创建带challenge的请求，返回202 |
| GET /api/v1/evidence/{id}/download | 已校验文件，权限验证，流式下载 |
| GET /api/v1/trends?asset_id=&from=&to=&resolution= | 1s/1m/1h，最多2000点，缺口标明 |
| POST /api/v1/assets/{id}/maintenance | 时间/备注，追加审计，不清故障 |
| GET /api/v1/stream | SSE推送状态和事件，Last-Event-ID可恢复 |
| GET /health/live、/health/ready | 进程活性与DB/Broker/存储可用性 |

服务端默认磁盘配额2 GiB；剩余<10%时停止接收新证据并报警，元数据优先。状态/事件ingest后前台SSE P95≤500 ms（局域网环境）。服务端的离线判定是60秒未收到心跳/state；同connection_id的LWT可加快判定，旧连接LWT不得覆盖新连接online。Broker自身不可达时看板显示“服务连接异常”，不能把10台设备全部误判机械故障。

## 8. NTF-01：手机通知

默认安装自托管ntfy，告警服务以独立token调用其发布API；手机按设备或站点订阅受保护topic。标题示例：“motor-01：振动偏离基线”；内容带首次发生时间或“设备开机后384秒”、严重度、状态、事件短ID，以及网页详情链接。LIVE_IMU不写“轴承外圈故障92%”。

通知策略：首次WARNING/ALARM、升级、恢复各一次；确认默认只更新详情，不群发；未确认每10分钟提醒一次。补传事件已恢复时合并为“离线期间发生并恢复”，不连续播放过去的触发/恢复风暴；数据库仍保留完整轨迹。服务启动后30秒积累重连补传，再对同事件选择摘要通知，实时新事件不受该缓冲拖延。

发布失败1/2/4/8/16/30/60秒退避，最大24小时；到期标记失败，不删除事件。渠道若没有幂等API，响应丢失时可能重复手机通知：payload始终带event_id，服务端避免已确定成功的重复发布；此类传输不确定性记录为DELIVERY_UNKNOWN，不承诺物理推送exactly-once。

Android：验收同Wi-Fi、ntfy应用后台连接获系统允许时的锁屏通知；通知授权/省电限制记录在测试前提中。网页前台告警页是无App的备用显示。

iOS：纯局域网不保证锁屏即时推送。按ntfy官方要求，需可达HTTPS服务与配置APNS上游（如ntfy.sh）；手机需能从所在网络读取本地服务，可通过用户选择的安全远程接入部署。未配置该路径则标记MISSING_EVIDENCE，不以浏览器前台弹窗代替锁屏验收。外出移动网络接收同样需远程可达路径，不默认开放公网端口。

## 9. 文件和运维交付

新增server/compose.yaml、mosquitto.conf.template、app/config.py、mqtt_ingest.py、event_store.py、commands.py、notify.py、api.py、schemas.py、migrations/、web/；scripts/server-up.ps1、server-backup.ps1、mqtt-provision.ps1、mqtt-simulate.py。

提供.env.example不含真实密码；初始化脚本生成站点ID、设备凭据、CA与局域网DNS/IP证书SAN，实际密钥放ignored secrets目录，权限只给运行账号。首次UI创建管理员，持久会话HttpOnly/SameSite，状态写操作审计。板端credentials不通过遥测/串口info回显。

备份使用SQLite在线备份接口与证据目录manifest，恢复后检查CRC/外键/事件计数。提供卸载仅针对本项目Compose和卷的明确命令说明，不删除共用Docker数据。M2交付部署自检报告、依赖锁、端口表和一台手机的真实通知证据。

## 10. 参考

- [MQTT 3.1.1标准](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html)：QoS1可重复、会话/retained/Will语义。
- [Paho Embedded C](https://github.com/eclipse-paho/paho.mqtt.embedded-c)：MQTTClient-C与平台网络适配选择。
- [Mosquitto配置](https://mosquitto.org/man/mosquitto-conf-5.html)：TLS、认证、ACL和Broker持久化选项。
- [ntfy配置](https://docs.ntfy.sh/config/#ios-instant-notifications)：自托管iOS即时通知的上游与网络条件。
