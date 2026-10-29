# SM2 synchronized publication lab

CONFIG_DUST_ECAT_SM_SYNC defaultn. This is SM-event synchronization, not DC or
Sync0: imported SSC has DC_SUPPORTED0 and pre-generated sources lack required
DC paths. Never enable DC by blindly changing a macro. Only FreeRun and exact
outputSM1/inputSM2(0x22) pair accepted. Input-only SM3 and DC/mixed modes rejected.
Sync0/1 hardware interrupts remain disabled. Current SSC minimum cycle1ms;
first test is a1ms functional run, not a125us performance comparison.

PDI hard ISR only masks its IRQ, latches pending and gives a bounded semaphore.
EEPROM emulation and SSC PDI_Isr run in the original main/SSC owner thread.
Before invoking SSC, require a real SM2 PROCESS_OUTPUT_EVENT and enabled SM
mode; unrelated mailbox/EEPROM AL sources do not run the business cycle.
Core/CAN/UART mutex operations never move into ISR. SSC interrupt disable/enable
requests are preserved, but enable is deferred while pending or servicing.
Semaphore consumption does not clear pending; a new notification during service
is retained. Reset clears pending/semaphore while line is masked. MainLoop
continues mailbox/AL/watchdog processing; owner wait is bounded100us in SM mode,
woken by PDI. Legacy busy-wait/sleep behavior remains for FreeRun.

SSC standard PDI flow consumes output, runs application, then maps/writes input.
CAN worker remains event-driven priority0; SSC owner priority1. A new CAN cache
version waits for next SM2 application cycle instead of writing from gateway
poll; existing !bEscIntEnabled guard disables that FreeRun extra write. No
repeated-publication suppression, DMA, late-refresh or fast-notice feature is
combined with this experiment. Layout remains Rx96/Tx400. No fixed host receive
offset; previous phase experiment lives separately in host tools/experiments.

SM test host omits forced sync SDOs: standard SSC CheckSyncTypeValue rejects
SM writes while nPdOutputSize is still0 in PREOP; SSC auto-selectsSM1/0x22
at mapping/start-input when bSyncSetByUser isfalse. FreeRun control explicitly
writes0/0 as before. Fresh boot required between runs to avoid retained state.
Do not infer synchronization merely from OP or host intent. After test, export
saved ELF symbols dust_pdi_stats20B (version,notifications,services,waits,
sync_cycles), sSyncManOutPar and sSyncManInPar. Require nonzero sync_cycles and
correct sync types; fallback to FreeRun must not be accepted as an SM result.
Counts include startup/idle and are not per-command counts.

Saved images: build/bench-host/sm-owner-control and sm-owner-01. Config differs
only SM_SYNC. Trace off, DMA off, event-only CAN worker, sched stats on.
Use matching saved ELF for exports, never current mutable build ELF after
rebuild. Board master must be inactive before JTAG export/flash.

Native checks: deferred PDI sanitizer contracts,5 SSC configurations (baseline,
coalesce,late,fast,SM), real standard SDO validation/capabilities, unsupported
sync tuples and full protocol suite passed. Native dictionary harness explicitly
initializes standard CoE subindex count/PDO sizes, which MainInit/CoE normally
sets. Does not replace real SSC event/SM handshakes on hardware. Target control
and SM builds passed. SM ROM174208B/RAM211108B. Native hostC++20 -O2 -Wall
-Wextra -Werror build passed; original benchmark binaries preserved.

First NUC command after SM flash verified:
sudo bash ~/dust-ecat-bench/ethercat_rtt_sm_sync_probe.sh 1000 64 sm-owner-01 sm

Require PASS0timeouts/0WKC plus real mode evidence. Export RAM after stop/master
release, no reset before counters. Then evaluate same-period FreeRun control
and only subsequently lower cycle times with accurate minimum-cycle capability
and timing/overrun checks. SSC cycle exceeded/missed-event/sync-error counters
must also be inspected; observed OP alone is insufficient for performance claim.

No user-mainline merge, commit or push performed. Experimental Kconfig default
remains disabled; no production transport change.
