#include "ed_voice_guidance.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){ed_snapshot_t s={0};ed_event_t e={0};char out[512];size_t n;s.snapshot_id=42;s.mono_us=9000;s.last_sample_mono_us=10000;s.quality=ED_QUALITY_VALID;s.diagnosis=ED_DIAG_UNKNOWN_ANOMALY;strcpy(e.event_id,"evt-42");assert(ed_voice_build_response(&s,&e,out,sizeof out,&n)==0);assert(strstr(out,"snapshot_id\":42")!=0);assert(strstr(out,"snapshot_age_us\":0")!=0);assert(strstr(out,"未知异常")!=0);assert(strstr(out,"read_only\":true")!=0);puts("voice guidance: all tests passed");return 0;}
