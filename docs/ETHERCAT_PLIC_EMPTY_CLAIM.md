# PLIC empty claim: 500 us DC lab reliability fix

The 512-probe dc-500us-wide-01 run halted after 419 successful RTT samples.
Saved CPU PC resolved to arch_system_halt; reason1 and PLIC save_irq0, plus
return address immediately after z_irq_spurious, identify the empty-claim path.
The interrupted PC was plic_irq_enable_set_state; the precise notification/mask
race is not established. This is not evidence of motor nonresponse or a Sync0
software deadline violation.

PLIC 1.0 defines claim0 as no pending interrupt. No source was claimed, so return
without dispatch or completion. Out-of-range nonzero IDs still call spurious;
valid but unregistered IDs still hit the default fatal handler. Move bounds
checks before optional statistics indexing as well. Valid interrupt acknowledge
ordering remains unchanged, including Andes edge mode.

Primary reference: https://github.com/riscv/riscv-plic-spec/blob/master/riscv-plic.adoc
section Interrupt Claim Process.

This is a small local backport to pinned Zephyr8c6db8fa778cf90eb93e823e513a1ab97703711d.
Apply explicitly after west update:

```
python3 tools/apply_zephyr_plic_fix.py
python3 tests/plic_empty_claim_contracts.py
```

The application DC CMake check rejects a dependency missing the diagnostic
marker. The patch application checks both forward and reverse applicability,
is idempotent, and refuses to overwrite incompatible edits. Patch is stored at
patches/zephyr/0001-plic-handle-empty-claim.patch. No license-restricted SSC files
are modified. Normal non-DC builds omit diagnostic counters; empty-claim fix
applies to all builds of this dependency.

Tests compile the actual PLIC handler with MMIO/ISR substitutes under ASan/UBSan:
10000 empty claims,10000 valid claims, empty-followed-by-valid, boundary/MAXUINT,
unregistered validIRQ, completion order. Four configurations cover shell stats
and Andes edge/level capability. A model does not reproduce hardware races.

DC-only dust_plic_claim_stats is32B littleendian8uint32:
version1,entries,empty,valid,invalid,last_claim,last_valid,reserved.
Counters have boot lifetime; entries == empty+valid+invalid (mod32bit).
An empty claim preserves the previous save_irq/save_dev; use last_claim for
latest raw value. No complete0 is issued. No logging in the handler.

First hardware check: same500us cycle,250us nominal shift, CAN3 ID3 disable-only,
2000 probes. Keep prior22-stage diagnostics, histograms and slowest4 cycles.
Read claim counters AND all timing snapshots after masterinactive before reset.
If empty>0 and test passes, the formerly fatal branch was exercised and recovered.
If empty==0, long-run pass alone does not demonstrate the branch was exercised.
Nonzero invalid claims remain fatal and must not be hidden. No RT guarantee.
