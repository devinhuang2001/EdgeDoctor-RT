#include "ed_baseline.h"
#include <math.h>
#include <string.h>
static float median(float *a,unsigned n){unsigned i,j;float t;for(i=1;i<n;i++){t=a[i];j=i;while(j&&a[j-1]>t){a[j]=a[j-1];j--;}a[j]=t;}return a[n/2];}
void ed_baseline_begin_at(ed_baseline_builder_t*b,const ed_asset_key_t*k,uint64_t now){if(!b)return;memset(b,0,sizeof* b);b->active=1u;b->started_mono_us=now;if(k)b->value.key=*k;b->value.schema_version=ED_MONITOR_SCHEMA_VERSION;b->value.policy_version=1u;}
void ed_baseline_begin(ed_baseline_builder_t*b,const ed_asset_key_t*k){ed_baseline_begin_at(b,k,0u);}
int ed_baseline_feed_ex(ed_baseline_builder_t*b,const ed_features_t*f,ed_quality_t q,uint8_t idle,uint8_t saturated,uint64_t now){if(!b||!f||!b->active||b->count>=60u)return-1;if(q!=ED_QUALITY_VALID||idle||saturated||!f->valid_mask)return-1;if(b->started_mono_us&&now>b->started_mono_us+120000000u){b->active=0u;b->timed_out=1u;return-2;}memcpy(b->samples[b->count],f->values,sizeof f->values);b->count++;return 0;}
int ed_baseline_feed(ed_baseline_builder_t*b,const ed_features_t*f){return ed_baseline_feed_ex(b,f,ED_QUALITY_VALID,0u,0u,0u);}
int ed_baseline_preview(const ed_baseline_builder_t*b,ed_baseline_t*out){unsigned j,i;float work[60];if(!b||!out||b->count<60u)return-1;*out=b->value;out->valid_windows=b->count;for(j=0;j<ED_FEATURE_COUNT;j++){for(i=0;i<b->count;i++)work[i]=b->samples[i][j];out->median[j]=median(work,b->count);for(i=0;i<b->count;i++)work[i]=fabsf(b->samples[i][j]-out->median[j]);out->mad[j]=median(work,b->count);}return 0;}
int ed_baseline_commit(ed_baseline_builder_t*b,ed_baseline_t*out){if(ed_baseline_preview(b,out)!=0)return-1;out->version++;b->active=0u;b->value=*out;return 0;}
float ed_baseline_deviation(const ed_baseline_t*b,const ed_features_t*f){unsigned i,used=0;float maxz=0.f,s,e;if(!b||!f)return NAN;for(i=0;i<ED_FEATURE_COUNT;i++){if(!(f->valid_mask&(1u<<i)))continue;used++;e=(i<=4u)?0.002f:((i==5u)?0.1f:((i==6u)?0.2f:0.000001f));s=fmaxf(1.4826f*b->mad[i],e);maxz=fmaxf(maxz,fabsf(f->values[i]-b->median[i])/s);}return used<12u?NAN:maxz;}
int ed_baseline_key_matches(const ed_baseline_t*b,const ed_asset_key_t*k){if(!b||!k)return 0;return strcmp(b->key.asset_id,k->asset_id)==0&&strcmp(b->key.sensor_profile_hash,k->sensor_profile_hash)==0&&strcmp(b->key.operating_profile_id,k->operating_profile_id)==0&&b->key.mount_revision==k->mount_revision;}
