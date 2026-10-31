# EdgeDoctor-RT KAIST 振动模型训练报告

生成日期：2026-09-02  
实验版本：`kaist-vibration-mlp-int8-v1`  
代码：`ai/train/train_kaist_vibration.py`  
结果目录：`artifacts/training/kaist-vibration-v1/`

## 摘要

本实验使用 KAIST Human Lab 发布的旋转机械数据集 v6（DOI
`10.17632/ztmf3m7h5x.6`，CC BY 4.0），只使用振动通道训练五类设备状态分类器：正常、轴承内圈、轴承外圈、不对中、不平衡。模型为 64 维归一化振动频带输入、32 个 ReLU 隐层、5 类输出的 MLP。模型参数已经导出为 C 头文件并随 EdgeDoctor-RT 固件编译进片上 Flash，不需要 SD 卡。

留出记录集结果为 Accuracy 96.50%、Macro-F1 96.44%、Macro-AUC 1.000。导出的静态 Q7 权重量化模型为 Accuracy 97.00%、Macro-F1 96.95%，与 FP32 的预测一致率为 99.5%。按负载留一验证的 Macro-F1 分别为 0 Nm 87.64%、2 Nm 90.44%、4 Nm 77.88%，最低负载折达到方案要求的 75%。

## 数据与许可

KAIST 官方实验室页面列出该旋转机械数据集及其故障类型为轴承故障、转子不平衡和轴系不对中，并链接到 Mendeley 数据页。数据页记录了正常、内圈、外圈、不对中、不平衡和 0/2/4 Nm 负载条件，并标注 CC BY 4.0 许可。

- 官方实验室数据入口：<https://human.kaist.ac.kr/index.php?document_srl=286&mid=sub03_05>
- 固定数据版本：<https://data.mendeley.com/datasets/ztmf3m7h5x/6>
- 数据论文：<https://doi.org/10.1016/j.dib.2023.109049>

仓库内振动压缩包 SHA-256 会写入 `metrics.json`。本次训练发现声学压缩包只有 5 条零负载记录，且与同名振动记录的 Test.Lab 起始时间不一致；因此声学没有被拼接进五类训练，避免制造伪同步样本。该限制与证据保存在 `docs/decision-log.md`。

## 可复现训练流程

```powershell
$env:PYTHONPATH = "$PWD\tools"
python ai\train\train_kaist_vibration.py `
  --data-root data\raw\kaist-v6\vibration `
  --windows-per-recording 40 `
  --epochs 100
```

1. 每条记录均匀抽取 40 个 1 秒窗口；记录 ID 在切窗前划分，禁止窗口泄漏。
2. 25.6 kHz 振动重采样到 6.4 kHz，使用与固件相同的 64 个对数频带和 0 到 100 归一化。
3. E1 按记录分层划分：每类独立保留记录作为验证和测试，其余记录训练。
4. 优化器为 AdamW，学习率 0.003，权重衰减 1e-4，批大小 64，随机种子 20260901。
5. 以验证集 Macro-F1 保存最佳 epoch；同时评估动态 INT8 权重仿真和 0/2/4 Nm 留一负载泛化。

## 指标

| 评估 | Accuracy | Macro-F1 | Macro-AUC |
|---|---:|---:|---:|
| E1 FP32 留出记录 | 96.50% | 96.44% | 1.000 |
| E1 动态 INT8 仿真 | 97.00% | 96.95% | 1.000 |
| E1 静态 Q7 权重导出 | 97.00% | 96.95% | — |
| LOLO 0 Nm | 92.33% | 87.64% | 0.9999 |
| LOLO 2 Nm | 93.50% | 90.44% | 0.9997 |
| LOLO 4 Nm | 82.17% | 77.88% | 0.9747 |

测试集每类 40 个窗口，共 200 个窗口。E1 混淆矩阵中仅有 7 个正常样本被判为不对中或不平衡；四种故障类别均保持 100% 召回。

## 论文风格图表

| 图 | 文件 |
|---|---|
| 训练损失、准确率与 Macro-F1 | `artifacts/training/kaist-vibration-v1/training_curves.png` |
| 归一化混淆矩阵 | `artifacts/training/kaist-vibration-v1/confusion_matrix.png` |
| One-vs-rest ROC | `artifacts/training/kaist-vibration-v1/roc_curves.png` |
| One-vs-rest Precision-Recall | `artifacts/training/kaist-vibration-v1/pr_curves.png` |
| 数据集负载与类别分布 | `artifacts/training/kaist-vibration-v1/dataset_distribution.png` |
| 特征空间 PCA 投影 | `artifacts/training/kaist-vibration-v1/feature_embedding.png` |
| 留一负载与量化对比 | `artifacts/training/kaist-vibration-v1/generalization_quantization.png` |

原始逐轮指标保存在 `history.csv`，记录级划分保存在 `splits.csv`，完整机器可读结果在 `metrics.json`。

## 固件部署

训练脚本生成 `firmware/common/include/edgedoctor_onboard_model_params.h`。标准化已融合进第一层偏置和权重，M55 使用 Q7 权重、INT32 第一层累加、ReLU 和 softmax 完成推理；模型参数从 9492 B 降为 2504 B，位于固件只读区。设备日志固定写入 `/flash/edgedoctor.csv`，不探测、不依赖 SD 卡。

板端使用同一份 `/flash/synthetic_smoke_2s.edgp` 快速回放验证，DWT 周期计数结果如下：

| 后端 | 前处理 P95 | 模型 P95 | 端到端 P95 | 丢帧 |
|---|---:|---:|---:|---:|
| 流式特征 + FP32 MLP | 835 µs | 589 µs | 1424 µs | 0/3 |
| 流式特征 + 静态 Q7 权重 | 846 µs | 125 µs | 967 µs | 0/3 |

当前静态 Q7 版本的第二层激活仍保留 FP32，以确保导出结果与离线评估一致；下一阶段才会引入 CMSIS-NN 的全 INT8 激活和 Ethos-U55 后端。不能将该版本表述为 Ethos-U55 加速。

当前实现是 Reference C 后端，尚未声称 Ethos-U55 覆盖率或 NPU 加速。后续如果要申报 NPU 性能，需要在同一组参数上增加 CMSIS-NN/U55 编译证据，并重新输出实测延迟与覆盖率。

## 复现实验边界

该报告的准确率只代表 KAIST v6 振动记录和本次记录级划分，不等价于现场设备准确率。声学五类同步训练需要补充同一采集时间的完整声学档案；现场部署前应使用目标设备的标注数据做独立校准和验收。
