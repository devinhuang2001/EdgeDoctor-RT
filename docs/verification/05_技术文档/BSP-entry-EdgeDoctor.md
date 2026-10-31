# EdgeDoctor BSP 入口说明

生成脚本会调用 `apply-edgedoctor-variant.py` 排除 XiaoZhi 唤醒词、
Edge Impulse 模型、TFLM 和 CMSIS-NN 构建，保留回放 DSP 所需的 CMSIS-DSP。
唤醒词兼容接口明确返回不支持；原版 XiaoZhi 工程不受此副本补丁影响。

本目录是官方 `Edgi_Talk_M55_XiaoZhi` v1.4.0 的 EdgeDoctor 应用集成副本。官方启动、链接、HAL 和 Wi-Fi 资源保持不变；EdgeDoctor 应用代码位于 `applications/edgedoctor` 和 `firmware/common`。

上电后 monitor 会自主启动板载 LSM6DS3TR-C 低频采集。串口可用以下命令查看状态：

```text
edgeimu info
edgeimu sample
edgeimu cal selftest
edmqtt info
edwifi save <ssid> <password>
```

Wi-Fi 凭据需要显式持久化：原厂 `wifi join` 只在本次上电有效，重启后会回到 AP 配置模式。
`edwifi save` 写入 `/flash/wifi_config.json`，板上 Wi-Fi 管理器在下次启动时读取并自动关联。

MQTT 配置一次即可，之后由固件在启动时自动拉起 worker：

```text
edmqtt config <site> <device> <broker_host> <port>
```

`site` 必须与服务端 `EDGEDOCTOR_SITE_ID` 一致，否则服务端下发的命令会发到设备没有订阅的 topic。
设备身份（device id）在启动时从该持久化配置读取，同时用于 MQTT topic 和消息体；两者不一致时服务端会
静默丢弃 events 和 telemetry，因此修改 device 后需要重启生效。

`edgeimu baseline start` 和 `edgeimu baseline commit` 用于建立运行基线；KAIST 五分类只通过 `edgeai replay` 回放路径验证，不与 live IMU 告警混用。

完整代码、验收矩阵和当前硬件证据见 EdgeDoctor-RT 源目录的 `README.md`、`docs/verification-matrix.md` 和 `benchmark/results/`。缺少 STA、TLS、断电或长稳原始证据时，验收状态保持 `MISSING_EVIDENCE`。

从官方 v1.4.0 SDK 生成普通目录副本时运行 `scripts/prepare-bsp.ps1`。脚本会复制板级 `libs`，排除原厂 3D emoji 对象树，并在生成副本中启用 `EDGEDOCTOR_RT` 兼容头；因此生成工程不依赖绝对 symlink/junction。构建时设置 `EDGEDOCTOR_RTT_ROOT`、`EDGEDOCTOR_SDK_ROOT`、`EDGEDOCTOR_LIBRARIES_ROOT` 和 `RTT_EXEC_PATH` 后执行 `scons -j4`。
