# DTG1 正式节点接入与实机测试（2026-11-07）

## 结果

真实 DTG1 控制链路已接入上位机正式工程 `/home/wbr-infantry/dust-control`，构建并运行 `build/production/dust_control_node`；本轮使用原有 ChassisRuntime/ChassisController，不是先前的 controller-runner 测试程序。下位机继续使用已验证的 DTG1 固件，未重新刷写。

现场确认：机器人可正常动作，延迟没有大到影响控制效果。使用者随后明确表示已经遥控失能，要求结束并归档。节点收到 SIGINT 后退出，记录器已关闭，EtherCAT 从站最终为 PREOP。

| 项目 | 结果 |
|---|---:|
| 遥测观测总跨度 | 186.034 s |
| 记录的遥测消息 | 179,918 |
| DM Arming | 0.300 s |
| Recovery | 0.646 s |
| 连续观测到的 Balance 段 | 96.182 s |
| 使能阶段记录中反馈无效 / IMU过期 / 遥控过期 | 0 / 0 / 0 |
| 控制累计漏周期 / 观测生产队列丢样 | 0 / 0 |
| 使能阶段控制计算最小 / p50 / p99 / 最大 | 1.430 / 4.672 / 21.299 / 40.060 μs |

ROS发布器消费控制观测后发布最新样本，订阅端采用 best-effort，因此179,918条消息不是所有控制周期的逐周期完整抓取；上表的反馈有效性是记录中实际观察的结果，累计漏周期和生产队列丢样来自控制器自身累计计数。它不是端到端RTT统计，也不是电机原始到达间隔统计。状态依次为 Disabled → DmArming → Recovery → Balance → Disabled，记录中没有保护状态。

## 正式接入

- 新增显式 `protocol: dtg1` 配置。保留原generic协议路径，DTG1固定60 B下行/180 B上行、125 μs，布局签名0x44544731，右CAN0、左CAN3。
- 正式会话完成 Discover/Arm/Stop、会话token检查、命令租约、六电机路由校验、完整六命令提交及重复序列抑制。
- 六电机真实反馈直达现有MotorFeedbackDecoder；HI91紧凑字段恢复为现有解码器输入；SBUS保留块独立ACK，板载IMU保留加速度、角速度、温度。
- HI91与SBUS共用最后60 B传感器页；板载IMU位于中间页前28 B。三份线协议头与MCU源码相同，架构检查仅对这些共用头及既有生成schema保留原命名空间。
- 控制线程CPU2/FIFO70，EtherCAT线程CPU3/FIFO80。正式ChassisRuntime在DTG1模式补全每周期六电机命令：失能/故障时关节Disable、轮电流零；工作状态下空目标使用编码器生成的零力矩，保留Enable/ClearError等原有时序。
- 测试程序专用的SIGUSR1门控、起始±20°、摇杆中位限制、跳跃/爬楼/腿长额外禁令、2 ms决策过期即退出和5/30秒时间截止均没有带入正式节点。正式控制器原有倾倒、超时、状态锁存、遥控失能及20 ms链路看门狗保留。
- 算法、校准、增益、Recovery、限幅没有修改：关节54 N·m，轮约4.749 N·m。此前某轮fault=6缺少触发帧，无法事后确定具体原因；没有将其认定为误保护，也未以关闭原控制器保护来绕过。

默认[主机配置](https://github.com/HNUST-Dust/dust-control/blob/dev/wbr_infantry_ethercat/config/dust_control.yaml)保持 `enable_output: false`。此次显式实机配置在 `/tmp/dust-production-live-config`，仅把enable_output设为true，其余标定来自正式配置；其完整副本已归档。没有添加自动启动服务，没有提交或打Git标签。

## 验证

正式生产构建通过（加载 `/opt/ros/humble/setup.bash`）。离线构建 `build/dtg1-tests` 的13项CTest全部通过，包含新DTG1握手/固定映射/六命令/ACK/过期拒绝/Stop契约和既有控制、设备解码、并发、配置、架构回归。正式ROS节点10秒零输出实机预检通过。实机运行使用独立ROS订阅记录器，未把旧测试探针链接进正式节点。

SIGINT清理后日志中现有 `Adapter base: EtherCAT master stopped` 是关闭会话时的清理提示；它出现在明确的SIGINT和控制线程停止之后，不是此次运行中的保护触发。

## 文件

- [最终统计](production-live-01-final-summary.json)
- [完整遥测（gzip JSONL）](production-live-01.jsonl.gz)
- [正式节点完整日志](production-live-01.log)
- [零输出预检日志](production-preflight.log)
- [13项回归日志](production-ctest.log)
- [对应正式工程源码（包含新文件）](dust-control-dtg1-formal-source.tgz)
- [接入前源码备份](dust-control-before-dtg1.tgz)
- [此次实机配置](dust-production-live-config.tgz)

对应节点SHA256：`d0848d6c3ce4243032157a6790f47f0f7c4dadeb490aac212cdda5a34bca875b`。
