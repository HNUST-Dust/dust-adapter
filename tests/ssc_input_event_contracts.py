#!/usr/bin/env python3
"""Compile actual SSC mapping before/after adjustment; compare timing semantics."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('adjust', root/'tools/ssc_build_adjustments.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
source = (root/'external/ssc/ecatappl.c').read_text()
updated = module.skip_unused_input_event(source)
assert source != updated
for bad in [source.replace('ALEvent = SWAPWORD(ALEvent);', 'ALEvent = 0;'),
            source.replace('APPL_InputMapping((UINT16*)aPdInputData);',
                           'if (ALEvent) return; APPL_InputMapping((UINT16*)aPdInputData);')]:
    try:
        module.skip_unused_input_event(bad)
    except ValueError:
        pass
    else:
        raise AssertionError('Unrecognized/used event accepted')
prefix = r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t MEM_ADDR;
#define TRUE 1
#define FALSE 0
#define SWAPWORD(v) (v)
static unsigned event_reads, maps, writes, clocks;
static int measuring;
#define MEASUREMENT_ACTIVE measuring
static struct { UINT32 u32CalcAndCopyTime, u32MinCycleTime; } sSyncManInPar, sSyncManOutPar;
static int bMinCycleTimeMeasurementStarted;
static UINT32 u32MinCycleTimeValue;
static MEM_ADDR aPdInputData[400];
static UINT16 nEscAddrInputData=0x1d00, nPdInputSize=400;
static UINT16 HW_GetALEventRegister_Isr(void) { ++event_reads; return 0x400; }
static UINT32 GetSystemTimeDelay(UINT32 start) { ++clocks; return start ? 72 : 100; }
static void APPL_InputMapping(UINT16 *p) { assert((void*)p==aPdInputData); ++maps; }
static void HW_EscWriteIsr(MEM_ADDR *p, UINT16 a, UINT16 n) {
 assert(p==aPdInputData && a==0x1d00 && n==400); ++writes;
}
'''
main = r'''
int main(void) {
 for (unsigned active=0; active<2; ++active) {
  measuring=active; bMinCycleTimeMeasurementStarted=TRUE;u32MinCycleTimeValue=9;
  sSyncManInPar.u32CalcAndCopyTime=3;
  sSyncManInPar.u32MinCycleTime=sSyncManOutPar.u32MinCycleTime=4;
  PDO_InputMapping();
  assert(maps==active+1 && writes==active+1);
  printf("%u %u %u %u %u %d\n", active, clocks,
   sSyncManInPar.u32CalcAndCopyTime,sSyncManInPar.u32MinCycleTime,
   sSyncManOutPar.u32MinCycleTime,bMinCycleTimeMeasurementStarted);
 }
#if MIN_PD_CYCLE_TIME == 0 || PD_INPUT_CALC_AND_COPY_TIME == 0
 assert(event_reads == EXPECT_EVENT_READS);
#else
 assert(event_reads == 0);
#endif
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp)
    for minimum, copy in [(1000000,0), (0,0), (0,100000), (1000000,100000)]:
        outputs=[]
        for name, text, expected in [('baseline',source,2), ('adjusted',updated,0)]:
            start=text.index('void PDO_InputMapping(void)')
            end=text.index('void PDO_OutputMapping(void)',start)
            section=text[start:end];section=section[:section.rfind('}')+1]
            c=tmp/(name+'.c');exe=tmp/name;c.write_text(prefix+section+main)
            subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra',
                '-Wno-unused-function','-Wno-unused-variable','-Werror',
                '-fsanitize=address,undefined',f'-DMIN_PD_CYCLE_TIME={minimum}',
                f'-DPD_INPUT_CALC_AND_COPY_TIME={copy}',f'-DEXPECT_EVENT_READS={expected}',
                str(c),'-o',str(exe)],check=True)
            outputs.append(subprocess.check_output([str(exe)]))
        assert outputs[0]==outputs[1], (minimum,copy,outputs)
print('Actual SSC input mapping / measurement on-off / static-auto timing / copy / read count / refusal contracts PASS')
