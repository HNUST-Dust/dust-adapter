# Configured SSC 5.13 for HPM5E3Y

These configured files accompany the HPM5E3Y EtherCAT slave application. The repository
maintainer confirmed that publication is covered by the chip vendor's authorization.
The original Beckhoff copyright and licence notices remain in each source file.
This directory is NOT licensed under the repository's Apache-2.0 licence.

Applicable SSC agreement referenced by the original file headers:
https://www.beckhoff.com/media/downloads/slave-stack-code/ethercat_ssc_license.pdf

`import_hashes.json` records the imported files before this README was added.
No change to the SSC configuration or application behavior is made by adding CI.

## 来源与更新方式

本目录是 Beckhoff SSC 5.13 工具（ET9300 Project Handler）针对本从站工程导出的
配置后源码集合，随仓库提交；构建期不再调用 SSC 工具重新生成。

- `ecat_def.h`、`objdef.c/h`、`eeprom.h` 包含本工程的 SSC 配置、对象字典与 SII
  导出结果（`eeprom.h` 由 ESI XML 生成，`ecat_def.h` 的定义已由 ET9300 Project
  Handler 求值）。
- `ecatslv.c/h`、`ecatappl.c/h`、`ecatcoe.c/h`、`coeappl.c/h`、`mailbox.c/h`、
  `sdoserv.c/h`、`esc.h` 是 SSC 5.13 栈实现文件，随工具工程一起导出，不要手改。
- `sampleappl.c/h` 是 SSC 示例应用；构建时由
  `src/adapters/ethercat_ssc/sampleappl.h` 与仓库自有回调替换，不参与编译。

升级或重新配置 SSC 工程时，必须通过 `tools/import_ssc.py` 受控导入，并更新
`import_hashes.json`；不要直接编辑本目录中的文件。`import_ssc.py` 会在导入后自动
应用项目适配补丁：`ecatslv.h` 的 ESC 硬件头引用改为 `adapters/esc/esc_hw.h`，
`ecat_def.h` 的 mailbox 临界区钩子改为 `esc_enter_critical()` /
`esc_leave_critical()`。
