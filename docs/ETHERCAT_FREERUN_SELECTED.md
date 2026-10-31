# Selected runtime: FreeRun (2026-10-30)

User selected FreeRun instead of continuing DC latency optimization. Default
prj.conf now explicitly disables SM/DC synchronized publication, RTT tracing,
worker scheduling statistics and periodic raw PHY diagnostics. Use original
licensed external/ssc. The unused DC-generated SSC copy has been removed; CMake rejects DC-enabled SSC sources.

Keep aligned word PDRAM access, event-driven CAN worker, table digest and
byte-equivalent skipped overwrite clearing. Do not enable unproven late
refresh/coalescing/fast-notice or fixed host receive-offset experiments. Normal
link checks, fault protection and state-transition messages remain. Periodic
raw PHY diagnostics are now explicitly opt-in in FreeRun as well as DC. DC
experiments and archived measurements remain available but are not selected.

CAN3-only bench: config/can_bench_dm_rtt.yaml, ID3 disable only, feedback0x13.
Other CAN buses have no configured motor devices. This is a bench image, not
a production multi-motor configuration. Fresh build: build/dm_rtt_freerun;
saved artifacts: build/bench-host/freerun-return-01.

Build with west build -b dust-hpm5e31 -d build/dm_rtt_freerun --
-DDUST_ADAPTER_SSC_DIR=<repo>/external/ssc
-DDUST_ADAPTER_CAN_CONFIG=<repo>/config/can_bench_dm_rtt.yaml

Actual PHY tests pass in quiet/raw FreeRun and DC, software publication byte
equivalence tests pass, real SSC/ESI contracts pass, target build passes.
ROM174988B, RAM211024B. Live FreeRun regression remains required.

NUC command: sudo bash ~/dust-ecat-bench/ethercat_rtt_aspm_probe.sh 125 2000 freerun-return-01

Uses original FreeRun master executable, normal receive/process/send and no
receive offset. Historical125us FreeRun bus cadence is not a125us worst-case
firmware execution guarantee and not a same-period comparison against500us DC.
After master exit export boot RTT, bEscIntEnabled/bDcSyncActive, SyncManager
configuration and fault evidence with the saved matching ELF before reflashing.

Idle periodic PHY register scans are also opt-in via RAW_DIAGNOSTICS; the one-time boot scan and normal100ms link maintenance remain enabled.

Repeat-run precondition: a completed benchmark sends Stop and leaves Core
Protected. New independent configuration is rejected until gateway reset
creates a new boot/session context. Do not run back-to-back shell loops without
reset and do not relax the intentional configuration/protection guards.
freerun-ssc-al-02 demonstrated startup deadline failure with zero samples;
03 did not execute. Preserve failure evidence before reset.
