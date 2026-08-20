#include <stdio.h>
#include <string.h>
#include "shim.h"
#include "../firmware/jtag.h"
#include "../firmware/svf.h"
extern unsigned long sim_clocks, sim_runtest;
extern volatile uint32_t jtag_edge_delay_us;
void svf_progress_hook(uint32_t s,uint32_t b){(void)s;(void)b;}

static int run(const char*name,const char*svf,svf_result_t want){
 svf_ctx_t c; svf_init(&c);
 svf_result_t r = svf_feed(&c,(const uint8_t*)svf,strlen(svf));
 if(r==SVF_OK) r = svf_finish(&c);
 int pass = (r==want);
 printf("%-32s %-14s %s", name, svf_result_str(r), pass?"PASS":"*** FAIL ***");
 if(r!=SVF_OK) printf("   [%s]", c.error_detail);
 printf("   (%u stmts, %u bits)\n", c.statements, c.total_bits);
 return pass;
}
/* Feed one byte at a time to prove streaming works across chunk splits. */
static int run_streamed(const char*name,const char*svf,svf_result_t want){
 svf_ctx_t c; svf_init(&c); svf_result_t r=SVF_OK;
 for(size_t i=0;i<strlen(svf)&&r==SVF_OK;i++) r=svf_feed(&c,(const uint8_t*)svf+i,1);
 if(r==SVF_OK) r=svf_finish(&c);
 int pass=(r==want);
 printf("%-32s %-14s %s   (%u stmts)\n",name,svf_result_str(r),pass?"PASS":"*** FAIL ***",c.statements);
 return pass;
}
int main(void){
 int ok=1;
 jtag_init();
 printf("IDCODE via jtag_read_idcode: 0x%08X (expect 0x020A50DD)\n\n", jtag_read_idcode());

 ok &= run("correct IDCODE check",
  "! comment\nTRST OFF;\nENDIR IDLE;\nENDDR IDLE;\nSTATE RESET;\n"
  "SIR 10 TDI (006);\nSDR 32 TDI (00000000) TDO (020A50DD) MASK (FFFFFFFF);\n", SVF_OK);

 ok &= run("wrong IDCODE -> mismatch",
  "STATE RESET;\nSIR 10 TDI (006);\nSDR 32 TDI (0) TDO (12345678) MASK (FFFFFFFF);\n",
  SVF_ERR_TDO_MISMATCH);

 ok &= run("USERCODE readback",
  "ENDIR IDLE;\nENDDR IDLE;\nSTATE RESET;\n"
  "SIR 10 TDI (007);\nSDR 32 TDI (0) TDO (0BADC0DE) MASK (FFFFFFFF);\n", SVF_OK);

 ok &= run("MASK ignores don't-care bits",
  "STATE RESET;\nSIR 10 TDI (006);\nSDR 32 TDI (0) TDO (FFFFFFDD) MASK (000000FF);\n", SVF_OK);

 ok &= run("multi-line wrapped hex vector",
  "ENDIR IDLE;\nENDDR IDLE;\nSTATE RESET;\nSIR 10 TDI (006);\n"
  "SDR 32 TDI (00000000)\n    TDO (020A50DD)\n    MASK (FFFFFFFF);\n", SVF_OK);

 ok &= run("// comment style",
  "// leading comment\nSTATE RESET;  // trailing\nSTATE IDLE;\n", SVF_OK);

 ok &= run("RUNTEST forms",
  "STATE RESET;\nRUNTEST 100 TCK;\nRUNTEST IDLE 50 TCK;\nRUNTEST IDLE 10 TCK 1.0E-3 SEC;\n", SVF_OK);

 ok &= run("multi-device chain rejected",
  "HIR 0;\nHDR 4;\nSTATE RESET;\n", SVF_ERR_CHAIN);

 ok &= run("zero-length chain padding ok",
  "HIR 0;\nHDR 0;\nTIR 0;\nTDR 0;\nSTATE RESET;\n", SVF_OK);

 ok &= run("unsupported command rejected",
  "STATE RESET;\nPIOMAP (IN A);\n", SVF_ERR_UNSUPPORTED);

 ok &= run("bad state name rejected", "STATE NOTASTATE;\n", SVF_ERR_SYNTAX);

 ok &= run("oversize shift rejected", "SDR 99999 TDI (0);\n", SVF_ERR_TOO_LONG);

 printf("\n-- ENDIR/ENDDR are distinct (regression) --\n");
 {
   /* ENDIR and ENDDR differ only at index 3. If they are conflated, an
      ENDIR sets end_dr, every SDR ends in Pause-IR, and the route back to
      Shift-DR passes through Update-IR — silently destroying the loaded
      instruction. Two consecutive SDRs after one SIR is what exposes it:
      the first read succeeds and the second returns the wrong register. */
   svf_ctx_t c; svf_init(&c);
   const char *svf =
     "ENDDR IDLE;\nENDIR IRPAUSE;\nSTATE RESET;\n"
     "SIR 10 TDI (007);\n"
     "SDR 32 TDI (0) TDO (0BADC0DE) MASK (FFFFFFFF);\n"
     "SDR 32 TDI (0) TDO (0BADC0DE) MASK (FFFFFFFF);\n";
   svf_result_t r = svf_feed(&c,(const uint8_t*)svf,strlen(svf));
   if (r==SVF_OK) r = svf_finish(&c);
   int pass = (r==SVF_OK);
   printf("%-34s %-14s %s\n","two SDRs survive ENDIR IRPAUSE",
          svf_result_str(r), pass?"PASS":"*** FAIL ***");
   if(!pass){ printf("   [%s]\n", c.error_detail); }
   ok &= pass;
 }

 printf("\n-- FREQUENCY clamps the clock, never speeds it up --\n");
 {
   /* RUNTEST waits are TCK counts computed from this declared frequency, so
      it sets a ceiling on jtag_edge_delay_us, never a floor: a file
      declaring faster than we already run must not speed anything up, only
      a slower one should add delay. */
   svf_ctx_t c; svf_init(&c);
   jtag_edge_delay_us = 0xDEAD; /* poison: any leftover value would be wrong */
   svf_result_t r = svf_feed(&c,(const uint8_t*)"FREQUENCY 1.80E+07 HZ;\n",23);
   if (r==SVF_OK) r = svf_finish(&c);
   int pass = (r==SVF_OK && jtag_edge_delay_us==0);
   printf("%-34s %-14s delay=%-4lu %s\n","18 MHz leaves free-running clock",
          svf_result_str(r), (unsigned long)jtag_edge_delay_us,
          pass?"PASS":"*** FAIL ***");
   ok &= pass;
 }
 {
   svf_ctx_t c; svf_init(&c);
   jtag_edge_delay_us = 0;
   svf_result_t r = svf_feed(&c,(const uint8_t*)"FREQUENCY 1.00E+05 HZ;\n",23);
   if (r==SVF_OK) r = svf_finish(&c);
   int pass = (r==SVF_OK && jtag_edge_delay_us==5);
   printf("%-34s %-14s delay=%-4lu %s\n","100 kHz slows the clock down",
          svf_result_str(r), (unsigned long)jtag_edge_delay_us,
          pass?"PASS":"*** FAIL ***");
   ok &= pass;
 }

 printf("\n-- streaming (1 byte per feed call) --\n");
 ok &= run_streamed("byte-at-a-time IDCODE check",
  "ENDIR IDLE;\nENDDR IDLE;\nSTATE RESET;\nSIR 10 TDI (006);\n"
  "SDR 32 TDI (00000000) TDO (020A50DD) MASK (FFFFFFFF);\n", SVF_OK);
 ok &= run_streamed("byte-at-a-time mismatch",
  "STATE RESET;\nSIR 10 TDI (006);\nSDR 32 TDI (0) TDO (DEADBEEF) MASK (FFFFFFFF);\n",
  SVF_ERR_TDO_MISMATCH);

 printf("\n%s\n", ok?"ALL TESTS PASSED":"SOME TESTS FAILED");
 return ok?0:1;
}
