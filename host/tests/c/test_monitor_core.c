#include "ed_alarm.h"
#include "ed_quality.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static ed_event_t stored;
static int reject_once;
static int accept_store(void *user, const ed_event_t *e, const ed_ack_request_t *r) { (void)user; (void)r; stored=*e; return 0; }
static int reject_store(void *user, const ed_event_t *e, const ed_ack_request_t *r) { (void)user; (void)e; (void)r; if (reject_once) { reject_once=0; return -1; } stored=*e; return 0; }
static ed_snapshot_t snapshot(void) { ed_snapshot_t s; memset(&s,0,sizeof(s)); strcpy(s.key.asset_id,"pump"); s.quality=ED_QUALITY_VALID; s.diagnosis=ED_DIAG_UNKNOWN_ANOMALY; s.baseline_deviation=12; return s; }
static void tick(ed_alarm_t *c, ed_snapshot_t *s, unsigned count) { while(count--) { s->mono_us+=500000; assert(ed_alarm_step(c,s,s->mono_us)>=0); } }
static void bounds_quality(void) {
 ed_imu_block_t b; ed_quality_state_t q; ed_quality_input_t i; ed_monitor_policy_t p; uint8_t idle;
 memset(&b,0,sizeof(b)); b.sample_period_ns=4807692; assert(ed_imu_block_validate(&b)<0); b.count=32; assert(ed_imu_block_validate(&b)==0); b.count=33; assert(ed_imu_block_validate(&b)<0);
 ed_monitor_policy_default(&p); ed_quality_init(&q); memset(&i,0,sizeof(i)); i.configured=1; i.window_samples=208; i.dynamic_rms_g=.1f;
 assert(ed_quality_step(&q,&p,&i,&idle)==ED_QUALITY_VALID); i.now_us=200000; assert(ed_quality_step(&q,&p,&i,&idle)==ED_QUALITY_STALE);
 i.bus_failing=1; i.now_us=1000000; assert(ed_quality_step(&q,&p,&i,&idle)==ED_QUALITY_DISCONNECTED);
 i.bus_failing=0; i.last_sample_mono_us=i.now_us; i.saturated_samples=3; assert(ed_quality_step(&q,&p,&i,&idle)==ED_QUALITY_SATURATED);
 i.saturated_samples=0; i.gap=1; assert(ed_quality_step(&q,&p,&i,&idle)==ED_QUALITY_GAPPED); i.gap=0; i.window_samples=0; assert(ed_quality_step(&q,&p,&i,&idle)==ED_QUALITY_WARMUP);
 i.window_samples=208; i.dynamic_rms_g=NAN; assert(ed_quality_step(&q,&p,&i,&idle)==ED_QUALITY_CONFIG_ERROR);
 i.dynamic_rms_g=.001f; assert(ed_quality_step(&q,&p,&i,&idle)==ED_QUALITY_VALID && !idle); i.now_us+=10000000; i.last_sample_mono_us=i.now_us; assert(ed_quality_step(&q,&p,&i,&idle)==ED_QUALITY_VALID && idle);
}
static void events_ack(void) {
 ed_alarm_t c; ed_snapshot_t s=snapshot(); ed_event_t e; ed_ack_request_t a; ed_command_result_t r; char id[ED_EVENT_ID_MAX];
 assert(ed_alarm_init(&c,"dev","boot",7,1,accept_store,NULL)==0); tick(&c,&s,2); assert(c.next_event_seq==0); tick(&c,&s,1); strcpy(id,stored.event_id); assert(strcmp(id,"dev-7-1")==0);
 tick(&c,&s,1200); assert(ed_alarm_get_event(&c,id,&e)==0); assert(e.revision==1 && e.severity==ED_SEVERITY_ALARM);
 memset(&a,0,sizeof(a)); strcpy(a.command_id,"cmd"); strcpy(a.event_id,id); strcpy(a.actor,"operator"); strcpy(a.target_boot_id,"boot"); a.expected_revision=2;
 assert(ed_alarm_ack(&c,&a,&r)==0 && r.status==ED_COMMAND_CONFLICT); a.expected_revision=1;
 assert(ed_alarm_ack(&c,&a,&r)==0 && r.status==ED_COMMAND_PENDING); assert(ed_alarm_get_event(&c,id,&e)==0 && e.ack==ED_UNACKNOWLEDGED);
 assert(ed_alarm_ack(&c,&a,&r)==0 && r.status==ED_COMMAND_PENDING);
 assert(ed_alarm_complete(&c,id,2,1,&r)==0 && r.status==ED_COMMAND_OK); assert(ed_alarm_get_event(&c,id,&e)==0 && e.ack==ED_ACKNOWLEDGED && e.condition==ED_CONDITION_ACTIVE && e.revision==2);
 assert(ed_alarm_ack(&c,&a,&r)==0 && r.status==ED_COMMAND_ALREADY_ACKNOWLEDGED);
 s.baseline_deviation=0; tick(&c,&s,19); assert(ed_alarm_get_event(&c,id,&e)==0 && e.condition==ED_CONDITION_ACTIVE);
 s.quality=ED_QUALITY_STALE; tick(&c,&s,6); assert(ed_alarm_get_event(&c,id,&e)==0 && e.condition==ED_CONDITION_ACTIVE);
 s.quality=ED_QUALITY_VALID; tick(&c,&s,19); assert(ed_alarm_get_event(&c,id,&e)==0 && e.condition==ED_CONDITION_ACTIVE); tick(&c,&s,1); assert(ed_alarm_get_event(&c,id,&e)==0 && e.condition==ED_CONDITION_RECOVERED && e.revision==3);
}
static void trigger_requires_consecutive_windows(void) {
 ed_alarm_t c; ed_snapshot_t s=snapshot();
 assert(ed_alarm_init(&c,"dev","boot",9,1,accept_store,NULL)==0);
 tick(&c,&s,2); assert(c.next_event_seq==0);
 s.baseline_deviation=0; tick(&c,&s,1);
 s.baseline_deviation=12; tick(&c,&s,2); assert(c.next_event_seq==0);
 tick(&c,&s,1); assert(c.next_event_seq==1);
}
static void recovery_requires_low_deviation(void) {
 ed_alarm_t c; ed_snapshot_t s=snapshot(); ed_event_t e; char id[ED_EVENT_ID_MAX];
 assert(ed_alarm_init(&c,"dev","boot",10,1,accept_store,NULL)==0);
 tick(&c,&s,3); strcpy(id,stored.event_id);
 s.baseline_deviation=9; tick(&c,&s,20);
 assert(ed_alarm_get_event(&c,id,&e)==0 && e.condition==ED_CONDITION_ACTIVE);
 s.baseline_deviation=c.policy.recovery_deviation; tick(&c,&s,19);
 assert(ed_alarm_get_event(&c,id,&e)==0 && e.condition==ED_CONDITION_ACTIVE);
 tick(&c,&s,1); assert(ed_alarm_get_event(&c,id,&e)==0 && e.condition==ED_CONDITION_RECOVERED);
}
static void warning_then_alarm_upgrade_and_key_isolation(void) {
 ed_alarm_t c; ed_snapshot_t s=snapshot(); ed_event_t e; char id[ED_EVENT_ID_MAX];
 assert(ed_alarm_init(&c,"dev","boot",11,1,accept_store,NULL)==0);
 s.baseline_deviation=7; tick(&c,&s,2); assert(c.next_event_seq==0);
 tick(&c,&s,1); assert(c.next_event_seq==1); strcpy(id,stored.event_id);
 assert(ed_alarm_get_event(&c,id,&e)==0 && e.severity==ED_SEVERITY_WARNING);
 s.baseline_deviation=12; tick(&c,&s,1);
 assert(ed_alarm_get_event(&c,id,&e)==0 && e.severity==ED_SEVERITY_ALARM && e.revision==2);
 strcpy(s.key.sensor_profile_hash,"different"); s.baseline_deviation=12; tick(&c,&s,3);
 assert(c.next_event_seq==2);
}
static void isolation_failure(void) {
 ed_alarm_t c; ed_snapshot_t s=snapshot(); ed_ack_request_t a; ed_command_result_t r; ed_event_t e; char first[ED_EVENT_ID_MAX]; unsigned n;
 assert(ed_alarm_init(&c,"dev","boot",8,1,NULL,NULL)==0); s.source=ED_SOURCE_KAIST_REPLAY; tick(&c,&s,4); assert(c.next_event_seq==0);
 s.source=ED_SOURCE_LIVE_IMU; s.baseline_deviation=NAN; tick(&c,&s,4); assert(c.next_event_seq==0); s.baseline_deviation=12; tick(&c,&s,3);
 strcpy(first,c.slots[0].event.event_id); memset(&a,0,sizeof(a)); strcpy(a.event_id,first); strcpy(a.command_id,"cmd"); strcpy(a.actor,"op"); strcpy(a.target_boot_id,"boot"); a.expected_revision=1;
 assert(ed_alarm_ack(&c,&a,&r)==0 && r.status==ED_COMMAND_STORAGE_ERROR); assert(ed_alarm_get_event(&c,first,&e)==0 && !e.durable && e.ack==ED_UNACKNOWLEDGED);
 c.submit=accept_store; assert(ed_alarm_ack(&c,&a,&r)==0 && r.status==ED_COMMAND_PENDING); assert(ed_alarm_complete(&c,first,2,0,&r)==0 && r.status==ED_COMMAND_STORAGE_ERROR); assert(ed_alarm_get_event(&c,first,&e)==0 && e.revision==1 && !e.ack_pending);
 for(n=1;n<8;n++) { (void)snprintf(s.key.asset_id,sizeof(s.key.asset_id),"pump%u",n); tick(&c,&s,3); } assert(c.next_event_seq==8);
 strcpy(s.key.asset_id,"overflow"); tick(&c,&s,3); assert(c.next_event_seq==8 && c.overflow_count>0);
}
static void sensor_disconnect_recovery(void) {
 ed_alarm_t c; ed_asset_key_t key; ed_event_t e;
 memset(&key,0,sizeof(key)); strcpy(key.asset_id,"pump"); strcpy(key.sensor_profile_hash,"imu"); strcpy(key.operating_profile_id,"default");
 assert(ed_alarm_init(&c,"dev","boot",12,1,accept_store,NULL)==0);
 assert(ed_alarm_condition(&c,&key,ED_SOURCE_LIVE_IMU,ED_EVENT_SENSOR,1,ED_SEVERITY_ALARM,1000000)==0);
 assert(ed_alarm_get_event(&c,"dev-12-1",&e)==0 && e.kind==ED_EVENT_SENSOR && e.condition==ED_CONDITION_ACTIVE);
 assert(ed_alarm_condition(&c,&key,ED_SOURCE_LIVE_IMU,ED_EVENT_SENSOR,0,ED_SEVERITY_ALARM,2000000)==0);
 assert(ed_alarm_get_event(&c,"dev-12-1",&e)==0 && e.condition==ED_CONDITION_RECOVERED && e.revision==2);
}
static void rejected_first_revision_is_retried(void) {
 ed_alarm_t c; ed_snapshot_t s=snapshot();
 reject_once=1;
 assert(ed_alarm_init(&c,"dev","boot",13,1,reject_store,NULL)==0);
 s.mono_us+=500000; assert(ed_alarm_step(&c,&s,s.mono_us)==0);
 s.mono_us+=500000; assert(ed_alarm_step(&c,&s,s.mono_us)==0);
 s.mono_us+=500000; assert(ed_alarm_step(&c,&s,s.mono_us)==-3);
 assert(c.next_event_seq==1 && c.slots[0].used==0 && c.overflow_count==0);
 c.submit=accept_store;
 tick(&c,&s,3);
 assert(c.next_event_seq==2 && c.slots[0].used!=0 && stored.revision==1);
}
static void journal_commit_ack_two_phase(void) {
 ed_alarm_t c; ed_snapshot_t s=snapshot(); ed_event_t e; ed_ack_request_t a; ed_command_result_t r; char id[ED_EVENT_ID_MAX];
 assert(ed_alarm_init(&c,"dev","boot",14,1,accept_store,NULL)==0); tick(&c,&s,3); strcpy(id,stored.event_id);
 memset(&a,0,sizeof(a)); strcpy(a.command_id,"cmd-two-phase"); strcpy(a.event_id,id); a.expected_revision=1;
 assert(ed_alarm_ack(&c,&a,&r)==0 && r.status==ED_COMMAND_PENDING);
 assert(ed_alarm_mark_durable(&c,id,1)==0);
 assert(ed_alarm_complete_pending(&c,id,2,&r)==0 && r.status==ED_COMMAND_PENDING);
 assert(ed_alarm_get_event(&c,id,&e)==0 && e.ack==ED_ACKNOWLEDGED && e.revision==2 && !e.durable);
 assert(ed_alarm_mark_durable(&c,id,2)==0);
 assert(ed_alarm_get_event(&c,id,&e)==0 && e.durable);
}
static void invalid_identity_cannot_issue_event(void) {
 ed_alarm_t c; ed_snapshot_t s=snapshot();
 assert(ed_alarm_init(&c,"dev","boot",0,0,accept_store,NULL)==0);
 assert(ed_alarm_condition(&c,&s.key,ED_SOURCE_LIVE_IMU,ED_EVENT_SENSOR,1,ED_SEVERITY_ALARM,1)<0);
 assert(c.next_event_seq==0 && !c.slots[0].used);
}
static void event_identity_preserves_64_bits(void) {
 ed_alarm_t c; ed_snapshot_t s=snapshot();
 assert(ed_alarm_init(&c,"dev","boot",UINT64_MAX,1,accept_store,NULL)==0);
 c.next_event_seq=UINT64_C(4294967295);
 tick(&c,&s,3);
 assert(strcmp(stored.event_id,"dev-18446744073709551615-4294967296")==0);
}
int main(void) { event_identity_preserves_64_bits(); invalid_identity_cannot_issue_event(); bounds_quality(); trigger_requires_consecutive_windows(); recovery_requires_low_deviation(); warning_then_alarm_upgrade_and_key_isolation(); events_ack(); isolation_failure(); sensor_disconnect_recovery(); rejected_first_revision_is_retried(); journal_commit_ack_two_phase(); puts("monitor core: all tests passed"); return 0; }
