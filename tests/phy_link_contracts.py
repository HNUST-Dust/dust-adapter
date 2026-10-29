#!/usr/bin/env python3
"""Compile actual PHY link updater with MDIO/link-state substitutes."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root.parent/'hpm_support/samples/ecat/Middleware/ESC/hpm5e31_phy.c').read_text()
start=s.index('static int prev0 =')
body=s[start:]
prefix=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define BIT(n) (1U<<(n))
#define PHY_BMSR_REG 1U
#define PHY_BMCR_REG 0U
#define PHY_LINK_STATUS BIT(2)
#define BOARD_ECAT_PORT0_PHY_ADDR 0
#define BOARD_ECAT_PORT1_PHY_ADDR 1
struct device {int unused;};
static bool port0_configured=true,port1_configured=true;
static unsigned reads[2],status_reads[2],dumps,transitions;
static bool links[2],latch_low,fail_read,invalid_status;
static unsigned serial, fail_at;
static uint16_t control=BIT(13)|BIT(8);
static int phy_read(const struct device *d,uint8_t addr,uint8_t reg,uint16_t *value) {
 (void)d;assert(addr<2);++reads[addr];
 ++serial; if(fail_at && serial==fail_at) return -1;
 if(addr==0 && fail_read) return -1;
 if(reg==PHY_BMSR_REG) {
   ++status_reads[addr];*value=PHY_LINK_STATUS;
   if(addr==0 && invalid_status) *value=0xffff;
   if(addr==0 && latch_low && (status_reads[addr]&1)) *value=0;
 } else {assert(reg==PHY_BMCR_REG && addr==0);*value=control;}
 return 0;
}
static void hpm_esc_set_link_state(const struct device *d,unsigned port,bool up) {
 (void)d;assert(port<2);links[port]=up;
}
static void printk(const char *format,...) {
 if(strstr(format,"phy dump")) ++dumps; else ++transitions;
}
'''
main=r'''
#if defined(CONFIG_DUST_ECAT_DC_SYNC)
static void poll_steps(struct device *d) {
 unsigned steps=0;bool complete;
 do {unsigned n=reads[0]+reads[1];complete=ecat_phy_update_link_step(d);
 assert(reads[0]+reads[1]-n<=1);assert(++steps<=5);
 } while(!complete);
}
#endif

int main(void) {
 struct device d={0};
 for(unsigned i=0;i<20;++i) ecat_phy_update_link(&d);
 assert(links[0] && links[1]);
 assert(reads[0]==60 && reads[1]==40 && dumps==0);
 assert(transitions==2);
 unsigned before=reads[0];latch_low=true;status_reads[0]=0;
 ecat_phy_update_link(&d);assert(links[0] && reads[0]-before==3);
 latch_low=false;fail_read=true;ecat_phy_update_link(&d);assert(!links[0] && links[1]);
 fail_read=false;invalid_status=true;ecat_phy_update_link(&d);assert(!links[0]);
 invalid_status=false;control=BIT(13);ecat_phy_update_link(&d);assert(!links[0]);
 control=BIT(13)|BIT(8);ecat_phy_update_link(&d);assert(links[0]);
 port1_configured=false;before=reads[1];ecat_phy_update_link(&d);
 assert(!links[1] && reads[1]==before);
 port0_configured=false;before=reads[0];ecat_phy_update_link(&d);
 assert(!links[0] && reads[0]==before);

#if defined(CONFIG_DUST_ECAT_DC_SYNC)
 port0_configured=port1_configured=true;
 latch_low=true;status_reads[0]=status_reads[1]=0;
 unsigned n=reads[0]+reads[1];poll_steps(&d);
 assert(links[0] && links[1] && reads[0]+reads[1]-n==5);
 latch_low=false;
 for(unsigned failure=1;failure<=5;++failure) {
   fail_at=serial+failure;poll_steps(&d);fail_at=0;
   assert(failure<=3 ? (!links[0] && links[1]) : (links[0] && !links[1]));
   poll_steps(&d);assert(links[0] && links[1]);
 }
 invalid_status=true;poll_steps(&d);assert(!links[0] && links[1]);
 invalid_status=false;control=BIT(13);poll_steps(&d);assert(!links[0]);
 control=BIT(13)|BIT(8);poll_steps(&d);assert(links[0]);
 port0_configured=port1_configured=false;n=reads[0]+reads[1];
 poll_steps(&d);assert(!links[0] && !links[1] && reads[0]+reads[1]==n);
#endif
 puts("Actual PHY link checks / latch-low / invalid-read / speed-duplex / quiet-diagnostics contracts PASS");
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t)/'contracts.c';p.write_text(prefix+body+main)
 for flags in (['-DCONFIG_DUST_ECAT_DC_SYNC=1'], []):
  subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*flags,str(p),'-o',str(p.with_suffix(''))],check=True)
  subprocess.run([str(p.with_suffix(''))],check=True)
