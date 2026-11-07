# Typed 192-byte TX PDO

Enable `CONFIG_ECAT_PAGED_TX=y` for this one-board lab profile. The configured
SM3 image is 192 bytes and uses eight 24-byte entries of object 0x3050.
The host transport uses the logical production capacities for Session processing
and registers the physical carrier with IgH. The paired profile requires a
distinct layout signature and checks the complete physical mapping at SAFEOP.

Every page carries the common 22-byte state header, an 18-byte page header,
one typed payload, and zero padding. CAN0, CAN3, onboard IMU, UART2 and UART5
update independent regions in the host cache. A diagnostic reply can use its
own page. The primary schedule is CAN0, UART2, CAN3, UART2, IMU. UART5 receives
an opportunity at 5 ms intervals and diagnostic requests can take a turn.
The host reads at 125 us. The MCU advances the schedule on each publication.

UART pages offer the oldest unacknowledged record. Each UART2 page can carry a
whole 128-byte stream block and its existing header. Duplicate pages are
idempotent. A change of boot or session clears the cache, and a protection
page clears all data regions.

Build the no-output 历史独立探针（正式化时撤下，源码保存在验收归档） with
`DUST_PAGED_BENCH=1`, the directory containing `paged_transport.hpp`, and the
firmware `src` directory on the include path. Link the production dust_ethercat,
dust_ethercat_pdo and dust_imu libraries. Invoke it as `host_bench 20 paged` or
`host_bench 20 paged-drop`. The latter suppresses all reads of the first
selected UART2 publication while preserving the original WKC checks.
The JSON `tx_bytes` field describes the 410-byte logical cache. The wire
carrier is 192 bytes.

The native [contracts](contracts.cpp) exercise carrier validation, replay,
session changes, mapping mismatch and ordered UART ACK handling. Compile
them with `CONFIG_ECAT_PAGED_TX=1`, the generated PDO header and the domain
protocol, PDO layout, PDO dictionary and LayoutService sources.
