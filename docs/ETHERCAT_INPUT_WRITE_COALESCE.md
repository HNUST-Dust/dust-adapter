# Active FreeRun input publication coalescing

`CONFIG_DUST_ECAT_INPUT_WRITE_COALESCE` defaults off pending board validation.
Enable only for the controlled candidate comparison. The original synchronous
CPU PDRAM write remains in use; DMA is still off. No SSC vendor file is edited.

Each successful Core.Publish increments a private 64-bit image revision, separate
from the wrapping 32-bit PDO publication number. APPL_InputMapping copies the
stable image as before and hands its buffer identity/revision to the immediately
following SSC HW_EscWriteIsr adapter. SSC mapping, measurement and loop/state
processing still execute on every normal call. Only the actual duplicate data
write can be omitted; the first write for every new image is always synchronous.

Eligibility requires the exact just-mapped source pointer, full prepared input
length, fixed input partition address, Active business state, running input AND
output, FreeRun (no ESC-sync/DC) and thread context, with no sticky DMA fault.
All other requests use the raw path and revoke previous write credit. Credit
is set only after the raw write returns without the existing sticky error.
The CPU driver has the existing void completion contract; this does not add
hardware bus-fault detection. Register/mailbox paths use unchanged raw macros.

Init, mapping generation and input/output Start/Stop invalidate credit. Mapped
buffers are copied before each adapter call; no ISR allocation, locks, queues,
async return or recursive PDO mapping is added. CAN/UART service, watchdog checks,
expiry and UART publication-window rules remain at their existing checkpoints.
Version wrap forces a write. Lifecycle/mode transitions cannot inherit a skip.

Buffered-SM rationale: Beckhoff Technology section 8.1 describes retaining the
last consistently written data, and a consumer loads a new buffer only when one
is available. This permits withholding an identical new producer transaction;
we never skip a partial transaction or touch additional SM buffer addresses.
Source: https://download.beckhoff.com/download/Document/io/ethercat-development-products/ethercat_esc_datasheet_sec1_technology_v2.5.pdf
This reasoning does not replace actual master/SM verification. Input-event
acknowledgement/timing and mixed traffic remain board validation concerns.

With SCHED_STATS enabled, dust_ecat_input_write_stats is 20 bytes `<5I>`:
version(1), requests, writes, skipped, failures. Counters include only calls
while Core is Active, so SAFEOP activity after the bench cannot contaminate
them. writes counts raw synchronous attempts, not independently verified bus
transactions. requests = writes + skipped (modulo uint32 wrap). Skips do not
imply CPU usage or a host RTT improvement.

Native integration compiles both feature-off and feature-on variants. Tests
exercise actual gateway/Core/SSC dictionary integration, mapping-to-adapter
handoff, first/new/duplicate publication, precise source/address/length checks,
mode and ISR fallback, rejected lifecycle transitions, transport failure,
Protected and input-stop paths, and watchdog protection during idle suppressed
writes. Existing CAN/UART/expiry/Stop and trace contracts also pass. Actual
linked PDO_InputMapping was disassembled to confirm APPL_InputMapping followed
by the adapter call. Full scripts/test_protocol.sh passed; after narrowing
counter scope to Active, both SSC variants passed again.

Candidate uses DTR1 v7 unchanged. Compare against saved ssc-v7-01, inspect
trace flags, master WKC/timeouts and input-write counters first. Quantify actual
RTT only with a later trace-off repeated ABBA. Single 64-probe localization run
is insufficient to declare a performance win or hard real-time guarantee.
The earlier empty-cache time-read optimization is retained in both images.
