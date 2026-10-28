# dust-adapter — EtherCAT CAN/UART 网关

`ethercat` 分支在 generic USB 固件基础上替换通讯后端，控制算法仍在主机。
固件统一由根目录 `CMakeLists.txt`、`prj.conf` 和 `boards/` 构建，全部应用源码位于
`src/`，不再使用独立 `apps` 工程。旧 USB 模块保留供协议测试和驱动实现参考，不参与当前固件链接；USB 固件请使用原 USB 分支。

## 构建

在已经初始化的 West 工作区中：

```bash
cd dust-adapter
west build -b dust-hpm5e31 -p always
# 或使用脚本，默认输出 build/ethercat_slave：
./scripts/build.sh ethercat -p always
./scripts/test_protocol.sh
```

从旧应用目录迁移时必须使用 `-p always` 或新的构建目录，不能复用旧 CMake 应用路径。
可用 `BUILD_DIR` 指定脚本输出目录、`WEST` 指定 west 可执行文件。
CAN YAML 使用 `-DDUST_ADAPTER_CAN_CONFIG=/绝对路径/config.yaml` 指定。
动态 PDO 的容量与端口仍由主机 YAML 配置；本次整理不改变协议。

当前使用 HPM5E31 片上 ESC、两颗外置 PHY、SSC 5.13、动态 PDO、CAN/UART 后端和本地保护。
SPI IMU 与加热暂时关闭。实验 VID=0 限制保留，编译通过不代表实板验收。
SSC 按原许可保留，见 [SSC 许可说明](external/ssc/README.md)。

源码职责和线程分析见 [源码组织](docs/SOURCE_LAYOUT.md)。
板级接线见 [HPM5E31 板级说明](docs/HPM5E31_BOARD.md)，
协议接入见 [SSC 网关](docs/ETHERCAT_SSC_GATEWAY.md)，
CAN 白名单与保护配置见 [CAN YAML 配置](config/README.md)，
性能记录见 [RTT 时间戳](docs/ETHERCAT_RTT_TRACE.md)。
