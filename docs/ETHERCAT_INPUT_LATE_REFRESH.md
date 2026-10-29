# Bounded CAN refresh before SSC input mapping

`CONFIG_DUST_ECAT_INPUT_LATE_REFRESH` defaults off pending hardware results and
is mutually exclusive with input-write coalescing. No DMA or driver changes.
DTR1 v7 ABI unchanged. It adds one CAN generation check before the existing
APPL_InputMapping copy, at the last application-owned point before SSC writes
the input image. It does not change the ESC transfer width or number of writes.

Eligible only in Active business state, AL OP, input/output running, FreeRun,
thread context, no sticky DMA fault, and no unprocessed output PDO. No Core or
SSC call is added to the worker/ISR; no recursive PDO mapping or wait loop.

If no CAN generation change and no existing dirty flag, copy the existing image.
Otherwise capture the stable CAN snapshot and call Core.Publish once with
advance_uart=false. That preserves current UART process-image bytes/window
cursor and command acknowledgement/exchange state, without DispatchIo. Mark
publication/time/state as usual, then copy the resulting image to SSC's buffer.
A CAN IRQ after this last check waits for the next service pass; there is no
attempt to chase continuously arriving feedback indefinitely.

On rejected late snapshot, Core enters protection. Clear feedback references
and make at most one additional protected publication with empty feedback;
if even that fails, clear the image to avoid transmitting old Active data.
No retry loop or extra hardware write is used. Stop invalidates/coalescing
credits if compiled separately. The normal independent watchdog/IO checks
continue at their old checkpoints; this callback does not process pending RxPDO.

With SCHED_STATS, dust_ecat_input_refresh_stats is 16 B `<4I>`: version(1),
checks, refreshes, failures. Checks count eligible Active mapping calls; refreshes
count successful late CAN publication attempts; failures count rejected attempts
(not the protected fallback). Counters stop growing when no longer Active.
They do not estimate CPU utilization or prove host RTT improvement.

Native SSC tests compile baseline, coalesce-only and late-refresh-only variants.
The late test injects real CAN feedback after normal APPL_Application and checks
that the very next hardware image contains its sequence, with UART block bytes
unchanged, dispatch count unchanged, exactly one hardware write and no recursive
mapping. Unchanged feedback adds no publication. Tests cover sync/DC/ISR/stopped
ports, pending RxPDO, independent watchdog protection and invalid snapshot
failure producing a valid Protected image. Existing native IO tests cover UART
window/cursor preservation, receive concurrency, expiry and Stop/Arm behavior.

Compare against saved ssc-v7-01, not the zero-skip coalescing candidate. Use the
same v7 decoder; optional pending-write markers can disappear when the late
snapshot now happens before that write. Inspect matching sequence validity,
late counters, host WKC/timeouts and capture→ESC-end, not only an earlier snapshot
timestamp. Quantify performance with trace-off repeated ABBA after localization.
