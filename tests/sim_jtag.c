/* Simulated MAX V TAP + host implementation of jtag.h, so svf.c can be
   exercised end to end without hardware. */
#include <string.h>
#include <stdio.h>
#include "shim.h"
#include "../firmware/jtag.h"

#define SIM_IDCODE   0x020A50DDu
#define SIM_USERCODE 0x0BADC0DEu
#define IRLEN 10
#define IR_IDCODE 0x006
#define IR_USERCODE 0x007

volatile uint32_t jtag_edge_delay_us = 0;
static tap_state_t st = TAP_RESET;
static const tap_state_t tap_next[TAP_STATE_COUNT][2] = {
 {TAP_IDLE,TAP_RESET},{TAP_IDLE,TAP_DRSELECT},{TAP_DRCAPTURE,TAP_IRSELECT},
 {TAP_DRSHIFT,TAP_DREXIT1},{TAP_DRSHIFT,TAP_DREXIT1},{TAP_DRPAUSE,TAP_DRUPDATE},
 {TAP_DRPAUSE,TAP_DREXIT2},{TAP_DRSHIFT,TAP_DRUPDATE},{TAP_IDLE,TAP_DRSELECT},
 {TAP_IRCAPTURE,TAP_RESET},{TAP_IRSHIFT,TAP_IREXIT1},{TAP_IRSHIFT,TAP_IREXIT1},
 {TAP_IRPAUSE,TAP_IRUPDATE},{TAP_IRPAUSE,TAP_IREXIT2},{TAP_IRSHIFT,TAP_IRUPDATE},
 {TAP_IDLE,TAP_DRSELECT}};
static const struct{const char*n;tap_state_t s;} names[]={
 {"RESET",TAP_RESET},{"TLR",TAP_RESET},{"IDLE",TAP_IDLE},{"RTI",TAP_IDLE},
 {"DRSELECT",TAP_DRSELECT},{"DRCAPTURE",TAP_DRCAPTURE},{"DRSHIFT",TAP_DRSHIFT},
 {"DREXIT1",TAP_DREXIT1},{"DRPAUSE",TAP_DRPAUSE},{"DREXIT2",TAP_DREXIT2},
 {"DRUPDATE",TAP_DRUPDATE},{"IRSELECT",TAP_IRSELECT},{"IRCAPTURE",TAP_IRCAPTURE},
 {"IRSHIFT",TAP_IRSHIFT},{"IREXIT1",TAP_IREXIT1},{"IRPAUSE",TAP_IRPAUSE},
 {"IREXIT2",TAP_IREXIT2},{"IRUPDATE",TAP_IRUPDATE}};
bool jtag_state_from_name(const char*n,tap_state_t*o){
 for(size_t i=0;i<count_of(names);i++) if(!strcmp(n,names[i].n)){*o=names[i].s;return true;} return false;}

/* device model */
static uint32_t ir=IR_IDCODE, dr=SIM_IDCODE, ir_sh=0; static int drlen=32;
unsigned long sim_clocks=0; unsigned long sim_runtest=0;

bool jtag_clock(bool tms,bool tdi,bool read){
 sim_clocks++;
 bool tdo = (st==TAP_DRSHIFT)?(dr&1):((st==TAP_IRSHIFT)?(ir_sh&1):1);
 if(st==TAP_DRSHIFT) dr=(dr>>1)|((uint32_t)tdi<<(drlen-1));
 else if(st==TAP_IRSHIFT) ir_sh=(ir_sh>>1)|((uint32_t)tdi<<(IRLEN-1));
 tap_state_t nx=tap_next[st][tms?1:0];
 if(nx==TAP_DRCAPTURE){ if(ir==IR_IDCODE){dr=SIM_IDCODE;drlen=32;}
   else if(ir==IR_USERCODE){dr=SIM_USERCODE;drlen=32;} else {dr=0;drlen=1;} }
 else if(nx==TAP_IRCAPTURE) ir_sh=0x01;
 else if(nx==TAP_IRUPDATE) ir=ir_sh&((1u<<IRLEN)-1);
 else if(nx==TAP_RESET){ir=IR_IDCODE;dr=SIM_IDCODE;drlen=32;}
 st=nx; return read?tdo:false;}
void jtag_reset(void){for(int i=0;i<5;i++)jtag_clock(true,false,false);st=TAP_RESET;}
tap_state_t jtag_state(void){return st;}
static int path(tap_state_t f,tap_state_t t,uint8_t*o){
 if(f==t)return 0; tap_state_t q[16];int8_t pv[16],pt[16];bool sn[16]={false};int h=0,tl=0;
 for(int i=0;i<16;i++){pv[i]=-1;pt[i]=0;} q[tl++]=f;sn[f]=true;
 while(h<tl){tap_state_t s=q[h++];for(int m=0;m<2;m++){tap_state_t n=tap_next[s][m];
  if(sn[n])continue;sn[n]=true;pv[n]=s;pt[n]=m;
  if(n==t){int L=0;tap_state_t c=t;uint8_t tmp[16];
   while(c!=f){tmp[L++]=pt[c];c=pv[c];} for(int i=0;i<L;i++)o[i]=tmp[L-1-i];return L;}
  q[tl++]=n;}} return -1;}
void jtag_goto(tap_state_t t){uint8_t m[16];int L=path(st,t,m);for(int i=0;i<L;i++)jtag_clock(m[i],false,false);}
void jtag_run_test(uint32_t c,tap_state_t s){sim_runtest+=c;jtag_goto(s);for(uint32_t i=0;i<c;i++)jtag_clock(false,false,false);}
static inline bool bg(const uint8_t*b,uint32_t i){return b&&((b[i>>3]>>(i&7))&1);}
bool jtag_shift(uint32_t n,const uint8_t*tdi,const uint8_t*te,const uint8_t*mk,
                tap_state_t ss,tap_state_t e,uint32_t*fb){
 if(!n){jtag_goto(e);return true;} jtag_goto(ss);
 bool chk=(te!=NULL),ok=true;
 for(uint32_t i=0;i<n;i++){bool b=bg(tdi,i),last=(i==n-1);
  bool got=jtag_clock(last,b,chk);
  if(chk&&ok){bool care=mk?bg(mk,i):true;
   if(care&&got!=bg(te,i)){ok=false;if(fb)*fb=i;}}}
 jtag_goto(e); return ok;}
uint32_t jtag_read_idcode(void){uint32_t id=0;jtag_reset();jtag_goto(TAP_DRSHIFT);
 for(int i=0;i<32;i++){if(jtag_clock(i==31,false,true))id|=(1u<<i);}
 jtag_goto(TAP_IDLE);jtag_reset();return id;}
void jtag_init(void){jtag_reset();}
