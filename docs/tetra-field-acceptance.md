# TETRA 现场验收项目

本文记录 2026-09-20 使用 RTL-SDR 对 862–865 MHz 实际 TETRA 网络进行的现场测试。
验收结果只覆盖已经取得证据的能力；没有发信终端时，不把 SDS 和集群调谐写成完整通过。

## 现场配置

| 项目 | 记录 |
| --- | --- |
| 接收机 | Generic RTL2832U，Rafael Micro R820T |
| 设备序列号 | `77771111153705700` |
| 有效接收中心 | `862.287822 MHz` |
| RTL 基带 | `72 kHz`，TETRA 18 ksym/s 对应整数 4 samples/symbol |
| 采集格式 | CU8，1,152,000 complex samples/s |
| 采集时长 | 约 29.13 秒 |
| 接收丢包 | IQ capture drops 0；input ring drops 0 |
| 解码身份 | MCC 86，MNC 28，CC `0x19` |

现场证据保存在本地 `captures/live/tetra_air_862287822_72k.*`。`captures/` 已加入
`.gitignore`，避免把大体积空口数据误提交到 Git。需要归档时应复制到受控制品存储，
同时保留本文件列出的 SHA-256。

## 验收结果

| 验收项目 | 结果 | 证据或限制 |
| --- | --- | --- |
| RTL-SDR 设备识别与连续接收 | 通过 | RTL2832U/R820T 正常打开并持续采样 |
| 862–865 MHz TETRA 信号定位 | 通过 | 多条约 20 kHz 载波被检出 |
| TETRA 空口同步 | 通过 | 修正为 72 kHz 基带后稳定解出正、负极性 NDB/SB |
| BSCH 解码 | 通过 | 29 秒日志中 1,348 条 BSCH；MCC 86、MNC 28、CC `0x19` |
| SCH-HD 与 TCH/FS 接收 | 通过 | 同一采集中持续出现 SCH-HD，记录 266 条 TCH/FS |
| 原始 IQ 留证与离线兼容性 | 通过 | 64 MiB CU8；`--iq-info` 报告 replay compatible、零丢包 |
| SDS 协议解析自动化测试 | 通过 | 独立参考向量、分片重组及文本解析测试已纳入 TETRA 测试套件 |
| 真实 SDS 空口报文 | 条件通过 | 本次网络采集中未出现 SDS；缺少可入网发信终端，待补测 |
| CC→VC→CC 真机集群调谐 | 待补测 | 未收到可用于调谐的 CHANNEL-ALLOCATION，尚无真实重调谐时间线 |
| 真实语音业务 | 部分通过 | 已收到 TCH/FS；没有受控终端和完整 CC→VC→CC 证据 |

## 证据摘要

| 文件 | SHA-256 |
| --- | --- |
| `tetra_air_862287822_72k.iq` | `9300397DD3261AC07B578A0F4C82BD8A775B7A0DF6166C24AE2F3983932BFD92` |
| `tetra_air_862287822_72k.iq.json` | `1B2CDCDF6BF88537AAF011D727BC86EE8CB19FFFB3288C1B8D2D7EAA4A1E4D5F` |
| `tetra_air_862287822_72k.log` | `558036FD37A1FB3BE88BE37F6BA4E6F60D1EE71D22376B27819504C23E56C5A8` |

日志统计：BSCH 1,348 条，TCH/FS 266 条，SYSINFO 0 条，SDS 0 条，
CHANNEL-ALLOCATION 0 条。因此阶段 4 的 SDS 真实空口门槛和阶段 5 的真机集群调谐门槛仍然开放。

## 补充验收方法

真实 SDS 可通过以下任一方式补测：借用已入网 TETRA 终端、由调度台发送测试短消息、
由网络维护方安排测试，或取得来源与授权清晰的原始 SDS IQ 录音。集群调谐验收需要在
控制信道监听期间产生一次语音呼叫，并保留按顺序出现的 BSCH、CHANNEL-ALLOCATION、
TCH/FS、返回控制信道 BSCH 以及 CC→VC→CC 重调谐事件。

## 软件修正

现场测试发现 48 kHz RTL 基带对应 TETRA 18 ksym/s 时只有 2.667 samples/symbol，运行时取整
会导致不稳定同步。RTL 输入、配置和 CLI 现支持 72 kHz 基带，使 TETRA 使用整数 4
samples/symbol。修正后的现场采集从偶发同步变为连续 BSCH/TCH 解码。
