# FreeRun SSC configuration audit / unused AL-event read experiment

Current comparison baseline freerun-return-01: CAN3-only125us/2000 PASS,
RTTmean755.877us,p95873.797us,max999.005us,zero timeout/WKC miss.

Compiler -dM/-E with the actual ecatappl compile command (see effective-config.json):
AL_EVENT_ENABLED1, INTERRUPTS_SUPPORTED1, DC_SUPPORTED0, ECAT_TIMER_INT0;
ESC_16BIT_ACCESS0, ESC_32BIT_ACCESS0. PDRAM word access is independently enabled
in the HPM driver; those SSC alignment flags do not make the current400B transfer
bytewise. Do not blindly force32bit register access with side-effect registers.
MIN_PD_CYCLE_TIME is overridden to1000000ns by gateway_config.h, not the500000ns
shown in the imported tool config. It describes application capability, not
the125us FreeRun host frame scheduler. Timing metadata needs a separate accurate
production review; do not lower declarations to claim faster execution.
PD_INPUT/OUTPUT_CALC_AND_COPY_TIME both0: auto measurement code remains.
MAX input1536/output1024 are capacities, not active Tx400/Rx96 packet lengths.
Do not shrink maxima expecting current packets to shrink.

Actual baseline disassembly shows PDO_InputMapping calls hpm5e_ssc_al_event
unconditionally when the auto timing branch is compiled. Its local event value
is never used. This experiment removes ONLY that read/swap pair in the recognized
build copy of the function; original licensed external/ssc is never rewritten.
Optional CONFIG_DUST_ECAT_SSC_SKIP_UNUSED_AL_READ defaultn, FreeRun-only.
No fake nonzero calc-copy constants; timing measurement and SDO values remain.
Reject source variants if the event has any remaining use or pattern differs.
Actual-function native ASan/UBSan tests compare baseline/adjusted mapping writes
and timer state for measurement off/on and all static/auto timing combinations;
PASS. Target build PASS ROM174988/RAM211024; disassembly confirms the one call
is gone. No host scheduling/PDO/identity/watchdog change; no extra profiling.

NUC: sudo bash ~/dust-ecat-bench/ethercat_rtt_aspm_probe.sh 125 2000 freerun-ssc-al-01

Preserve original normal receive/process/send order, no fixed receive offset.
Saved matching ELF/HEX/config under firmware build/bench-host/freerun-ssc-al-01.
Live validation pending. A single run cannot establish a small RTT gain.

Primary SSC reference: https://download.beckhoff.com/download/document/io/ethercat-development-products/an_et9300_v1i10.pdf
Next master-side question: distinguish wire/frame completion from application
observation with timestamps. Historical receive mean0.875us and final wake
mean0.059us do not demonstrate NIC/driver slowness or a recoverable125us.

Live experiment PASS2000: mean718.904us vs baseline755.877us; P50/P95 nearly unchanged. Host send-call mean0.547us and receive-to-consume2.167us. Band-population shift prevents causal37us gain claim. Repeat current image before host scheduling changes; raw results archived in host docs/bench-results/2026-10-30-freerun/freerun-ssc-al-01.

Repeat-run precondition: a completed benchmark sends Stop and leaves Core
Protected. New independent configuration is rejected until gateway reset
creates a new boot/session context. Do not run back-to-back shell loops without
reset and do not relax the intentional configuration/protection guards.
freerun-ssc-al-02 demonstrated startup deadline failure with zero samples;
03 did not execute. Preserve failure evidence before reset.

Fresh-reset B replicate03 PASS2000, mean722.802us, p95873.379us,
max999.021us, no timeout/WKC miss. This agrees with B01mean718.904us but
does not improve P95; rare999us tail returned. Fresh-reset A control prepared
using SHA256-verified original freerun-return-01 saved HEX, no rebuild, under
freerun-ssc-control-02. Continue to treat skip-read option as defaultn until
the control establishes whether the mean shift follows the firmware change.

Completed fresh-reset A/B/B/A control: all4valid sessions PASS2000 with no
timeouts/WKC misses. Means A755.877,B718.904,B722.802,A776.132us.
A pooled mean766.0045us vs B720.853us: observed -45.1515us (-5.89%).
This supports keeping B as the selected lab baseline; the optional setting
remains defaultn outside the explicit FreeRun experiment configuration.
P50/P95 nearly unchanged,999us tail recurs; no worst-case or direct45us
register-access saving claim. Sequential4sessions are not randomized loaded
validation. Failed no-reset02 had zero samples and is excluded.
Restore saved B firmware after exporting A control. Next investigate master
receive observation timing without introducing fixed receive offset.
