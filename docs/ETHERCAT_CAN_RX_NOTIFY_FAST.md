# Atomic version hint for the late CAN checkpoint

CONFIG_DUST_ECAT_CAN_RX_NOTIFY_FAST defaults off, depends on late refresh and
CAN receive worker. CPU word ESC writes and DTR1 v7 remain unchanged.

Worker cache generation updates, SSC expiry changes, Stop and Arm each also
increment a Zephyr atomic notification version under the same capture mutex.
Regular ServiceCanReceive samples that version into owner-private observed
state while holding the mutex after expiry/generation handling. CanReceivePending
only compares the atomic version against this private observation. It never
clears producer state, reads mutable worker cache fields, takes a capture mutex,
reads uptime or adds a trace checkpoint. A producer arriving after a false probe
remains pending for the next regular/late check. If already dirty, the late
caller bypasses the probe and enters the full service path. Generation equality
uses uint32 bit patterns, separate from protocol publication numbers.

This is an advisory **version** hint, NOT an expiry/freshness validator. A false
probe may coexist with aged cached data if no event has arrived. All pre-existing
regular SSC receive-service calls and CaptureCanFeedback expiry checks remain
unconditional. Only the optional extra check in APPL_InputMapping uses the hint.
Never gate the entire main-loop service on this hint, or expiry would stop.
Data pointers are still copied under mutex before publication; no SSC ownership
or IO safety policy is relaxed. Single-thread/feature-off probe returns true.

Native tests use actual std::thread and barriers. They verify 100 unchanged
probes take no capture mutex and do not read the clock, repeated positive probes
cannot clear a notification, an event following a negative probe is observed,
Stop clearing is notified and does not resurrect data, concurrent producer /
consumer snapshots remain stable, and no-IRQ expiry survives uint32 timestamp
wrap. SSC tests compile baseline/coalesce/late/fast-late variants; the fast case
avoids the full service call on unchanged mapping, while all UART preservation,
mode, command, dispatch, watchdog and invalid-snapshot protection tests pass.
Target disassembly confirms CanReceivePending calls atomic_get and no mutex /
uptime functions; in this ELF atomic_get itself is just lw followed by ret.

With SCHED_STATS, dust_ecat_can_notice_stats is 16 B `<4I>`: version1, probes,
pending, unchanged. Current production caller probes only eligible Active late
mappings; counters otherwise have BSS lifetime (not reset by gateway reinit).
probes = pending + unchanged modulo uint32. Existing input_refresh_stats remains
16 B `<4I>` version1, checks, refreshes, failures. A positive hint does not prove
a unique motor reply; lifecycle/expiry also notify. Counts are not CPU utilization.

Compare the fast candidate against saved late-v7-01 (only fast flag differs),
and against saved ssc-v7-01 for total effect. Inspect fast-negative counts,
late rebuilds, trace flags, capture→ESC-end and host RTT. Earlier per-cycle lock
cost was a hypothesis; reduced instruction work is not itself proof of RTT
benefit. Quantify only with later trace-off ABBA after a useful localization run.
