#include "ed_mqtt_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static size_t field(const unsigned char *p, size_t at, const char *text)
{
    size_t n=((size_t)p[at]<<8)|p[at+1];
    assert(n==strlen(text)); assert(memcmp(p+at+2,text,n)==0);
    return at+2+n;
}
int main(void)
{
    ed_mqtt_config_t config={0};
    ed_mqtt_config_t loaded={0};
    unsigned char packet[512]; size_t length, at;
    strcpy(config.site_id,"site"); strcpy(config.device_id,"pump");
    strcpy(config.broker_host,"broker"); config.broker_port=8883; config.keepalive_s=30;
    assert(ed_mqtt_encode_connect(&config,packet,sizeof(packet),&length)==0);
    assert(packet[0]==0x10 && packet[1]==19 && packet[9]==0x02);
    assert(field(packet,12,"ed-pump")==length);
    strcpy(config.username,"operator"); strcpy(config.password,"secret");
    assert(ed_mqtt_decode_config(&config,sizeof(config),&loaded)==0);
    assert(strcmp(loaded.password,"secret")==0);
    assert(ed_mqtt_decode_config(&config,offsetof(ed_mqtt_config_t,username),&loaded)==0);
    assert(loaded.username[0]=='\0' && loaded.password[0]=='\0' && loaded.broker_port==8883);
    assert(ed_mqtt_decode_config(&config,sizeof(config)-1,&loaded)<0);
    assert(ed_mqtt_encode_connect(&config,packet,sizeof(packet),&length)==0);
    assert(packet[9]==0xc2);
    at=field(packet,12,"ed-pump"); at=field(packet,at,"operator");
    assert(field(packet,at,"secret")==length);
    memset(config.password,'p',sizeof(config.password)-1);
    assert(ed_mqtt_encode_connect(&config,packet,sizeof(packet),&length)==0);
    assert((packet[1]&0x80)!=0 && packet[10]==0xc2);
    assert(ed_mqtt_encode_connect(&config,packet,16,&length)<0);
    memset(config.username,'x',sizeof(config.username));
    assert(ed_mqtt_validate_config(&config)<0);
    memset(config.username,0,sizeof(config.username));
    assert(ed_mqtt_validate_config(&config)<0); /* password without username */
    puts("mqtt CONNECT credentials: passed"); return 0;
}
