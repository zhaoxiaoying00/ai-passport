#include "walkman_online.h"
#include "online_setup.h"
#include "online_stream.h"
#include "online_tts.h"
#include "online_upload.h"
#include "online_queue.h"
#include "bsp_audio.h"
#include "esp_websocket_client.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define PCM_BYTES 640
#define FRAME_MAX 4096
#define WIRE_MAX 131072
#define OUTPUT_RATE 24000
static const char *TAG="walkman_online";
typedef online_packet packet;
typedef struct { unsigned kind,value; } command;
static QueueHandle_t commands;
static online_queue audio_queue;
static portMUX_TYPE audio_lock=portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static wn_state state={.phase=WN_UNCONFIGURED};
static atomic_bool got_ip,connected,session_ready,recording,allowed,hello,failed,synthetic,closing,warming;
static atomic_uint epoch;
static online_config_t config;
static esp_websocket_client_handle_t socket_handle;
static esp_transport_handle_t tls_transport,ws_transport;
static char *frame;
static online_stream stream;
static online_tts_stream tts_stream;
static bool tts_mode;
static atomic_bool text_ready,tts_ready;
static char tts_id[37];
static unsigned tts_samples,tts_sentence,tts_base_byte,tts_max_byte,tts_word_byte;
static bool tts_odd;
static uint8_t tts_byte;
static uint16_t page_bytes[ONLINE_CAPTION_PAGES];
/* Restore bounded recent context when switching between the dialogue and
 * timestamped synthesis sockets. No transcripts are written to flash. */
static char history[2048];
static unsigned history_used;
static online_restore restore;
static unsigned restore_offset;
static void remember(const char *text,bool assistant) {
    size_t n=strlen(text);if(!n)return;
    if(n+2>sizeof(history))return;
    while(history_used+n+2>sizeof(history)) {
        unsigned first=(unsigned)strlen(history+1)+2;
        memmove(history,history+first,history_used-first);history_used-=first;
    }
    history[history_used++]=assistant?'a':'u';
    memcpy(history+history_used,text,n+1);history_used+=(unsigned)n+1;
}
static unsigned stream_epoch;
static size_t wire_used;
static size_t used,base,chunk_used,chunk_total;
static bool assembling;
static int pending=-1;
static unsigned pending_value;
static online_upload *upload_buffer;
static bool flush_upload(void);
static atomic_uint received_samples;
static unsigned queued(void) {portENTER_CRITICAL(&audio_lock);unsigned n=audio_queue.count;portEXIT_CRITICAL(&audio_lock);return n;}
static unsigned queue_capacity(void) {portENTER_CRITICAL(&audio_lock);unsigned n=ONLINE_QUEUE_BASE+audio_queue.extra_count;portEXIT_CRITICAL(&audio_lock);return n;}
static bool queue_peek(packet *p) {portENTER_CRITICAL(&audio_lock);bool ok=online_queue_peek(&audio_queue,p);portEXIT_CRITICAL(&audio_lock);return ok;}
static bool queue_pop(packet *p,TickType_t wait) {
    TickType_t start=xTaskGetTickCount();
    do {
        portENTER_CRITICAL(&audio_lock);bool ok=online_queue_pop(&audio_queue,p);portEXIT_CRITICAL(&audio_lock);
        if(ok)return true;
        if(!wait || xTaskGetTickCount()-start>=wait)return false;
        vTaskDelay(1);
    }while(true);
}
static bool queue_push(const packet *p,TickType_t wait) {
    TickType_t start=xTaskGetTickCount();
    do {
        portENTER_CRITICAL(&audio_lock);
        bool cancelled=p->epoch!=atomic_load(&epoch) || !atomic_load(&allowed);
        bool ok=cancelled || online_queue_push(&audio_queue,p);
        portEXIT_CRITICAL(&audio_lock);
        if(ok)return true;
        if(!wait || xTaskGetTickCount()-start>=wait)return false;
        vTaskDelay(1);
    }while(true);
}
static void clear_audio_queue(void) {
    packet *extra[ONLINE_QUEUE_EXTRA];
    portENTER_CRITICAL(&audio_lock);memcpy(extra,audio_queue.extra,sizeof(extra));
    online_queue_reset(&audio_queue);online_queue_extend(&audio_queue,NULL,0);portEXIT_CRITICAL(&audio_lock);
    for(unsigned i=0;i<ONLINE_QUEUE_EXTRA;++i)free(extra[i]);
}
static void phase(unsigned p,const char *message) {
    portENTER_CRITICAL(&lock);
    if(!state.error_code || p==WN_ERROR){state.phase=p;if(message)snprintf(state.message,sizeof(state.message),"%s",message);}
    portEXIT_CRITICAL(&lock);
}
wn_state wn_status(void) {portENTER_CRITICAL(&lock);wn_state copy=state;portEXIT_CRITICAL(&lock);copy.testing=atomic_load(&synthetic);copy.recording=atomic_load(&recording) && !copy.testing;copy.captured=atomic_load(&received_samples);copy.wifi=atomic_load(&got_ip);copy.connected=atomic_load(&connected);copy.prepared=atomic_load(&warming) && atomic_load(&session_ready);return copy;}
static void fail(unsigned code,const char *why) {
    atomic_store(&recording,false);atomic_store(&allowed,false);atomic_fetch_add(&epoch,1);
    /* Background preparation is optional. An active request clears warming
     * before taking ownership, so its errors still reach the user. */
    if(atomic_load(&warming) && code!=WN_ERR_CONTROL && code!=WN_ERR_SYSTEM) {
        ESP_LOGI(TAG,"preparation unavailable code=%u",code);atomic_store(&failed,true);return;
    }
    portENTER_CRITICAL(&lock);
    bool first=state.error_code==WN_ERR_NONE;
    if(first){state.error_code=code;++state.failures;state.phase=WN_ERROR;snprintf(state.message,sizeof(state.message),"%s",why);}
    portEXIT_CRITICAL(&lock);
    if(first)ESP_LOGW(TAG,"failure code=%u heap=%u largest=%u",code,(unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    atomic_store(&failed,true);
}
static unsigned current_phase(void) {portENTER_CRITICAL(&lock);unsigned p=state.phase;portEXIT_CRITICAL(&lock);return p;}
static void clear_error(void) {portENTER_CRITICAL(&lock);state.error_code=WN_ERR_NONE;portEXIT_CRITICAL(&lock);}
void wn_command(unsigned kind,unsigned value) {
    if(kind==WN_CANCEL){atomic_store(&recording,false);atomic_store(&allowed,false);atomic_fetch_add(&epoch,1);}
    if(kind==WN_STOP)atomic_store(&recording,false);
    command c={kind,value};if(!commands || xQueueSend(commands,&c,0)!=pdTRUE) {
        portENTER_CRITICAL(&lock);++state.dropped;portEXIT_CRITICAL(&lock);fail(WN_ERR_CONTROL,"操作有点快，再试一次吧");
    }
}
static cJSON *event(const char *kind) {cJSON *j=cJSON_CreateObject();if(j)cJSON_AddStringToObject(j,"type",kind);return j;}
static bool send_json(cJSON *j) {
    if(!j){fail(WN_ERR_MEMORY,"网络内存不足");return false;}
    char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);if(!text){fail(WN_ERR_MEMORY,"网络内存不足");return false;}
    int len=strlen(text);bool ok=socket_handle && atomic_load(&connected) && esp_websocket_client_send_text(socket_handle,text,len,pdMS_TO_TICKS(2000))==len;
    free(text);if(!ok)fail(WN_ERR_SEND,"语音发送失败，请重试");return ok;
}
static const char *field(cJSON *j,const char *key) {cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);return cJSON_IsString(v)?v->valuestring:"";}
static void append_text(const char *delta) {
    portENTER_CRITICAL(&lock);size_t n=strlen(state.text),left=sizeof(state.text)-1-n;
    size_t take=strlen(delta);if(take>left)take=left;
    while(take && ((unsigned char)delta[take]&0xc0)==0x80)--take;
    memcpy(state.text+n,delta,take);state.text[n+take]=0;portEXIT_CRITICAL(&lock);
}
static bool tts_word(const char *prefix,const char *word,void *context) {
    (void)context;
    /* The service places sentence.index before its cumulative words array.
     * If it ever changes ordering, retain the page instead of guessing. */
    const char *sentence=strstr(prefix,"\"sentence\"");
    const char *index=sentence?strstr(sentence,"\"index\""):NULL;
    if(!index || !(index=strchr(index,':')))return true;
    unsigned id=(unsigned)strtoul(index+1,NULL,10);
    cJSON *j=cJSON_Parse(word);if(!j)return false;
    cJSON *begin=cJSON_GetObjectItemCaseSensitive(j,"begin_time");
    cJSON *at=cJSON_GetObjectItemCaseSensitive(j,"begin_index");
    const char *text=field(j,"text");
    if(!cJSON_IsNumber(begin) || !cJSON_IsNumber(at) || begin->valuedouble<0 || begin->valuedouble>3600000){cJSON_Delete(j);return false;}
    if(id!=tts_sentence){tts_sentence=id;tts_base_byte=tts_max_byte;}
    if(!at->valueint)tts_word_byte=tts_base_byte;
    size_t length=strlen(text);
    portENTER_CRITICAL(&lock);
    const char *found=length?strstr(state.text+tts_word_byte,text):NULL;
    if(found && (size_t)(found-(state.text+tts_word_byte))<24) {
        unsigned end=(unsigned)(found-state.text+length);
        /* The codec queues at most six 240-frame DMA descriptors. */
        unsigned sample=(unsigned)begin->valueint*24+1440;
        for(unsigned page=1;page<ONLINE_CAPTION_PAGES && page_bytes[page]!=UINT16_MAX;++page)
            if(end>page_bytes[page] && state.page_samples[page]==UINT32_MAX)state.page_samples[page]=sample;
        tts_word_byte=end;if(end>tts_max_byte)tts_max_byte=end;
    }
    portEXIT_CRITICAL(&lock);cJSON_Delete(j);return true;
}
static void tts_receive(void) {
    cJSON *j=cJSON_Parse(frame);if(!j){fail(WN_ERR_PROTOCOL,"语音数据不完整，请重试");return;}
    cJSON *header=cJSON_GetObjectItemCaseSensitive(j,"header");
    const char *kind=field(header,"event");
    if(!strcmp(kind,"task-started"))atomic_store(&tts_ready,true);
    else if(!strcmp(kind,"task-finished") && atomic_load(&allowed)) {
        if(tts_odd){cJSON_Delete(j);fail(WN_ERR_PROTOCOL,"语音格式不完整");return;}
        packet p={.epoch=atomic_load(&epoch),.done=true};
        if(!queue_push(&p,pdMS_TO_TICKS(150)))fail(WN_ERR_PLAYBACK_QUEUE,"语音结束确认失败");
    } else if(!strcmp(kind,"task-failed"))fail(WN_ERR_PROVIDER,"语音服务暂不可用，请重试");
    cJSON_Delete(j);
}
static void receive(void) {
    char *raw=NULL;size_t length=0;int audio=online_audio_event(frame,used,&raw,&length);
    if(audio<0){ESP_LOGW(TAG,"cloud JSON parse failed bytes=%u",(unsigned)used);fail(WN_ERR_PROTOCOL,"语音数据不完整，请重试");return;}
    if(audio==1) {
        if(!atomic_load(&allowed))return;
        packet p={.epoch=atomic_load(&epoch)};
        for(size_t at=0;at<length;) {
            size_t n=length-at;if(n>848)n=848;
            if(mbedtls_base64_decode((uint8_t*)p.pcm,sizeof(p.pcm),&p.len,(const unsigned char*)raw+at,n) || p.len%2){fail(WN_ERR_PROTOCOL,"语音格式不完整");break;}
            if(p.len && !queue_push(&p,pdMS_TO_TICKS(150))){fail(WN_ERR_PLAYBACK_QUEUE,"网络语音拥塞，请重试");break;}
            at+=n;
        }
        return;
    }
    if(used>4096){fail(WN_ERR_PROTOCOL,"语音数据不完整，请重试");return;}
    cJSON *j=cJSON_Parse(frame);if(!j){fail(WN_ERR_PROTOCOL,"语音数据不完整，请重试");return;}
    const char *type=field(j,"type");
    if(!strcmp(type,"session.updated")){portENTER_CRITICAL(&lock);restore.configured=true;portEXIT_CRITICAL(&lock);}
    else if(!strcmp(type,"conversation.item.created")) {
        cJSON *item=cJSON_GetObjectItemCaseSensitive(j,"item");unsigned number;
        if(sscanf(field(item,"id"),"restore_%u",&number)==1) {
            portENTER_CRITICAL(&lock);online_restore_ack(&restore,number);portEXIT_CRITICAL(&lock);
        }
    }
    else if(!strcmp(type,"conversation.item.input_audio_transcription.completed"))remember(field(j,"transcript"),false);
    else if(!strcmp(type,"error")) {
        cJSON *error=cJSON_GetObjectItemCaseSensitive(j,"error");
        const char *code=field(error,"code");
        ESP_LOGW(TAG,"provider request failed");fail(WN_ERR_PROVIDER,"语音服务暂不可用，请重试");atomic_store(&failed,true);
        (void)code;
    } else if(atomic_load(&allowed)) {
        if(!strcmp(type,"response.audio_transcript.delta") || !strcmp(type,"response.text.delta"))append_text(field(j,"delta"));
        else if(!strcmp(type,"response.done")) {
            cJSON *response=cJSON_GetObjectItemCaseSensitive(j,"response");
            if(!strcmp(field(response,"status"),"failed"))fail(WN_ERR_PROVIDER,"语音服务暂不可用，请重试");
            else atomic_store(&text_ready,true);
        }
    }
    cJSON_Delete(j);
}
static bool stream_pcm(const uint8_t *pcm,size_t length,void *context) {
    (void)context;
    if(!atomic_load(&allowed) || stream_epoch!=atomic_load(&epoch))return true;
    packet p={.epoch=stream_epoch,.len=length};memcpy(p.pcm,pcm,length);
    if(queue_push(&p,pdMS_TO_TICKS(150)))return true;
    if(!atomic_load(&allowed) || stream_epoch!=atomic_load(&epoch))return true;
    fail(WN_ERR_PLAYBACK_QUEUE,"网络语音拥塞，请重试");return false;
}
static void ws_event(void *arg,esp_event_base_t b,int32_t id,void *data) {
    (void)arg;(void)b;
    if(atomic_load(&closing))return;
    if(id==WEBSOCKET_EVENT_CONNECTED){int one=1;int fd=esp_transport_get_socket(ws_transport);if(fd<0 || setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))!=0){ESP_LOGW(TAG,"TCP_NODELAY setup failed");fail(WN_ERR_TRANSPORT,"语音连接失败，请重试");atomic_store(&failed,true);return;}ESP_LOGI(TAG,"cloud ready heap=%u largest=%u",(unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));frame=malloc(FRAME_MAX);if(!frame){ESP_LOGW(TAG,"cloud receive allocation failed");fail(WN_ERR_MEMORY,"网络内存不足");atomic_store(&failed,true);return;}atomic_store(&connected,true);atomic_store(&hello,true);used=0;assembling=false;}
    else if(id==WEBSOCKET_EVENT_DISCONNECTED || id==WEBSOCKET_EVENT_ERROR || id==WEBSOCKET_EVENT_CLOSED) {
        if(id==WEBSOCKET_EVENT_ERROR && data){esp_websocket_event_data_t *e=data;bool transport_error=e->error_handle.error_type==WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;ESP_LOGW(TAG,"cloud error type=%d tls=%d stack=%d http=%d socket=%d heap=%u largest=%u",e->error_handle.error_type,transport_error?e->error_handle.esp_tls_last_esp_err:0,transport_error?e->error_handle.esp_tls_stack_err:0,e->error_handle.esp_ws_handshake_status_code,transport_error?e->error_handle.esp_transport_sock_errno:0,(unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));}
        atomic_store(&connected,false);atomic_store(&session_ready,false);used=0;assembling=false;
        /* Idle provider retirement is harmless; an active turn needs an error.
         * In either case the worker must destroy the dead handle before reuse. */
        unsigned current=current_phase();
        if(current==WN_CONNECTING || current==WN_LISTENING || current==WN_THINKING || current==WN_SPEAKING)
            fail(WN_ERR_TRANSPORT,"语音连接断开，请重试");
        atomic_store(&failed,true);
    } else if(id==WEBSOCKET_EVENT_DATA) {
        if(atomic_load(&failed))return;
        portENTER_CRITICAL(&lock);state.ws_stack=uxTaskGetStackHighWaterMark(NULL);portEXIT_CRITICAL(&lock);
        esp_websocket_event_data_t *d=data;
        if(tts_mode && (d->op_code==2 || (d->op_code==0 && !assembling))) {
            if(!atomic_load(&allowed))return;
            if(d->data_len<0){fail(WN_ERR_PROTOCOL,"语音格式不完整");return;}
            for(unsigned at=0;at<(unsigned)d->data_len;) {
                packet p={.epoch=atomic_load(&epoch)};uint8_t *bytes=(uint8_t *)p.pcm;
                if(tts_odd){bytes[p.len++]=tts_byte;tts_odd=false;}
                unsigned n=(unsigned)d->data_len-at;if(n>PCM_BYTES-p.len)n=PCM_BYTES-p.len;
                memcpy(bytes+p.len,d->data_ptr+at,n);p.len+=n;at+=n;
                if(p.len%2){tts_odd=true;tts_byte=bytes[--p.len];}
                if(p.len && !queue_push(&p,pdMS_TO_TICKS(150))){fail(WN_ERR_PLAYBACK_QUEUE,"网络语音拥塞，请重试");return;}
                tts_samples+=p.len/2;
            }
            return;
        }
        if(d->op_code!=1 && d->op_code!=0)return;
        if(!frame || d->payload_offset<0 || d->data_len<0 || d->payload_len<0){fail(WN_ERR_PROTOCOL,"语音数据不完整");return;}
        if(!d->payload_offset) {
            if(d->op_code==1){if(assembling){fail(WN_ERR_PROTOCOL,"语音分片顺序错误");return;}wire_used=0;assembling=true;stream_epoch=atomic_load(&epoch);if(tts_mode)online_tts_begin(&tts_stream,frame,FRAME_MAX,tts_word,NULL);else online_stream_begin(&stream,frame,FRAME_MAX,stream_pcm,NULL);}
            else if(!assembling || chunk_used!=chunk_total){fail(WN_ERR_PROTOCOL,"语音分片顺序错误");return;}
            base=wire_used;chunk_used=0;chunk_total=d->payload_len;
        }
        if(!assembling || base>=WIRE_MAX || chunk_total!=(size_t)d->payload_len || !online_fragment(&chunk_used,WIRE_MAX-base,d->payload_offset,d->data_len,d->payload_len)){fail(WN_ERR_PROTOCOL,"语音消息过长，请重试");return;}
        if(!(tts_mode?online_tts_feed(&tts_stream,d->data_ptr,d->data_len):online_stream_feed(&stream,d->data_ptr,d->data_len))){ESP_LOGW(TAG,"cloud stream failure reason=%u metadata=%u wire=%u depth=%u audio=%d",stream.error,(unsigned)stream.used,(unsigned)wire_used,stream.depth,stream.audio);fail(WN_ERR_PROTOCOL,"语音数据不完整，请重试");atomic_store(&failed,true);return;}
        wire_used=base+chunk_used;
        if(chunk_used==chunk_total && d->fin){if(!(tts_mode?online_tts_end(&tts_stream):online_stream_end(&stream))){fail(WN_ERR_PROTOCOL,"语音数据不完整，请重试");atomic_store(&failed,true);return;}used=tts_mode?tts_stream.used:stream.used;if(tts_mode)tts_receive();else receive();used=0;assembling=false;}
    }
}
static void wifi_event(void *arg,esp_event_base_t b,int32_t id,void *data) {
    (void)arg;
    if(b==IP_EVENT && id==IP_EVENT_STA_GOT_IP){atomic_store(&got_ip,true);portENTER_CRITICAL(&lock);state.wifi_reason=0;portEXIT_CRITICAL(&lock);}
    if(b==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *info=data;
        portENTER_CRITICAL(&lock);state.wifi_reason=info?info->reason:0;portEXIT_CRITICAL(&lock);
        atomic_store(&got_ip,false);atomic_store(&recording,false);atomic_store(&allowed,false);
        unsigned current=current_phase();
        if(current==WN_LISTENING || current==WN_THINKING || current==WN_SPEAKING)
            fail(WN_ERR_TRANSPORT,"Wi-Fi 断开，请重连后再试");
        else {phase(WN_CONNECTING,"Wi-Fi 断开，正在重连");atomic_store(&failed,true);}
    }
}
static bool read_blob(const char *space,online_config_t *c) {
    nvs_handle_t n;size_t size=sizeof(*c);if(nvs_open(space,NVS_READONLY,&n)!=ESP_OK)return false;
    esp_err_t e=nvs_get_blob(n,"config",c,&size);nvs_close(n);
    return e==ESP_OK && size==sizeof(*c) && memchr(c->ssid,0,sizeof(c->ssid)) && memchr(c->password,0,sizeof(c->password)) && c->ssid[0];
}
bool wn_configure_json(const char *raw) {
    cJSON *j=cJSON_Parse(raw);if(!j)return false;online_config_t c={.version=1};
    if(strlen(field(j,"ssid"))>=sizeof(c.ssid) || strlen(field(j,"password"))>=sizeof(c.password) || strlen(field(j,"api_key"))>=sizeof(c.token) || strlen(field(j,"model"))>96){cJSON_Delete(j);return false;}
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"setup"))) {
        const char *key=field(j,"api_key");bool valid=strlen(key)>=12 && strlen(key)<193 && !strpbrk(key,"\r\n\t ");
        nvs_handle_t n;esp_err_t e=valid?nvs_open("wm_online",NVS_READWRITE,&n):ESP_ERR_INVALID_ARG;
        if(e==ESP_OK){e=nvs_set_str(n,"default_key",key);if(e==ESP_OK)e=nvs_set_u8(n,"setup",1);if(e==ESP_OK)e=nvs_commit(n);nvs_close(n);}
        cJSON_Delete(j);return e==ESP_OK;
    }
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"reuse_wifi"))) {
        if(!read_blob("wm_online",&c) && !read_blob("bean_online",&c)){cJSON_Delete(j);return false;}
    } else {snprintf(c.ssid,sizeof(c.ssid),"%s",field(j,"ssid"));snprintf(c.password,sizeof(c.password),"%s",field(j,"password"));}
    c.version=1;snprintf(c.token,sizeof(c.token),"%s",field(j,"api_key"));
    snprintf(c.url,sizeof(c.url),"wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=%s",*field(j,"model")?field(j,"model"):"qwen-audio-3.0-realtime-plus");cJSON_Delete(j);
    if(!online_config_valid(&c)){memset(&c,0,sizeof(c));return false;}
    nvs_handle_t n;esp_err_t e=nvs_open("wm_online",NVS_READWRITE,&n);
    if(e==ESP_OK){e=nvs_set_blob(n,"config",&c,sizeof(c));if(e==ESP_OK)e=nvs_commit(n);nvs_close(n);}memset(&c,0,sizeof(c));return e==ESP_OK;
}
static void configure_session(void) {
    unsigned count=0;for(unsigned at=0;at<history_used;){at+=(unsigned)strlen(history+at+1)+2;++count;}
    portENTER_CRITICAL(&lock);restore=(online_restore){.total=count};portEXIT_CRITICAL(&lock);restore_offset=0;
    cJSON *j=event("session.update"),*s=cJSON_AddObjectToObject(j,"session");
    cJSON *m=cJSON_AddArrayToObject(s,"modalities");cJSON_AddItemToArray(m,cJSON_CreateString("text"));
    cJSON_AddStringToObject(s,"voice","longanlingxin");cJSON_AddBoolToObject(s,"enable_speech_emotion",true);
    cJSON_AddStringToObject(s,"instructions","你是元气随身听，一个俏皮温暖的语音伙伴。用自然中文口语、轻快可爱的语气，针对对方刚说的具体事情给出真诚回应和鼓励。通常每次两三句、六十字以内；对方明确要求长内容时可适当展开。不要称呼姓名或同学，不要强行积极或说教，不要假装真人，不要声称做了现实世界的事。对方难过时先理解感受。停顿自然连贯，不要逐字念。");
    cJSON *transcription=cJSON_AddObjectToObject(s,"input_audio_transcription");cJSON_AddStringToObject(transcription,"model","gummy-realtime-v1");
    cJSON_AddNullToObject(s,"turn_detection");cJSON_AddStringToObject(s,"input_audio_format","pcm");cJSON_AddStringToObject(s,"output_audio_format","pcm");cJSON_AddNumberToObject(s,"max_history_turns",6);send_json(j);
}
static void restore_history(void) {
    portENTER_CRITICAL(&lock);
    bool send=online_restore_send(&restore),ready=online_restore_ready(&restore);unsigned number=restore.sent;
    portEXIT_CRITICAL(&lock);
    if(ready){atomic_store(&session_ready,true);phase(WN_READY,"准备好听你说啦");return;}
    if(send) {
        bool assistant=history[restore_offset++]=='a';const char *text=history+restore_offset;restore_offset+=(unsigned)strlen(text)+1;
        cJSON *j=event("conversation.item.create");cJSON *item=cJSON_AddObjectToObject(j,"item");
        char id[32];snprintf(id,sizeof(id),"restore_%u",number);cJSON_AddStringToObject(item,"id",id);
        cJSON_AddStringToObject(item,"type","message");cJSON_AddStringToObject(item,"role",assistant?"assistant":"user");
        cJSON *content=cJSON_AddArrayToObject(item,"content"),*entry=cJSON_CreateObject();
        cJSON_AddStringToObject(entry,"type",assistant?"output_text":"input_text");cJSON_AddStringToObject(entry,"text",text);cJSON_AddItemToArray(content,entry);
        send_json(j);
    }
}
static bool prepare_playback(void) {
    ESP_LOGI(TAG,"playback reserve packets=%u packet_bytes=%u heap=%u largest=%u",ONLINE_QUEUE_EXTRA,(unsigned)sizeof(packet),(unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    /* Codec/UI allocations fragment the heap during capture. Requiring one
     * 10 KiB extent here failed despite more than 23 KiB total being free. */
    packet *extra[ONLINE_QUEUE_EXTRA]={0};
    for(unsigned i=0;i<ONLINE_QUEUE_EXTRA;++i) {
        extra[i]=malloc(sizeof(*extra[i]));
        if(!extra[i]) {
            for(unsigned j=0;j<i;++j)free(extra[j]);
            fail(WN_ERR_MEMORY,"网络内存不足");return false;
        }
    }
    portENTER_CRITICAL(&audio_lock);bool ok=online_queue_extend(&audio_queue,extra,ONLINE_QUEUE_EXTRA);portEXIT_CRITICAL(&audio_lock);
    if(!ok){for(unsigned i=0;i<ONLINE_QUEUE_EXTRA;++i)free(extra[i]);fail(WN_ERR_PROTOCOL,"录音发送失败，请重试");}
    return ok;
}
static void begin(unsigned kind,unsigned value) {
    atomic_fetch_add(&epoch,1);clear_audio_queue();received_samples=0;
    free(upload_buffer);upload_buffer=NULL;
    portENTER_CRITICAL(&lock);++state.reply_id;state.reply_played=0;state.reply_finished=false;memset(state.page_samples,0xff,sizeof(state.page_samples));state.page_samples[0]=0;state.text[0]=0;state.seconds=0;state.uploaded=0;state.upload_ms=0;portEXIT_CRITICAL(&lock);atomic_store(&allowed,true);
    atomic_store(&text_ready,false);atomic_store(&synthetic,kind==WN_PROBE);
    if(kind==WN_RECORD || kind==WN_PROBE) {
        upload_buffer=malloc(sizeof(*upload_buffer));
        if(!upload_buffer){fail(WN_ERR_MEMORY,"网络内存不足");return;}
        online_upload_begin(upload_buffer);
        if(!send_json(event("input_audio_buffer.clear")))return;
        atomic_store(&recording,true);phase(WN_LISTENING,"我在听，说完按确定");
    }
    else {
        static const char *prompts[]={"今天需要一点鼓励，给我一句俏皮又真诚的夸夸。","我今天有点累，请温柔地陪我放松一下。","讲一个约一百字的小蜗牛的温柔小故事，包含一句用中文引号括起来的对话。"};
        remember(prompts[value%3],false);
        cJSON *j=event("conversation.item.create"),*item=cJSON_AddObjectToObject(j,"item");cJSON_AddStringToObject(item,"type","message");cJSON_AddStringToObject(item,"role","user");
        cJSON *content=cJSON_AddArrayToObject(item,"content"),*text=cJSON_CreateObject();cJSON_AddStringToObject(text,"type","input_text");cJSON_AddStringToObject(text,"text",prompts[value%3]);cJSON_AddItemToArray(content,text);if(!send_json(j) || !send_json(event("response.create")))return;phase(WN_THINKING,"给我一点点时间");
    }
}
static void close_socket(void) {
    bool had_socket=socket_handle!=NULL;
    atomic_store(&closing,true);
    atomic_store(&allowed,false);atomic_store(&recording,false);atomic_store(&synthetic,false);atomic_store(&session_ready,false);atomic_fetch_add(&epoch,1);
    /* Wake the client's 1 s read poll before joining it. The closing flag
     * distinguishes intentional teardown from an interrupted conversation. */
    if(ws_transport){int fd=esp_transport_get_socket(ws_transport);if(fd>=0)shutdown(fd,SHUT_RDWR);}
    if(socket_handle){esp_websocket_client_stop(socket_handle);esp_websocket_client_destroy(socket_handle);socket_handle=NULL;}
    if(ws_transport){esp_transport_destroy(ws_transport);ws_transport=NULL;}
    if(tls_transport){esp_transport_destroy(tls_transport);tls_transport=NULL;}
    free(frame);frame=NULL;free(upload_buffer);upload_buffer=NULL;
    atomic_store(&connected,false);atomic_store(&warming,false);atomic_store(&hello,false);atomic_store(&failed,false);atomic_store(&tts_ready,false);atomic_store(&text_ready,false);clear_audio_queue();
    /* Let the idle task reclaim the deleted WebSocket stack before another
     * TLS handshake allocates into the same small, fragmented heap. */
    if(had_socket)vTaskDelay(pdMS_TO_TICKS(20));
    atomic_store(&closing,false);
}
static bool open_socket(void) {
    char headers[240];snprintf(headers,sizeof(headers),"Authorization: Bearer %s\r\n",config.token);
    tls_transport=esp_transport_ssl_init();if(!tls_transport)return false;
    esp_transport_ssl_crt_bundle_attach(tls_transport,esp_crt_bundle_attach);esp_transport_set_default_port(tls_transport,443);
    ws_transport=esp_transport_ws_init(tls_transport);if(!ws_transport)return false;
    esp_transport_set_default_port(ws_transport,443);
    const char *url=tts_mode?"wss://dashscope.aliyuncs.com/api-ws/v1/inference":config.url;
    const char *path=strchr(url+6,'/');
    esp_transport_ws_config_t transport_config={.ws_path=path,.headers=headers,.propagate_control_frames=true};
    if(esp_transport_ws_set_config(ws_transport,&transport_config)!=ESP_OK)return false;
    /* Recording messages fit both a WebSocket frame and the 2 KiB TLS
     * output record. Keep synthesis buffering independent from upload. */
    esp_websocket_client_config_t ws={.uri=url,.ext_transport=ws_transport,.buffer_size=tts_mode?4608:ONLINE_UPLOAD_WIRE_MAX,.task_stack=6144,
        .disable_auto_reconnect=true,.network_timeout_ms=8000,.ping_interval_sec=15,.crt_bundle_attach=esp_crt_bundle_attach};
    ESP_LOGI(TAG,"cloud start heap=%u largest=%u",(unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    socket_handle=esp_websocket_client_init(&ws);memset(headers,0,sizeof(headers));
    if(!socket_handle)return false;
    portENTER_CRITICAL(&lock);restore=(online_restore){0};portEXIT_CRITICAL(&lock);
    if(esp_websocket_register_events(socket_handle,WEBSOCKET_EVENT_ANY,ws_event,NULL)!=ESP_OK)return false;
    esp_err_t result=esp_websocket_client_start(socket_handle);if(result!=ESP_OK)ESP_LOGW(TAG,"cloud start failed code=%d",result);return result==ESP_OK;
}
static cJSON *tts_event(const char *action) {
    cJSON *j=cJSON_CreateObject(),*header=cJSON_AddObjectToObject(j,"header");
    cJSON_AddStringToObject(header,"action",action);cJSON_AddStringToObject(header,"task_id",tts_id);
    cJSON_AddStringToObject(header,"streaming","duplex");cJSON_AddObjectToObject(j,"payload");return j;
}
static void tts_start(void) {
    snprintf(tts_id,sizeof(tts_id),"%08lx-%04x-4000-8000-%08lx%04x",(unsigned long)esp_random(),0x1234,(unsigned long)esp_random(),0x5678);
    cJSON *j=tts_event("run-task"),*p=cJSON_GetObjectItemCaseSensitive(j,"payload");
    cJSON_AddStringToObject(p,"task_group","audio");cJSON_AddStringToObject(p,"task","tts");
    cJSON_AddStringToObject(p,"function","SpeechSynthesizer");cJSON_AddStringToObject(p,"model","qwen-audio-3.0-tts-plus");
    cJSON_AddObjectToObject(p,"input");cJSON *params=cJSON_AddObjectToObject(p,"parameters");
    cJSON_AddStringToObject(params,"text_type","PlainText");cJSON_AddStringToObject(params,"voice","longanlingxin");
    cJSON_AddStringToObject(params,"format","pcm");cJSON_AddNumberToObject(params,"sample_rate",OUTPUT_RATE);
    cJSON_AddBoolToObject(params,"word_timestamp_enabled",true);
    cJSON_AddStringToObject(params,"instruction","用轻快温暖的语气自然地说，像朋友一样，停顿连贯，不要逐字念。");send_json(j);
}
static void tts_send_text(void) {
    if(!prepare_playback())return;
    cJSON *j=tts_event("continue-task"),*p=cJSON_GetObjectItemCaseSensitive(j,"payload"),*input=cJSON_AddObjectToObject(p,"input");
    /* Text is immutable between realtime response.done and the next begin. */
    cJSON_AddStringToObject(input,"text",state.text);
    if(!send_json(j))return;
    j=tts_event("finish-task");p=cJSON_GetObjectItemCaseSensitive(j,"payload");cJSON_AddObjectToObject(p,"input");send_json(j);
}
static bool start_synthesis(void) {
    close_socket();tts_mode=true;
    if(!state.text[0]){fail(WN_ERR_PROVIDER,"没有听清楚，再说一次吧");return false;}
    remember(state.text,true);tts_odd=false;tts_samples=tts_base_byte=tts_max_byte=tts_word_byte=0;tts_sentence=0;
    memset(page_bytes,0xff,sizeof(page_bytes));
    char lines[3][ONLINE_LINE_BYTES];unsigned count=online_caption_page(state.text,0,lines);
    for(unsigned page=1;page<count && page<ONLINE_CAPTION_PAGES;++page)page_bytes[page]=(uint16_t)online_caption_boundary(state.text,page);
    atomic_store(&allowed,true);phase(WN_THINKING,"好心情马上送达");return open_socket();
}
static bool flush_upload(void) {
    if(!upload_buffer || !upload_buffer->samples_bytes)return true;
    size_t length=online_upload_finish(upload_buffer);
    if(!length){fail(WN_ERR_PROTOCOL,"录音编码失败");return false;}
    int64_t started=esp_timer_get_time();
    bool ok=socket_handle && esp_websocket_client_send_text(socket_handle,upload_buffer->json,length,pdMS_TO_TICKS(2000))==(int)length;
    unsigned duration=(unsigned)((esp_timer_get_time()-started)/1000);
    if(duration>100){wifi_ap_record_t ap={0};esp_wifi_sta_get_ap_info(&ap);ESP_LOGW(TAG,"upload delayed ms=%u queue=%u heap=%u largest=%u rssi=%d",duration,queued(),(unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),ap.rssi);}
    portENTER_CRITICAL(&lock);if(ok)state.uploaded+=upload_buffer->samples_bytes/2;if(duration>state.upload_ms)state.upload_ms=duration;portEXIT_CRITICAL(&lock);
    online_upload_begin(upload_buffer);
    if(!ok)fail(WN_ERR_SEND,"录音发送失败，请重试");
    return ok;
}
static void upload(packet *p) {
    if(p->epoch!=atomic_load(&epoch) || !p->input || !upload_buffer || atomic_load(&failed))return;
    if(upload_buffer->samples_bytes+p->len>ONLINE_UPLOAD_PCM_MAX && !flush_upload())return;
    if(!online_upload_append(upload_buffer,(const uint8_t*)p->pcm,p->len)){fail(WN_ERR_PROTOCOL,"录音编码失败");return;}
    if(upload_buffer->samples_bytes>=ONLINE_UPLOAD_PCM_MAX)flush_upload();
}
static void network_task(void *arg) {
    (void)arg;
    if(esp_netif_init()!=ESP_OK || esp_event_loop_create_default()!=ESP_OK){fail(WN_ERR_SYSTEM,"网络初始化失败");vTaskDelete(NULL);return;}
    nvs_handle_t n;uint8_t setup=0;
    if(nvs_open("wm_online",NVS_READWRITE,&n)==ESP_OK){nvs_get_u8(n,"setup",&setup);if(setup){nvs_erase_key(n,"setup");nvs_commit(n);}nvs_close(n);}
    if(setup) {
        char info[128];esp_err_t e=online_start_setup(info,sizeof(info));phase(e==ESP_OK?WN_SETUP:WN_ERROR,e==ESP_OK?info:"配网启动失败");vTaskDelete(NULL);return;
    }
    bool configured=read_blob("wm_online",&config) && online_config_valid(&config);
    portENTER_CRITICAL(&lock);state.configured=configured;portEXIT_CRITICAL(&lock);
    if(configured) {
        esp_netif_create_default_wifi_sta();wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();wifi_config_t wifi={0};
        memcpy(wifi.sta.ssid,config.ssid,strlen(config.ssid));memcpy(wifi.sta.password,config.password,strlen(config.password));
        if(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL)!=ESP_OK ||
           esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL)!=ESP_OK || esp_wifi_init(&init)!=ESP_OK ||
           esp_wifi_set_storage(WIFI_STORAGE_RAM)!=ESP_OK || esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK ||
           esp_wifi_set_config(WIFI_IF_STA,&wifi)!=ESP_OK || esp_wifi_start()!=ESP_OK){fail(WN_ERR_SYSTEM,"Wi-Fi 初始化失败");vTaskDelete(NULL);return;}
        memset(wifi.sta.password,0,sizeof(wifi.sta.password));memset(config.password,0,sizeof(config.password));
        esp_wifi_set_ps(WIFI_PS_NONE);phase(WN_CONNECTING,"正在连接 Wi-Fi");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);esp_sntp_setservername(0,"ntp.aliyun.com");
    } else phase(WN_UNCONFIGURED,"先到网络设置连接 Wi-Fi");
    bool clock_started=false;int64_t retry=0,deadline=0,idle_since=0;
    for(;;) {
        command c;
        while(xQueueReceive(commands,&c,0)==pdTRUE) {
            if(c.kind==WN_CONFIGURE) {
                if(nvs_open("wm_online",NVS_READWRITE,&n)==ESP_OK){nvs_set_u8(n,"setup",1);nvs_commit(n);nvs_close(n);esp_restart();}
                fail(WN_ERR_SYSTEM,"暂时无法进入配置");continue;
            }
            if(c.kind==WN_CANCEL){pending=-1;deadline=0;close_socket();clear_error();phase(configured?WN_READY:WN_UNCONFIGURED,"准备好听你说啦");continue;}
            if(c.kind==WN_DROP_PROBE) {
                /* Fault injection only during synthetic diagnostics; never drop a real conversation. */
                if(atomic_load(&synthetic) && atomic_load(&connected) && ws_transport){int fd=esp_transport_get_socket(ws_transport);if(fd>=0)shutdown(fd,SHUT_RDWR);}
                continue;
            }
            if(!configured){phase(WN_UNCONFIGURED,"先到网络设置完成连接");continue;}
            if(c.kind==WN_STOP) {
                packet p;while(queue_pop(&p,0))upload(&p);bool uploaded=flush_upload() && !atomic_load(&failed);
                free(upload_buffer);upload_buffer=NULL;
                if(!uploaded){fail(WN_ERR_SEND,"录音发送失败，请重试");continue;}
                if(received_samples<4000){fail(WN_ERR_SHORT_RECORD,"这次太短啦，再说一次吧");continue;}
                if(atomic_load(&synthetic)){close_socket();phase(WN_READY,"准备好听你说啦");deadline=0;continue;}
                if(!send_json(event("input_audio_buffer.commit")) || !send_json(event("response.create")))continue;
                phase(WN_THINKING,"让我想想怎么回应你");deadline=esp_timer_get_time()+60000000;continue;
            }
            if(c.kind==WN_RECORD || c.kind==WN_QUICK || c.kind==WN_PROBE) {
                unsigned now=current_phase();
                if(tts_mode || now==WN_THINKING || now==WN_SPEAKING || now==WN_LISTENING || now==WN_ERROR || atomic_load(&failed) || (socket_handle && !atomic_load(&connected) && !atomic_load(&warming)))close_socket();
                atomic_store(&warming,false);tts_mode=false;clear_error();
                pending=(int)c.kind;pending_value=c.value;phase(WN_CONNECTING,"正在准备语音陪伴");deadline=esp_timer_get_time()+30000000;
            }
        }
        if(configured && !atomic_load(&got_ip)) {
            if(esp_timer_get_time()-retry>5000000 || !retry){esp_wifi_connect();retry=esp_timer_get_time();}
        } else if(configured && !clock_started){esp_sntp_init();clock_started=true;phase(WN_READY,"准备好听你说啦");}
        if(pending>=0 && atomic_load(&got_ip)) {
            if(time(NULL)<1704067200)phase(WN_CONNECTING,"正在同步时间");
            else if(!socket_handle && !open_socket()){fail(WN_ERR_TRANSPORT,"语音连接失败，请重试");pending=-1;close_socket();}
            if(!atomic_load(&failed) && atomic_load(&session_ready)){unsigned kind=(unsigned)pending;pending=-1;begin(kind,pending_value);deadline=esp_timer_get_time()+60000000;}
        }
        if(!atomic_load(&failed) && atomic_exchange(&hello,false)){if(tts_mode)tts_start();else configure_session();}
        if(!tts_mode && atomic_load(&connected) && !atomic_load(&failed) && !atomic_load(&session_ready))restore_history();
        if(!atomic_load(&failed) && atomic_exchange(&text_ready,false)) {
            if(!start_synthesis())fail(WN_ERR_TRANSPORT,"语音连接失败，请重试");
            deadline=esp_timer_get_time()+60000000;
        }
        if(!atomic_load(&failed) && atomic_exchange(&tts_ready,false))tts_send_text();
        packet p;
        for(unsigned batch=0;batch<4 && !atomic_load(&failed) && upload_buffer;++batch) {
            if(!queue_pop(&p,0))break;
            upload(&p);
        }
        if(atomic_load(&failed)){pending=-1;close_socket();deadline=0;}
        unsigned snapshot=current_phase();int64_t now=esp_timer_get_time();
        /* Only an outstanding request owns a timeout. Idle Wi-Fi recovery
         * must not inherit the deadline of an already completed reply. */
        if(pending<0 && snapshot!=WN_THINKING)deadline=0;
        if(pending<0 && !socket_handle && atomic_load(&got_ip) && snapshot==WN_CONNECTING) {
            phase(WN_READY,"准备好听你说啦");snapshot=current_phase();
        }
        if(snapshot==WN_READY) {
            if(!idle_since)idle_since=now;
            if(socket_handle && tts_mode) {
                /* Free synthesis and playback storage first; never keep two
                 * TLS sessions alive on this board. No mic or reply is started. */
                close_socket();tts_mode=false;atomic_store(&warming,true);
                if(!open_socket())close_socket();
                idle_since=esp_timer_get_time();
            } else if(socket_handle && now-idle_since>20000000) {
                close_socket();phase(WN_READY,"准备好听你说啦");
            }
        } else idle_since=0;
        if(deadline && now>deadline && (pending>=0 || snapshot==WN_THINKING)){pending=-1;close_socket();fail(WN_ERR_TIMEOUT,"等待有点久，按确定重试");deadline=0;}
        portENTER_CRITICAL(&lock);state.net_stack=uxTaskGetStackHighWaterMark(NULL);portEXIT_CRITICAL(&lock);
        /* Drain queued capture promptly; do not add a fixed 5 ms delay to
         * every small upload while the microphone keeps producing data. */
        vTaskDelay(pdMS_TO_TICKS(upload_buffer && queued()?1:5));
    }
}
bool wn_audio_step(unsigned volume) {
    static packet p;
    static unsigned current_epoch,skip_reads;
    static bool playing;
    static int64_t prefill_since,next_capture;
    unsigned generation=atomic_load(&epoch);
    if(current_epoch!=generation){current_epoch=generation;playing=false;prefill_since=0;skip_reads=4;next_capture=esp_timer_get_time();}
    if(atomic_load(&recording)) {
        if(bsp_audio_set_format(16000,16,1)!=ESP_OK){fail(WN_ERR_AUDIO,"麦克风暂不可用");return false;}
        if(atomic_load(&synthetic)){next_capture+=20000;int64_t left=next_capture-esp_timer_get_time();if(left>0)vTaskDelay(pdMS_TO_TICKS((unsigned)(left+999)/1000));memset(p.pcm,0,PCM_BYTES);skip_reads=0;}
        else if(bsp_audio_read(p.pcm,PCM_BYTES)!=ESP_OK){fail(WN_ERR_AUDIO,"麦克风暂不可用");return false;}
        if(skip_reads){--skip_reads;return true;}
        if(!atomic_load(&recording) || generation!=atomic_load(&epoch))return true;
        p.len=PCM_BYTES;p.epoch=generation;p.done=false;p.input=true;received_samples+=PCM_BYTES/2;
        portENTER_CRITICAL(&lock);state.seconds=received_samples/16000;portEXIT_CRITICAL(&lock);
        if(!queue_push(&p,0)){portENTER_CRITICAL(&lock);++state.dropped;portEXIT_CRITICAL(&lock);fail(WN_ERR_CAPTURE_QUEUE,"录音发送失败，请重试");}
        if(received_samples>=20*16000)wn_command(WN_STOP,0);
        return true;
    }
    if(bsp_audio_set_format(OUTPUT_RATE,16,1)!=ESP_OK)return false;
    if(queue_peek(&p) && p.input){
        if(p.epoch!=atomic_load(&epoch) || !atomic_load(&allowed)){queue_pop(&p,0);return true;}
        memset(p.pcm,0,sizeof(p.pcm));return bsp_audio_write(p.pcm,sizeof(p.pcm))==ESP_OK;
    }
    if(!playing && queued()>0) {
        if(!prefill_since)prefill_since=esp_timer_get_time();
        if(queued()<queue_capacity() && esp_timer_get_time()-prefill_since<800000){vTaskDelay(pdMS_TO_TICKS(5));return true;}
    }
    if(queue_pop(&p,0)) {
        prefill_since=0;if(p.epoch!=atomic_load(&epoch) || !atomic_load(&allowed))return true;
        if(p.done){portENTER_CRITICAL(&lock);state.reply_finished=true;portEXIT_CRITICAL(&lock);playing=false;memset(p.pcm,0,sizeof(p.pcm));bsp_audio_write(p.pcm,sizeof(p.pcm));phase(WN_READY,"想继续聊，就按一下确定");return true;}
        playing=true;phase(WN_SPEAKING,"好心情正在送达");
        for(size_t i=0;i<p.len/2;++i)p.pcm[i]=(int16_t)((int)p.pcm[i]*(int)volume/4);
        bool ok=bsp_audio_write(p.pcm,p.len)==ESP_OK;
        if(ok && p.epoch==atomic_load(&epoch)) {
            portENTER_CRITICAL(&lock);state.played+=p.len/2;state.reply_played+=p.len/2;portEXIT_CRITICAL(&lock);
        }
        return ok;
    }
    if(playing){portENTER_CRITICAL(&lock);++state.starves;portEXIT_CRITICAL(&lock);}
    memset(p.pcm,0,sizeof(p.pcm));return bsp_audio_write(p.pcm,sizeof(p.pcm))==ESP_OK;
}
void wn_start(void) {
    commands=xQueueCreate(16,sizeof(command));
    if(!commands){fail(WN_ERR_MEMORY,"网络内存不足");return;}
    if(xTaskCreate(network_task,"walkman_net",6144,NULL,4,NULL)!=pdPASS)fail(WN_ERR_SYSTEM,"网络任务启动失败");
}
