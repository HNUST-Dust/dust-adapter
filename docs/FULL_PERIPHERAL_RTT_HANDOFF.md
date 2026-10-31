# 交接手册：全外设 RTT 测试（未完成，含调试状态）

> 本手册写给接手的 agent。目标是让一个没有上下文的人能直接从当前状态继续。
> 最后更新：2026-10-30

---

## 1. 目标

把 HPM5E31 板上的**全部外设收发接入**，然后做**全负载 RTT 测试**：

| 外设 | 接线 | 状态 |
|---|---|---|
| CAN2（右轮 0x200、右关节B 0x001、右关节D 0x002） | 已供电 | ✅ 已配置 |
| CAN3（左轮 0x200、左关节B 0x000、左关节D 0x003=DM） | 已供电 | ✅ 已配置 |
| UART2（HI91 921600，RX） | — | ✅ 已配置 |
| UART5（SBUS 100000/8E2，仅 RX） | — | ✅ 已配置 |
| SPI3 板载 IMU（ICM-42688P，CS=PC21、DRDY=PC18） | 未接/驱动不匹配 | ⚠️ 本次已接入代码但**芯片是 ICM-42688P、驱动期望 0x6A（HXY），且 SPI3 无空闲 DMA 通道**，已暂时排除 |

**核心目标**：主站每 10ms 发 6 个电机失能/0力矩帧（只失能，绝不便能），从站同时收 6 路 CAN 反馈 + UART +（未来）IMU，测 DM（CAN3 0x003→0x013）的 RTT。

---

## 2. 环境

### 下位机（从站固件，本 Mac）
- 工作区：`/Users/panpoming/Documents/zephyr_projects/dust-adapter-ws`
- 应用：`dust-adapter/`（本手册的所有路径都相对它）
- 分支：`ethercat`，HEAD=`dd5d789`
- west venv：`/Users/panpoming/Documents/zephyr_projects/.venv/bin/west`
- Zephyr SDK：`/Users/panpoming/Documents/zephyr_projects/zephyr-sdk-0.16.5`
- openocd（带 RTT/HPM 支持）：`/Users/panpoming/Documents/zephyr_projects/openocd-hpm-rtt-v0.1.0/local-rtt/bin/openocd`
- openocd 脚本：`dust-adapter-ws/sdk_env/hpm_sdk/boards/openocd`
- 调试器：Vllink.Basic2（CMSIS-DAP `vid_pid 0x1209 0x6666`，串号 `Vllink.Basic2.3091FD7A76`）

**构建环境变量（必须）**：
```bash
export PATH="/Users/panpoming/Documents/zephyr_projects/.venv/bin:$PATH"
export ZEPHYR_SDK_INSTALL_DIR="/Users/panpoming/Documents/zephyr_projects/zephyr-sdk-0.16.5"
export CCACHE_DIR="/Users/panpoming/Documents/zephyr_projects/dust-adapter-ws/.ccache"  # 沙箱要求，ccache 不能写 ~/Library
```

### 上位机（主站，wbr-nuc 经 SSH）
- SSH：`ssh wbr-nuc`（用户 `wbr-infantry`，免密）
- dust-control 仓库：`~/dust-control`（分支 `dev/wbr_infantry_ethercat`）
- **RTT 台架工具目录：`~/dust-ecat-bench`**（不在 git 里，独立目录）

### 关键硬件事实
- EtherCAT 从站在线：`0 0:0 PREOP + DUST HPM5E31 Gateway LAB`，VID=0、PID=0x44544331、revision=0x10000
- 主站 IgH 1.7.0，I225 网卡（PCI 0000:58:00.0，驱动 ec_igc），ASPM 测试前要临时关闭
- ESC 基地址：`0xF1700000`（AL 状态 0x0130、AL 状态码 0x0134）
- ESC RAM 60 KiB

---

## 3. 当前未提交改动（重要！交接时这些还在工作区）

`git status --short` 显示 5 个未提交文件，其中**有临时代码调试代码**：

| 文件 | 改动 | 性质 |
|---|---|---|
| `CMakeLists.txt` | 加入 `src/drivers/imu_sensor.cpp` | 功能（IMU 接入） |
| `boards/dust-hpm5e31.overlay` | `&spi3 { status = "okay"; }` | 功能（IMU 接入） |
| `prj.conf` | `CONFIG_SPI=y`、`CONFIG_SPI_ASYNC=y`、`CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW=y` | 功能（IMU 接入） |
| `src/ethercat/slave/io_backend.cpp` | IMU 实例化 + Init + CaptureIo 采集 + `limits.imu` | 功能（IMU 接入） |
| `src/ethercat/slave/gateway.cpp` | **`dust_config_diag[6]` 变量 + 多处 printk 诊断** | ⚠️ **临时调试，最终要删** |

> 注意：`gateway.cpp` 里的 `dust_config_diag`、`APPL_*Handler` 里的诊断 printk 和 `dust_config_diag[5] |= ...` 调用标记，都是**调试临时加的**，功能稳定后要清理。

### IMU 接入的设计（io_backend.cpp）
- `#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)` 包裹，`InitializeIo` 里 `onboard_imu.Init()`，`ApplyIoLimits` 里 `limits.imu = imu_ready`，`CaptureIo` 里 `TryTakeCompleted` + `TryStartAsync` 采集（用 `configuration.imu` 门控，避免 `onboard_imu:false` 时误触发 `Publish` 的 Wire 错误）。

### IMU 的两个已知硬件问题（本轮暂时排除 IMU 的原因）
1. **芯片不匹配**：板级 DTS 注释写 ICM-42688P（WHO_AM_I=0x47），但 `imu_sensor.cpp` 的 `kWhoAmIExpected = 0x6A`（HXY）。所以 Init 的 WHO_AM_I 检查必失败。
2. **无空闲 DMA**：SPI3 异步收发（`spi_transceive_cb`）需 RX+TX 两个 DMA 通道，但板子 HDMA 8 通道全被 UART0-3 占用、XDMA 0-5 被 UART4-6 占用、XDMA 6 给 ESC 保留。要接 IMU 必须先腾 DMA（关掉不用的 UART）。

---

## 4. 正在调试的问题（核心阻塞）

**现象**：全外设主站工具跑起来后，从站卡在 **PREOP**（不是 OP），主站 35 秒后 "deadline exceeded、No RTT samples"。

**已确认的定位结论**（用 `dust_config_diag[5]` 调用标记验证）：

1. `APPL_GenerateMapping` **被调用且成功** → 映射 SDO 下载 OK、配置+映射全部有效
2. `APPL_StartInputHandler` **被调用且成功** → `CheckSm()`（SM 地址/长度/模式）通过、`core->Configure` 通过、core 进入 Ready
3. `APPL_StartOutputHandler` **从未被调用** → OP 转换没走到应用层

即：**从站应用层全部校验通过，卡在 SSC（Beckhoff 从站栈）内部的 SAFEOP→OP 状态机**，且 AL 状态码（0x0134）= 0（不是标准错误码）。

**对比**：DM 配置（1 CAN TX + CAN3 RX，SM2=96B/SM3=400B）能到 OP 并正常收 1000 样本；全外设配置（6 CAN TX + CAN2/CAN3 RX + UART，SM2=208B/SM3=896B）卡住。两者用**同一个 `Map` 函数、同一个 ESI、同样 `EC_WD_ENABLE`**。

**最可能的两个 SSC 内部检查**（`external/ssc/ecatslv.c`）：
- `ALSTATUSCODE_INVALIDWDCFG`（SM2 看门狗触发位/时间不一致，约 946 行）
- `ALSTATUSCODE_FREERUNNEEDS3BUFFERMODE`（FreeRun 要求 3-buffer 模式，约 852 行）

**JTAG 读 SM 寄存器发现**：转换失败后 SM2/SM3 被 SSC 复位成 0（起始地址 0、长度 0），而 mailbox SM0/SM1 正常 —— 说明 SSC 在应用回调之后做了某个内部检查并拒绝了 SAFEOP 转换。

---

## 5. 建议的下一步：二分隔离

最快定位是逐步改主站配置（`~/dust-ecat-bench/ethercat_dm_rtt_full.yaml`），依次测：

1. **6 CAN TX + 仅 CAN3 RX**（去掉 CAN2 RX 和 UART）→ 判断是不是 UART/CAN2 引入的
2. 若仍卡，**6 CAN TX + CAN3 RX + 更小 Tx**（反馈槽 8→4，SM3 变小）→ 判断是不是 896B 尺寸边界

> 注意：`~/dust-ecat-bench/ethercat_dm_rtt.cpp` 里的校验被我改成了 `Requires >=6 chassis TX slots` + `Requires CAN2 and CAN3 feedback only`。做第 1 步隔离时要同时把校验改成只要求 CAN3，否则工具会拒配置。

---

## 6. 完整操作手册

### 6.1 构建 + 烧录从站（本 Mac）
```bash
export PATH="/Users/panpoming/Documents/zephyr_projects/.venv/bin:$PATH"
export ZEPHYR_SDK_INSTALL_DIR="/Users/panpoming/Documents/zephyr_projects/zephyr-sdk-0.16.5"
export CCACHE_DIR="/Users/panpoming/Documents/zephyr_projects/dust-adapter-ws/.ccache"
cd /Users/panpoming/Documents/zephyr_projects/dust-adapter-ws
west build -s dust-adapter -b dust-hpm5e31 -d dust-adapter/build/periph_full -p always
west flash -d dust-adapter/build/periph_full
```

### 6.2 主站工具（wbr-nuc，`~/dust-ecat-bench`）
- 主站源码：`ethercat_dm_rtt.cpp`（我改过：校验 + 命令构建发 6 帧）
- 全外设二进制：`dust_ethercat_dm_rtt_full`
- 全外设配置：`ethercat_dm_rtt_full.yaml`
- 重新编译：`g++ -std=c++20 -O2 -I include ethercat_dm_rtt.cpp pdo_config.cpp -o dust_ethercat_dm_rtt_full -lethercat -lyaml-cpp -pthread`
- 跑测试（要 sudo 关 ASPM）：`sudo bash ethercat_rtt_full_probe.sh 1000 <tag>`
- 跑测试（免 sudo，用于快速触发配置下载，但带 ASPM 抖动）：`./dust_ethercat_dm_rtt_full ethercat_dm_rtt_full.yaml /tmp/x.csv 30`

### 6.3 诊断手段

**从站 AL 状态**（wbr-nuc）：
```bash
ethercat slaves          # 看真实状态 PREOP/SAFEOP/OP + 错误标志
ethercat reg_read -p 0 0x0134 2   # AL 状态码（0x0000=无错误）
```

**JTAG 读 RAM 变量**（本 Mac，`dust_config_diag` 符号地址要每次用 nm 重查）：
```bash
# 找符号地址
/Users/panpoming/Documents/zephyr_projects/zephyr-sdk-0.16.5/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-nm -S \
  dust-adapter/build/periph_full/zephyr/zephyr.elf | grep dust_config_diag
# 读 24 字节（6×u32）
./openocd-hpm-rtt-v0.1.0/local-rtt/bin/openocd \
  -s dust-adapter-ws/sdk_env/hpm_sdk/boards/openocd \
  -f probes/cmsis_dap.cfg -f soc/hpm5e00.cfg \
  -c 'cmsis-dap vid_pid 0x1209 0x6666' -c 'cmsis-dap backend usb_bulk' \
  -c 'reset_config trst_only' -c 'adapter speed 500' -c 'init' -c 'halt' \
  -c 'dump_image /tmp/diag.bin 0x<addr> 0x18' -c 'resume' -c 'shutdown'
xxd /tmp/diag.bin
```

**`dust_config_diag` 布局（6×u32 小端）**：
- `[0]`：拒绝标记（1=genmap 拒，2=startinput 拒，3=startoutput 拒，4=SDO write abort）
- `[1][2][3][4]`：错误细节（错误码/索引/字节数）
- `[5]`：调用标记位（bit0=genmap 被调，bit1=startinput 被调，bit2=startoutput 被调）

**JTAG 读 ESC 寄存器**（ESC 基 0xF1700000，`mdw` 读 32 位字）：
```bash
-c 'mdw 0xF1700810 8'   # SM2/SM3 起始地址+长度
-c 'mdw 0xF1700420 2'   # 看门狗时间
```

**RTT 控制台**（`_SEGGER_RTT` 符号，openocd `rtt setup/start/channels`，注意要 `riscv set_mem_access sysbus` + `riscv virt2phys_mode off`）——不太稳定，优先用 JTAG dump RAM 变量。

---

## 7. 关键文件参考

| 路径 | 内容 |
|---|---|
| `dust-adapter/src/ethercat/slave/gateway.cpp` | 网关/对象字典/`APPL_*Handler`（含临时诊断） |
| `dust-adapter/src/ethercat/slave/io_backend.cpp` | CAN/UART/IMU 后端 |
| `dust-adapter/src/ethercat/core.cpp` / `layout.cpp` / `protocol.hpp` | 协议核心（配置/布局/线格式） |
| `dust-adapter/external/ssc/ecatslv.c` | SSC 状态机（`INVALIDWDCFG`/`FREERUNNEEDS3BUFFERMODE`/`StartOutputHandler`） |
| `dust-adapter/config/can_devices.yaml` | CAN 白名单/保护帧（6 电机，编译期） |
| `dust-adapter/docs/ETHERCAT_SSC_GATEWAY.md` | SSC 网关接入边界（SM 分区/三缓冲） |
| `dust-adapter/docs/ETHERCAT_RTT_TRACE.md` | RTT trace 说明 |
| `wbr-nuc:~/dust-ecat-bench/ethercat_dm_rtt.cpp` | 主站 DM RTT 工具源码 |
| `wbr-nuc:~/dust-ecat-bench/pdo_config.cpp` | 主站布局生成（与从站 layout.cpp 对齐） |

---

## 8. 已完成的历史（背景，别重复做）

- **单线程 vs 多线程**：干净 RTT 对比单线程 687µs、多线程 755µs，已切单线程（默认）。
- **late refresh**：默认开启，mean RTT 从 687µs 降到 674µs。
- **周期缩短实验**：125→100µs 只省 23µs 且尾部变差；62/80µs 被 EtherCAT 帧往返 ~65µs 卡死，已回退到 125/250/500/1000。
- **DC Sync0/SM Sync**：实测比 FreeRun 更差（拿延迟换确定性），已放弃。
- 这些历史都已在 git（`dd5d789` 及之前）提交，本手册第 3 节的未提交改动是当前唯一没落地的。

---

## 9. 交接时的必做清理

1. 删掉 `gateway.cpp` 里的 `dust_config_diag` 变量 + 诊断 printk（临时调试）。
2. 决定 IMU 是否保留（当前驱动不匹配 ICM-42688P，且无 DMA 通道，建议本轮先不接 IMU，专注 CAN+UART 打通）。
3. 打通后把未提交改动整理提交（可参考之前的提交风格：一条功能一条 commit）。
