# 蜂鸣器硬件检查

独立测试固件在 PWM0 P1（PF01）以 2 kHz、50% 占空比短鸣 100 ms，每 2 秒重复。启动失败输出到 SEGGER RTT。使用前确保 EtherCAT 主站不活动；测试结束后恢复网关固件。

```sh
export PATH=/Users/panpoming/Documents/zephyr_projects/.venv/bin:$PATH
export ZEPHYR_SDK_INSTALL_DIR=/Users/panpoming/Documents/zephyr_projects/zephyr-sdk-0.16.5
west build -p always -s dust-adapter/tests/board_buzzer_probe -b dust-hpm5e31 -d dust-adapter/build/board_buzzer_probe
west flash --skip-rebuild -d dust-adapter/build/board_buzzer_probe
```

网关启动时仅初始化为静音，`dust_buzzer_beep()` 可异步触发 1–2000 ms 的单次短鸣；目前没有绑定自动报警条件。`dust_buzzer_stop()` 可提前停止，API 在普通线程调用。失败不改变网关保护状态。

PWM0 的周期计数器由各通道共用。板载蜂鸣器和加热器统一配置为 2 kHz，分别控制占空比，访问 PWM0 的寄存器更新由共用互斥锁串行化；硬件端的波形和加热效果尚未验证。该离线构建未证明实际频率、音量或硬件连接。
