# Aevum 文档

文档索引。真正重要的入口在仓库根目录，这里只做导航。

## 先读这几份

| 文档 | 内容 |
| --- | --- |
| [../README.md](../README.md) | 项目是什么，代码在哪张表里 |
| [../PROGRESS.md](../PROGRESS.md) | 真实进度与当前缺口 |
| [../ROADMAP.md](../ROADMAP.md) | 阶段划分与下一步 |
| [../start_here.md](../start_here.md) | 新人入口，三条路 |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | 提交规范与代码质量要求 |
| [../CHANGELOG.md](../CHANGELOG.md) | 版本变更 |
| [../interface/web/README.md](../interface/web/README.md) | 社区平台本体 |
| [architecture/overview.md](architecture/overview.md) | 运行面、分层与边界 |
| [architecture/code-map.md](architecture/code-map.md) | 入口、领域模块与拆分规则 |
| [audit/2026-11-18-code-audit.md](audit/2026-11-18-code-audit.md) | 本轮全仓审计、指标与风险 |

## 机器人控制

| 文档 | 内容 |
| --- | --- |
| [design/robot-control-philosophy.md](design/robot-control-philosophy.md) | 身体自描述、几何能力与参数化动作的设计哲学和工程经验 |
| [design/toy-vehicle.md](design/toy-vehicle.md) | 车辆与 3R 机械臂的 glTF 模型、Forge 通用模型查看、内核在环 3D 联调与硬件顺序 |

## 目录说明

| 目录 | 存放 |
| --- | --- |
| `api/` | 各模块对外接口说明 |
| `architecture/` | 架构总览、代码地图与三模块设计 |
| `adr/` | 架构决策记录，一篇一个决定 |
| `audit/` | 可复算的代码审计报告 |
| `guides/` | 安装、开发、测试、部署的操作步骤 |
| `operations/` | 运维手册 |
| `security/` | 安全说明与基线 |
| `release/` | 发布流程 |
| `tutorials/` | 快速开始与示例 |
| `design/` | 设计稿与决策记录 |
| `faq/` | 常见问题 |

## 状态

页面文档已全部补齐，不再保留 0 字节占位。指标口径和最新数值见 `audit/2026-11-18-code-audit.md`，复算命令是 `python ops/tools/analyze.py`。
