# DC Sync0 lab integration

User generated source /Users/panpoming/Downloads/dust_hpm5e_dc imported into
external/ssc_dc with manifest. Only DC_SUPPORTED changes0->1 relative to the
previous configured ecat_def.h. Full newly generated DC implementation is used;
no hand-splicing licensed DC state machine and no mutation of external/ssc.
external/ssc_dc is ignored by git. No licensed source published or pushed.

CONFIG_DUST_ECAT_DC_SYNC defaultn, depends on the owner-thread SM infrastructure.
Independent build/dm_rtt_dc explicitly selects external/ssc_dc; CMake rejects
DC mode with FreeRun-generated SSC and DC-generated SSC without DC wiring.
Default ESI/SII bytes unchanged. DC build ESI adds Sync0-only mode, AssignActivate
0x0300 matching official HPM SDK ESI, CycleTimeSync0 factor1; no Sync1 mode.
Generated project min500us is overridden by existing gateway minimum1ms for
this first functional run. It does not advertise125us support. Rx96/Tx400 and
application identity/layout stay unchanged at1ms (signature2699856283).

Independent Sync0 hard IRQ reads status0x098e to acknowledge and increments
atomic pending count plus a semaphore wake. IRQ does not run SSC/Core or take
CAN/UART mutexes. Sync0 stays countable while owner works. Main services PDI
output mapping then Sync0 application/input mapping; ordering follows service
policy, not timestamped physical edge ordering. For the initial1ms/500us shift
these paths should be separated, but actual IRQ/service latency must be checked.
This is deferred synchronization, not zero-jitter hardware-latched application
execution. HPM GPR already prevents sync->PDI IRQ routing; additionally the DC
build masks Sync0/1 AL event bits in SSC event reads so full PDI_Isr cannot
service the same pulse again. Sync1 IRQ disabled and no Sync1 support advertised.

SSC's own DC watchdog remains enabled during startup. Extra software faults:
bit1 pending backlog>1, bit2 IRQ-to-owner latency>=configured period, bit4 owner
handler work>=period. A backlog is serviced once, not replayed as stale business
cycles. Sticky faults protect Core before the pending handler if business is
already Active; a work-overrun fault protects immediately after it returns.
Startup settling is not enforced by this extra policy; fault/maximum timing
fields reset once when Core enters Active, while SSC DC watchdog remains in
force throughout. Heavy inactive PHY scans are suppressed while DC is active;
normal link maintenance remains. No safety path, motor protection or frame
contract removed. Only CAN3 ID3 disable commands allowed by the bench.

Runtime symbol dust_sync0_stats is32B <8I>: version,notifications,services,
coalesced,fault,period_cycles,max_lateness,max_work. Timer24MHz; last3 timing
fields are cycles, not nanoseconds. Notifications/services/coalesced include
startup and stop; max timing/fault reset on business Arm and may include stop.
dust_pdi_stats remains20B. Export via saved matching ELF after master inactive,
before reset/flash. Need nonzero Sync0 services, types2/2, cycle1,000,000ns,
support mask7 and fault0; also inspect standard SSC exceeded/missed/sync status.
Do not infer actual phase alignment from host shift parameter alone.

Independent NUC dust_ethercat_dm_rtt_dc SHA256:
f41d7d1f1cb120dfa55e354c4f002aef28ea9e138ab0897821935776b5952931.
Normal receive/process/send order; no fixed receive offset. Select only LAB
slave as DC reference, configure0x0300/cycle1ms/shift500000ns/Sync1off; provide
steady-clock application time at the same cycle location; queue reference
clock sync every10cycles and slave clock compensation every cycle. DC adds
clock datagrams, so it is not identical bus traffic to historical FreeRun.
Initial clock phase and actual IRQ phase/latency need hardware verification.
No performance acceptance before functional and repeated phase/control tests.

Validation: full legacy native suite PASS; real generated DC SSC SDO/mapping
contracts PASS; Sync0 pending/latency/work/wrap fault contracts PASS; ESI DC mode
and default-byte invariance PASS; target build and NUC C++20 -O2 -Wall -Wextra
-Werror build PASS. Target ROM177384B/RAM211172B. Final saved ELF/HEX/config,
manifest, own-source patch, tests and logs archived. New functionality has not
yet passed a live DC test. No commit/merge/push.

First NUC command:
sudo bash ~/dust-ecat-bench/ethercat_rtt_dc_probe.sh 1000 64 dc-sync0-01 500000

After completion read log/CSV and master state, then export dust_sync0_stats,
dust_pdi_stats, sSyncManOutPar/sSyncManInPar using saved
build/bench-host/dc-sync0-01/zephyr.elf. Check DC activation/period/phase registers
read-only where useful. Never halt an active master. Preserve failure logs and
restore A baseline if the run fails; do not alter watchdog or relax WKC merely
to enter OP. Do not infer performance benefit from64 samples or compare1ms
result directly to historical125us FreeRun.

## 2026-10-30 pre-maintenance owner checkpoint

The DC owner now checks read-only PDI/Sync0 pending snapshots immediately
before maintenance, returning to its existing PDI then Sync0 entry when work
is queued. Snapshots do not consume counts, timestamps or wake semaphores.
Normal link checks and all existing watchdog/fault paths remain enabled.
This cannot prevent an interrupt arriving after the snapshot from waiting
for an in-progress MDIO operation; splitting maintenance remains a later step.

Keep the quiet baseline measurement configuration for comparison: RTT trace,
detailed DC phase profiling and periodic raw PHY diagnostics disabled; memory
wait profiling enabled temporarily. Do not attribute this experiment to removal
of wait profiling. Native pending/backlog/wrap/budget contracts and the target
build pass; live CAN3-only 500us/2000-sample validation is still required.

Checkpoint live run dc-maint-check-01: CAN3-only500us/2000 PASS, no timeout,
WKC miss, Active sync fault or coalescing. Maximum wait171.708->108.958us;
mean12.938->15.758us. RTTmean1775.518->1638.662us is a single-run observation,
not established gain. Handlermax270.083us still exceeds250us. Raw snapshots
and log/CSV are archived in host docs/bench-results/2026-10-30-dc-wait.

Next dc-maint-step-01 splits normal PHY checks into at most one MDIO read per
owner iteration (DC quiet builds only). The poll remains due until both ports
are evaluated; the next100ms interval begins after completion. Each iteration
returns through normal PDI/Sync0 service. Preserve latch-low double BMSR reads,
BMCR100M/full-duplex checks, failed-read link-down behavior, and link-transition
messages. Original blocking updater remains for boot/nonDC/explicit raw-debug
builds. This is cooperative splitting, not asynchronous MDIO: a single read
can still block. Native tests exercise per-read failures, latch-low, wrong
duplex, unconfigured ports, and the one-read-per-step bound. Live validation
for this second step remains pending; same instrumentation and host phase.

Incremental-maintenance live run dc-maint-step-01: CAN3-only500us/2000 PASS,
zero timeout/WKC miss/Active fault/coalescing. Active41975cycles: mean handler
104.174us,max209.458us; mean wait16.862us,max114.167us. RTTmean1690.065us,
p951995.577us. Overall latency gain NOT established versus checkpoint run.
Maintenance-attributed mean wait0.08657->0.03336us and top4 no longer contain
LinkMaintenance; remaining tails involve semaphore/PDI/SSCMainLoop with IRQ
overlap. This is attribution evidence, not a bound on every MDIO wait.
PLIC empty-claim counter1, invalid0, Activefault0: the previously fixed empty
claim return path was exercised without a fatal error in this run. First
protection is the normal Stop at core.cpp:210. Single-run max below250us is
insufficient to reduce period. Keep installed image and500us for now.
