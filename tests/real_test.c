/* Parse the user's real SVF end to end, fed in 64-byte chunks exactly as
   USB delivers it. TDO vectors are stripped beforehand because the
   simulated TAP cannot produce real programming data — this validates the
   PARSER against the real file, not the device model. */
#include <stdio.h>
#include <stdlib.h>
#include "shim.h"
#include "../firmware/jtag.h"
#include "../firmware/svf.h"
void svf_progress_hook(uint32_t s,uint32_t b){(void)s;(void)b;}
int main(int argc,char**argv){
 FILE*f=fopen(argv[1],"rb"); if(!f){perror("open");return 1;}
 fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
 unsigned char*buf=malloc(sz); fread(buf,1,sz,f); fclose(f);
 printf("File: %ld bytes\n",sz);
 svf_ctx_t c; svf_init(&c);
 svf_result_t r=SVF_OK; long off=0; const size_t CH=64;
 while(off<sz && r==SVF_OK){
  size_t n=(sz-off<(long)CH)?(size_t)(sz-off):CH;
  r=svf_feed(&c,buf+off,n); off+=n;
 }
 if(r==SVF_OK) r=svf_finish(&c);
 printf("Result       : %s\n",svf_result_str(r));
 if(r!=SVF_OK) printf("Detail       : %s (at statement %u)\n",c.error_detail,c.error_statement);
 printf("Statements   : %u\n",c.statements);
 printf("Bits shifted : %u\n",c.total_bits);
 return r==SVF_OK?0:1;
}
