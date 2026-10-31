/* Host-only render of the actual production object tree; dependencies below
 * intentionally report unavailable data. No hardware or network is touched. */
#include "../../firmware/m55/ui/edgedoctor_ui.c"
#include <stdlib.h>
#include <assert.h>
extern const unsigned char xiaozhi_font[];
extern const int xiaozhi_font_size;
static int fixture_enabled;
static ed_snapshot_t fixture_snapshot;
static ed_monitor_ui_state_t fixture_monitor;
int ed_monitor_app_read_snapshot(ed_snapshot_t *o) { if(!fixture_enabled)return -1;*o=fixture_snapshot;return 1; }
int ed_monitor_app_read_ui_state(ed_monitor_ui_state_t *o) { if(!fixture_enabled)return -1;*o=fixture_monitor;return 1; }
int ed_monitor_app_baseline_start(uint8_t a) { (void)a;return -1; }
int ed_monitor_app_baseline_commit(void) { return -1; }
int ed_monitor_app_post_ack(const ed_ack_request_t *r) { (void)r;return -1; }
int ed_monitor_app_read_active_event(ed_event_t *e) { (void)e;return -1; }
int ed_monitor_app_baseline_status(uint32_t *w,uint8_t *a,uint8_t *v,uint8_t *t) { *w=0;*a=*v=*t=0;return -1; }
int edgedoctor_app_read_result(edgedoctor_result_t *o) { (void)o;return -1; }
int edgedoctor_app_read_spectrum(edgedoctor_ui_spectrum_t *o) { (void)o;return -1; }
void edgedoctor_app_profile(edgedoctor_profile_snapshot_t *o) { memset(o,0,sizeof(*o)); }
int edgedoctor_app_is_replaying(void) { return 0; }
void edgedoctor_logger_stats(edgedoctor_logger_stats_t *o) { memset(o,0,sizeof(*o)); }
const char *edgedoctor_logger_path(void) { return "unavailable"; }
void ed_mqtt_get_state(ed_mqtt_state_t *o) { memset(o,0,sizeof(*o)); }
void ed_mqtt_get_ui_state(ed_mqtt_ui_state_t *o) { memset(o,0,sizeof(*o)); }
static uint32_t framebuffer[480*800], drawbuffer[480*800];
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{
    int y; uint32_t *src=(uint32_t *)p;
    for(y=a->y1;y<=a->y2;y++) { memcpy(framebuffer+y*480+a->x1,src,(a->x2-a->x1+1)*4);src+=a->x2-a->x1+1; }
    lv_display_flush_ready(d);
}
static void save(const char *path)
{
    FILE *f=fopen(path,"wb");unsigned i;
    if(!f) exit(2);
    fprintf(f,"P6\n480 800\n255\n");
    for(i=0;i<480*800;i++) { unsigned char b[3]={(framebuffer[i]>>16)&255,(framebuffer[i]>>8)&255,framebuffer[i]&255};fwrite(b,1,3,f); }
    fclose(f);
}
static void inspect(lv_obj_t *o,int depth)
{
    unsigned i;lv_area_t a;
    if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return;
    lv_obj_get_coords(o,&a);
    if(lv_obj_check_type(o,&lv_label_class))
        printf("label %d [%ld,%ld,%ld,%ld] %s\n",depth,(long)a.x1,(long)a.y1,(long)a.x2,(long)a.y2,lv_label_get_text(o));
    for(i=0;i<lv_obj_get_child_count(o);i++)inspect(lv_obj_get_child(o,i),depth+1);
}
static unsigned object_count(lv_obj_t *o)
{
    unsigned n=1,i;
    for(i=0;i<lv_obj_get_child_count(o);i++)n+=object_count(lv_obj_get_child(o,i));
    return n;
}
static void check_page(unsigned tab)
{
    lv_area_t a,b;lv_obj_t *page=lv_obj_get_child(lv_tabview_get_content(ui.tabview),tab);
    lv_obj_get_coords(page,&a);lv_obj_get_coords(ui.nav,&b);
    assert(a.x1>=0 && a.x2<480 && a.y2<b.y1);
    assert(b.y1==736 && b.y2==799);
}
static void check_horizontal(lv_obj_t *o)
{
    unsigned i;lv_area_t a;
    if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return;
    lv_obj_get_coords(o,&a);
    if(a.x1<0 || a.x2>=480) {
        fprintf(stderr,"Horizontal overflow: %ld..%ld\n",(long)a.x1,(long)a.x2);
        assert(0);
    }
    for(i=0;i<lv_obj_get_child_count(o);i++)check_horizontal(lv_obj_get_child(o,i));
}
int main(int argc,char **argv)
{
    unsigned i;char path[512];lv_display_t *d;lv_font_t *s,*m,*l;
    if(argc!=2)return 2;
    lv_init();d=lv_display_create(480,800);lv_display_set_color_format(d,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d,drawbuffer,NULL,sizeof(drawbuffer),LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d,flush);
    s=lv_tiny_ttf_create_data(xiaozhi_font,xiaozhi_font_size,16);
    m=lv_tiny_ttf_create_data(xiaozhi_font,xiaozhi_font_size,22);
    l=lv_tiny_ttf_create_data(xiaozhi_font,xiaozhi_font_size,28);
    if(!s||!m||!l)return 3;
    edgedoctor_ui_init(s,m,l);
    for(i=2;i<6;i++) {
        lv_tabview_set_active(ui.tabview,i,LV_ANIM_OFF);paint_navigation(i);
        lv_tick_inc(1200);edgedoctor_ui_process();lv_obj_update_layout(ui.root);lv_refr_now(d);
        snprintf(path,sizeof(path),"%s/tab-%u.ppm",argv[1],i);save(path);
        printf("PAGE %u\n",i);inspect(lv_obj_get_child(lv_tabview_get_content(ui.tabview),i),0);
        check_page(i);
    }
    fixture_enabled=1;
    fixture_monitor.baseline_valid=1;
    fixture_monitor.baseline.version=3;
    fixture_monitor.baseline.median[3]=0.017f;
    fixture_monitor.baseline_windows=60;
    fixture_monitor.event_count=3;
    fixture_snapshot.source=ED_SOURCE_LIVE_IMU;
    fixture_snapshot.quality=ED_QUALITY_VALID;
    fixture_snapshot.diagnosis=ED_DIAG_WITHIN_BASELINE;
    fixture_snapshot.baseline_version=3;
    strcpy(fixture_snapshot.key.asset_id,"电机 01");
    strcpy(fixture_snapshot.key.operating_profile_id,"工况 A");
    for(i=0;i<3;i++) {
        ed_event_t *e=&fixture_monitor.events[i];
        snprintf(e->event_id,sizeof e->event_id,"fixture-E%u",i+1);
        strcpy(e->key.asset_id,"电机 01");e->source=ED_SOURCE_LIVE_IMU;
        e->severity=i==0?ED_SEVERITY_ALARM:ED_SEVERITY_WARNING;
        e->condition=i==2?ED_CONDITION_RECOVERED:ED_CONDITION_ACTIVE;
        e->ack=i==1?ED_ACKNOWLEDGED:ED_UNACKNOWLEDGED;
        e->revision=1;e->baseline_version=3;e->durable=1;
    }
    for(i=1;i<=75;i++) {
        fixture_snapshot.snapshot_id=i;fixture_snapshot.mono_us=(uint64_t)i*60000000ULL;
        fixture_snapshot.features.values[3]=i==52?0.084f:0.017f+(i%5)*0.001f;
        fixture_snapshot.baseline_deviation=i==52?8.0f:1.2f;
        lv_tick_inc(1000);edgedoctor_ui_process();
    }
    for(i=2;i<6;i++) {
        lv_tabview_set_active(ui.tabview,i,LV_ANIM_OFF);paint_navigation(i);
        lv_tick_inc(1200);edgedoctor_ui_process();lv_obj_update_layout(ui.root);lv_refr_now(d);
        snprintf(path,sizeof(path),"%s/tab-%u-fixture.ppm",argv[1],i);save(path);check_page(i);
        check_horizontal(lv_obj_get_child(lv_tabview_get_content(ui.tabview),i));
    }
    lv_tabview_set_active(ui.tabview,2,LV_ANIM_OFF);paint_navigation(2);
    lv_obj_send_event(event_rows[0],LV_EVENT_CLICKED,NULL);
    lv_tick_inc(1200);edgedoctor_ui_process();lv_obj_update_layout(ui.root);
    assert(!lv_obj_has_flag(event_detail,LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(selected_event,"fixture-E1")==0);
    lv_obj_send_event(event_ack_button,LV_EVENT_CLICKED,NULL);
    assert(strstr(lv_label_get_text(event_action),"提交失败")!=NULL);
    assert(!lv_obj_has_state(event_ack_button,LV_STATE_DISABLED));
    check_horizontal(lv_obj_get_child(lv_tabview_get_content(ui.tabview),2));
    lv_tabview_set_active(ui.tabview,5,LV_ANIM_OFF);paint_navigation(5);
    lv_obj_remove_flag(system_details,LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(ui.root);
    check_horizontal(lv_obj_get_child(lv_tabview_get_content(ui.tabview),5));
    lv_obj_add_flag(system_details,LV_OBJ_FLAG_HIDDEN);
    printf("PASS: detail selection, failed acknowledgement retry, horizontal bounds\n");
    { unsigned before=object_count(ui.root);
      for(i=0;i<200;i++) {
        lv_tabview_set_active(ui.tabview,2+i%4,LV_ANIM_OFF);paint_navigation(2+i%4);
        lv_tick_inc(1200);edgedoctor_ui_process();lv_obj_update_layout(ui.root);
      }
      assert(before==object_count(ui.root));
      printf("PASS: 200 tab changes, stable %u objects, fixed bottom navigation\n",before);
    }
    return 0;
}
