# EdgeDoctor BSP 入口说明

本目录是官方 `Edgi_Talk_M55_XiaoZhi` v1.4.0 的 EdgeDoctor 应用集成副本。官方启动、链接、HAL 和 Wi-Fi 资源保持不变；EdgeDoctor 应用代码位于 `applications/edgedoctor` 和 `firmware/common`。

上电后 monitor 会自主启动板载 LSM6DS3TR-C 低频采集。串口可用以下命令查看状态：

```text
edgeimu info
edgeimu sample
edgeimu cal selftest
edmqtt info
```

MQTT 需要先配置站点、设备、broker，再启动 worker：

```text
edmqtt config <site> <device> <broker_host> <port>
edmqtt start
```

`edgeimu baseline start` 和 `edgeimu baseline commit` 用于建立运行基线；KAIST 五分类只通过 `edgeai replay` 回放路径验证，不与 live IMU 告警混用。

完整代码、验收矩阵和当前硬件证据见 EdgeDoctor-RT 源目录的 `README.md`、`docs/verification-matrix.md` 和 `benchmark/results/`。缺少 STA、TLS、断电或长稳原始证据时，验收状态保持 `MISSING_EVIDENCE`。
