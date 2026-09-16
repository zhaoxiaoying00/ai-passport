#include "walkman_audio.h"
#include "walkman_online.h"
#include "online_state.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
LV_FONT_DECLARE(walkman_16);
LV_FONT_DECLARE(walkman_12);
LV_FONT_DECLARE(walkman_14);
static const char *TAG="walkman";
static QueueHandle_t inputs;
static lv_obj_t *screen;
static lv_display_t *display;
static int page=3, view_page, selected, battery=-1;
static online_caption_cursor voice_caption;
static unsigned physical_seen;
static wa_state view;
static nvs_handle_t storage;
static bool storage_ready, save_failed;
static uint8_t last_saved[WM_SAVE_SIZE];
static int64_t save_due;
static const unsigned timers[]={0,15,30,60};
static const char *repeats[]={"列表循环","单曲循环","顺序播放"};
typedef struct { char command; bool physical; } input_t;
#define BG 0xf8f5f0
#define INK 0x3f4059
#define MUTED 0x89889c
#define ROSE 0xf3c6d0
#define CREAM 0xfffaf1
#define GREEN 0x889ec2
#define CAPTURE_LINES 8
#define CAPTURE_STRIPES ((BSP_LCD_H + CAPTURE_LINES - 1) / CAPTURE_LINES)
static uint16_t *capture_pixels;
static int capture_start_y;
static unsigned capture_rows;
static void rectangle(lv_layer_t *layer,int x,int y,int w,int h,uint32_t color,uint32_t border,int radius) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d); d.bg_color=lv_color_hex(color); d.radius=radius;
    d.border_color=lv_color_hex(border); d.border_width=border?2:0;
    lv_area_t a={x,y,x+w-1,y+h-1}; lv_draw_rect(layer,&d,&a);
}
static void line(lv_layer_t *l,int x,int y,int ex,int ey,uint32_t color,int width) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d); d.p1.x=x; d.p1.y=y; d.p2.x=ex; d.p2.y=ey;
    d.color=lv_color_hex(color); d.width=width; d.round_start=1; d.round_end=1; lv_draw_line(l,&d);
}
static void art_rectangle(lv_layer_t *l,bool small,int x,int y,int w,int h,uint32_t color,uint32_t border,int radius) {
    if(small) { x=120+(x-120)*60/100; y=70+(y-67)*60/100; w=w*60/100; h=h*60/100; radius=radius*60/100; }
    rectangle(l,x,y,w,h,color,border,radius);
}
static void art_line(lv_layer_t *l,bool small,int x,int y,int ex,int ey,uint32_t color,int width) {
    if(small) { x=120+(x-120)*60/100; ex=120+(ex-120)*60/100; y=70+(y-67)*60/100; ey=70+(ey-67)*60/100; }
    line(l,x,y,ex,ey,color,width);
}
static void draw(lv_event_t *e) {
    lv_layer_t *l=lv_event_get_layer(e);
    rectangle(l,184,18,34,18,0xe7eddf,0,8);
    if(view_page && view_page!=4) return;
    const bool small=true;
    art_rectangle(l,small,31,67,178,105,0xe9e6f6,0,22);
    art_rectangle(l,small,40,74,160,90,0xf0edfa,0,18);
    // A smiling cloud wearing headphones; native drawing keeps RAM bounded.
    art_rectangle(l,small,72,97,96,51,CREAM,0,24);
    art_rectangle(l,small,89,82,62,62,CREAM,0,30);
    art_rectangle(l,small,79,87,83,62,0xe1adc0,0,29);
    art_rectangle(l,small,84,91,73,61,CREAM,0,28);
    art_rectangle(l,small,72,106,15,32,ROSE,0,7);
    art_rectangle(l,small,154,106,15,32,ROSE,0,7);
    art_rectangle(l,small,99,116,5,7,INK,0,3); art_rectangle(l,small,136,116,5,7,INK,0,3);
    art_rectangle(l,small,92,125,13,6,0xf6d6d5,0,3); art_rectangle(l,small,135,125,13,6,0xf6d6d5,0,3);
    art_line(l,small,114,127,120,131,INK,2);art_line(l,small,120,131,126,127,INK,2);
    for(int i=0;i<3;++i) {
        int x=i==0?54:i==1?184:173, y=i==0?88:i==1?94:151;
        art_line(l,small,x-3,y,x+3,y,GREEN,2);art_line(l,small,x,y-3,x,y+3,GREEN,2);
    }
    if(view.player.playing) {
        for(unsigned i=0;i<5;++i) {
            int h=4+(int)((view.player.position/(WM_RATE/8)+i*3)%4)*2;
            art_rectangle(l,small,101+(int)i*8,160-h,4,h,GREEN,0,2);
        }
    }
    if(view_page==4){rectangle(l,17,167,206,68,CREAM,0,12);return;}
    if(small) rectangle(l,17,167,206,63,CREAM,0,12);
    rectangle(l,23,237,194,4,0xe6e1df,0,2);
    unsigned total=wm_tracks[view.player.track].samples;
    int width=total?(int)((uint64_t)view.player.position*194/total):0;
    if(width) rectangle(l,23,237,width,4,GREEN,0,2);
    rectangle(l,26,270,56,30,0xeeeaf1,0,15);
    rectangle(l,158,270,56,30,0xeeeaf1,0,15);
    rectangle(l,100,264,40,40,INK,0,20);
    if(view.player.playing) {
        rectangle(l,113,276,4,15,CREAM,0,2);rectangle(l,123,276,4,15,CREAM,0,2);
    } else {
        line(l,116,275,127,284,CREAM,3);line(l,127,284,116,293,CREAM,3);line(l,116,293,116,275,CREAM,3);
    }
}
static lv_obj_t *label(const char *text,int x,int y,int width,bool small,uint32_t color) {
    lv_obj_t *o=lv_label_create(screen);
    lv_obj_set_style_text_font(o,small?&walkman_12:&walkman_16,0);
    lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_pos(o,x,y); lv_obj_set_width(o,width); lv_label_set_text(o,text); return o;
}
static void refresh(void) {
    wa_state s=wa_status();
    wn_state net=wn_status();
    if(!bsp_lvgl_lock(1000)) return;
    view=s; view_page=page; lv_obj_clean(screen);
    char text[160];
    label("元气随身听",24,17,192,false,INK);
    if(battery>=0) snprintf(text,sizeof(text),"%d%%",battery); else snprintf(text,sizeof(text),"--%%");
    label(text,183,20,36,true,0x74866d);
    if(page==0) {
        snprintf(text,sizeof(text),"%02u / %02u    %s",s.player.track+1,wm_track_count,s.failed?"音频出现问题":s.player.playing?"好心情正在派送":"给自己一点好心情");
        label(text,18,46,204,true,MUTED);
        const wm_track *track=&wm_tracks[s.player.track];
        bool timed=track->cue_count>0;
        lv_obj_t *title=label(track->title,19,141,202,false,INK);
        lv_label_set_long_mode(title,LV_LABEL_LONG_DOT); lv_obj_set_height(title,22);
        if(timed) {
            int active=wm_active_cue(s.player.track,s.player.position);
            for(int row=-1;row<=1;++row) {
                int cue=active+row; const char *words=cue>=0 && (unsigned)cue<track->cue_count?wm_cues[track->cue_start+(unsigned)cue].text:"";
                lv_obj_t *o=label(words,22,row==-1?167:row==0?188:209,196,true,row==0?INK:0xb8b3c1);
                lv_obj_set_style_text_font(o,&walkman_14,0);
                lv_obj_set_height(o,20); lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);
            }
        } else {
            lv_obj_t *caption=label(track->caption,25,190,190,true,MUTED);
            lv_obj_set_style_text_font(caption,&walkman_14,0);
            lv_obj_set_height(caption,20); lv_label_set_long_mode(caption,LV_LABEL_LONG_DOT);
        }
        unsigned sec=s.player.position/WM_RATE,total=wm_tracks[s.player.track].samples/WM_RATE;
        snprintf(text,sizeof(text),"%u:%02u",sec/60,sec%60); label(text,20,247,38,true,MUTED);
        snprintf(text,sizeof(text),"%u:%02u",total/60,total%60); label(text,184,247,36,true,MUTED);
        snprintf(text,sizeof(text),"%s  ·  %u%%",s.player.volume?repeats[s.player.repeat]:"静音",s.player.volume*25);
        label(text,60,247,120,true,MUTED);
        label("上一首",26,277,56,true,INK); label("下一首",158,277,56,true,INK);
        if(save_failed) snprintf(text,sizeof(text),"设置未保存，请稍后重试");
        else if(s.player.timer_minutes) snprintf(text,sizeof(text),"%u 分钟后暂停 · 长按确定返回",(unsigned)((s.player.timer_left/WM_RATE+59)/60));
        else snprintf(text,sizeof(text),"长按确定 · 心情主页");
        label(text,19,305,202,true,MUTED);
    } else if(page==3) {
        label("一份随身的好心情",20,51,200,false,MUTED);
        const char *items[]={"说说心情","惊喜夸夸","陪我放松","本地随身听","声音与定时","网络设置"};
        for(int i=0;i<6;++i) {
            lv_obj_t *o=label(items[i],24,82+i*32,192,false,INK);
            lv_obj_set_style_bg_color(o,lv_color_hex(i==selected?ROSE:CREAM),0);
            lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_radius(o,12,0);lv_obj_set_style_pad_ver(o,5,0);
        }
        label(net.wifi?"Wi-Fi 已连接 · 随时开聊":"本地音乐随时可听",20,282,200,true,MUTED);
        label("上下选择 · 确定开始",20,300,200,true,MUTED);
    } else if(page==4) {
        static const char *names[]={"还差一步连接","正在连接","准备好了","你说，我在听","认真想一想","给你一点好心情","再试一次吧","连接设备热点"};
        label(net.recording?"麦克风开启 · 最长二十秒":"联网陪伴 · 麦克风关闭",18,46,204,true,MUTED);
        label(names[net.phase<=WN_SETUP?net.phase:WN_ERROR],19,141,202,false,INK);
        char lines[3][ONLINE_LINE_BYTES];
        unsigned count=online_caption_page(net.text,voice_caption.active,lines);
        unsigned old=voice_caption.active;
        online_caption_update(&voice_caption,net.reply_id,net.reply_played,
                              net.phase==WN_SPEAKING,(uint32_t)(esp_timer_get_time()/1000),
                              net.page_samples,count);
        if(old!=voice_caption.active) {
            online_caption_page(net.text,voice_caption.active,lines);
            printf("WM_PAGE {\"reply\":%u,\"page\":%u,\"sample\":%u,\"cue\":%lu,\"manual\":%s}\n",
                   net.reply_id,voice_caption.active,net.reply_played,
                   (unsigned long)net.page_samples[voice_caption.active],voice_caption.manual?"true":"false");
        }
        if(count) {
            unsigned rows=0;while(rows<3 && lines[rows][0])++rows;
            int top=(int)online_caption_top(rows);
            for(unsigned row=0;row<rows;++row) {
                lv_obj_t *o=label(lines[row],22,top+(int)row*20,196,true,INK);
                lv_obj_set_style_text_font(o,&walkman_14,0);lv_obj_set_height(o,20);
                lv_label_set_long_mode(o,LV_LABEL_LONG_CLIP);
            }
        } else {lv_obj_t *o=label(net.message,25,180,190,true,MUTED);lv_obj_set_style_text_font(o,&walkman_14,0);lv_obj_set_height(o,48);}
        if(net.recording)snprintf(text,sizeof(text),"%u / 20 秒",net.seconds);
        else if(count)snprintf(text,sizeof(text),"上下翻页 · 音量 %u%%",s.player.volume*25);
        else snprintf(text,sizeof(text),"上下翻阅回应 · 音量 %u%%",s.player.volume*25);
        label(text,20,245,200,true,MUTED);
        label(net.recording?"确定 · 说完发送":net.phase==WN_THINKING||net.phase==WN_SPEAKING?"确定 · 停止回应":"确定 · 开始说话",20,274,200,false,INK);
        label("长按确定 · 心情主页",20,305,200,true,MUTED);
    } else if(page==5) {
        if(net.phase==WN_SETUP) {
            label("用手机连接设备热点",20,65,200,false,INK);
            label(net.message,20,104,200,false,MUTED);
            label("连接后在浏览器打开\n192.168.4.1\n选择 Wi-Fi 并填写密码",20,174,200,false,INK);
        } else {
            label("连接 Wi-Fi，开启语音陪伴",20,65,200,false,INK);
            label("设置会打开设备热点\n用手机选择 Wi-Fi 并填密码\n原有音乐和音量会保留",20,111,200,true,MUTED);
            label(selected==0?"● 返回":"返回",30,203,180,false,selected==0?INK:MUTED);
            label(selected==1?"● 开始配置":"开始配置",30,243,180,false,selected==1?INK:MUTED);
        }
        label("长按确定 · 心情主页",20,305,200,true,MUTED);
    } else if(page==1) {
        label("把喜欢的节奏调好",20,51,200,false,MUTED);
        for(int i=0;i<5;++i) {
            if(i==0) snprintf(text,sizeof(text),"音量   %u%%",s.player.volume*25);
            if(i==1) snprintf(text,sizeof(text),"模式   %s",repeats[s.player.repeat]);
            if(i==2) { if(s.player.timer_minutes) snprintf(text,sizeof(text),"定时   %u 分钟",s.player.timer_minutes); else snprintf(text,sizeof(text),"定时   关闭"); }
            if(i==3) snprintf(text,sizeof(text),"按键说明");
            if(i==4) snprintf(text,sizeof(text),"返回随身听");
            lv_obj_t *o=label(text,24,86+i*36,192,false,INK);
            lv_obj_set_style_bg_color(o,lv_color_hex(i==selected?ROSE:CREAM),0);
            lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0); lv_obj_set_style_radius(o,12,0);
            lv_obj_set_style_pad_ver(o,6,0);
        }
        label("上下选择 · 确定修改",20,278,200,true,MUTED);
        label("长按确定返回",20,295,200,true,MUTED);
    } else {
        label("随时给心情充充电",20,57,200,false,INK);
        label("短按上/下：上一首/下一首\n短按确定：播放或暂停\n长按上/下：增加/降低音量\n长按确定：心情主页\n\n定时按播放时间倒计时\n暂停时，倒计时也会暂停\n重启后按确定开始播放",25,96,190,false,MUTED);
        label("确定返回设置",20,285,200,true,INK);
    }
    lv_obj_invalidate(screen); bsp_lvgl_unlock();
}
static void status(void) {
    wa_state s=wa_status();
    printf("WM_STATE {\"page\":%d,\"selected\":%d,\"cue\":%d,\"subtitle_px\":14,\"track\":%u,\"count\":%u,\"volume\":%u,\"repeat\":%u,\"timer\":%u,\"timer_left\":%lu,\"playing\":%s,\"position\":%lu,\"ready\":%s,\"failed\":%s,\"blocks\":%u,\"peak\":%u,\"max_gap_us\":%u,\"max_render_us\":%u,\"stack_free\":%u,\"dropped\":%u,\"save_ok\":%s,\"physical\":%u,\"heap\":%lu,\"min_heap\":%lu}\n",page,selected,wm_active_cue(s.player.track,s.player.position),s.player.track,wm_track_count,s.player.volume,s.player.repeat,s.player.timer_minutes,(unsigned long)s.player.timer_left,s.player.playing?"true":"false",(unsigned long)s.player.position,s.ready?"true":"false",s.failed?"true":"false",s.blocks,s.peak,s.max_gap_us,s.max_render_us,s.stack_free,s.dropped,storage_ready&&!save_failed?"true":"false",physical_seen,(unsigned long)esp_get_free_heap_size(),(unsigned long)esp_get_minimum_free_heap_size());
    wn_state net=wn_status();
    printf("WN_STATE {\"reply_id\":%u,\"reply_played\":%u,\"caption_next_sample\":%lu,\"caption_page\":%u,\"caption_pages\":%u,\"caption_manual\":%s,\"phase\":%u,\"wifi\":%s,\"configured\":%s,\"recording\":%s,\"played\":%u,\"dropped\":%u,\"starves\":%u,\"text_bytes\":%u,\"wifi_reason\":%u,\"testing\":%s,\"captured\":%u,\"uploaded\":%u,\"upload_ms\":%u,\"error_code\":%u,\"failures\":%u,\"connected\":%s,\"prepared\":%s,\"net_stack\":%u,\"ws_stack\":%u}\n",net.reply_id,net.reply_played,(unsigned long)(voice_caption.active+1<ONLINE_CAPTION_PAGES?net.page_samples[voice_caption.active+1]:UINT32_MAX),voice_caption.active,voice_caption.count,voice_caption.manual?"true":"false",net.phase,net.wifi?"true":"false",net.configured?"true":"false",net.recording?"true":"false",net.played,net.dropped,net.starves,(unsigned)strlen(net.text),net.wifi_reason,net.testing?"true":"false",net.captured,net.uploaded,net.upload_ms,net.error_code,net.failures,net.connected?"true":"false",net.prepared?"true":"false",net.net_stack,net.ws_stack);
    fflush(stdout);
}
static void key(char c) {
    wm_player p=wa_status().player;
    /* USB-only long-reply regression probe; never opens the microphone. */
    if(c=='j'){wa_command(WA_NETWORK,1);wn_command(WN_QUICK,2);page=4;voice_caption=(online_caption_cursor){0};return;}
    if(c=='O') { if(page==4){wn_command(WN_CANCEL,0);wa_command(WA_NETWORK,0);}page=page==3?0:3;selected=0;return; }
    if(c=='U' || c=='D') { wa_command(WA_VOLUME,c=='U'?(p.volume<4?p.volume+1:4):(p.volume?p.volume-1:0)); return; }
    if(page==0) { if(c=='u') wa_command(WA_PREV,0); if(c=='d') wa_command(WA_NEXT,0); if(c=='o') wa_command(WA_TOGGLE,0); }
    else if(page==3) {
        if(c=='u')selected=(selected+5)%6;
        if(c=='d')selected=(selected+1)%6;
        if(c=='o') {
            if(selected<3){if(wn_status().phase==WN_SETUP){page=5;return;}wa_command(WA_NETWORK,1);wn_command(selected==0?WN_RECORD:WN_QUICK,selected==2?1:0);page=4;voice_caption=(online_caption_cursor){0};}
            else if(selected==3){wa_command(WA_NETWORK,0);page=0;}
            else if(selected==4){page=1;selected=0;}
            else{page=5;selected=0;}
        }
    } else if(page==4) {
        wn_state net=wn_status();
        if(c=='o'){voice_caption=(online_caption_cursor){0};if(net.recording)wn_command(WN_STOP,0);else if(net.phase==WN_THINKING || net.phase==WN_SPEAKING || net.phase==WN_CONNECTING)wn_command(WN_CANCEL,0);else wn_command(WN_RECORD,0);}
        if(c=='u' || c=='d')online_caption_move(&voice_caption,c=='u'?-1:1,(uint32_t)(esp_timer_get_time()/1000));
    } else if(page==5) {
        if(c=='u' || c=='d')selected^=1;
        if(c=='o'){if(selected==1)wn_command(WN_CONFIGURE,0);else{page=3;selected=0;}}
    }
    else if(page==1) {
        if(c=='u') selected=(selected+4)%5;
        if(c=='d') selected=(selected+1)%5;
        if(c=='o') {
            if(selected==0) wa_command(WA_VOLUME,(p.volume+1)%5);
            if(selected==1) wa_command(WA_REPEAT,(p.repeat+1)%3);
            if(selected==2) { unsigned i=0; while(i<3 && timers[i]!=p.timer_minutes) ++i; wa_command(WA_TIMER,timers[(i+1)%4]); }
            if(selected==3) page=2;
            if(selected==4) page=0;
        }
    } else if(c=='o') page=1;
}
static void save(void) {
    wm_player p=wa_status().player; uint8_t data[WM_SAVE_SIZE]; wm_save(&p,data);
    if(!memcmp(data,last_saved,sizeof(data))) return;
    esp_err_t err=storage_ready?nvs_set_blob(storage,"settings",data,sizeof(data)):ESP_FAIL;
    if(err==ESP_OK) err=nvs_commit(storage);
    save_failed=err!=ESP_OK;
    if(!save_failed) memcpy(last_saved,data,sizeof(data));
    ESP_LOGI(TAG,"SAVE %s",esp_err_to_name(err));
}
static void on_button(bsp_btn_t b,bsp_btn_ev_t ev,void *arg) {
    (void)arg;
    if(ev!=BSP_BTN_CLICK && ev!=BSP_BTN_LONG) return;
    const char *codes=ev==BSP_BTN_LONG?"UDO":"udo";
    input_t in={codes[b],true}; (void)xQueueSend(inputs,&in,0);
}
static void serial_task(void *arg) {
    (void)arg;
    bool discard_line=false;
    for(;;) { int c=getchar(); if(c==EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if(discard_line){if(c=='\n')discard_line=false;continue;}
        if(c=='N') {
            /* NVS writes need stack for flash/cache handling. Keep the
             * bounded JSON input off the serial task's small stack. */
            char *raw=malloc(1024);size_t used=0;bool overflow=raw==NULL;
            int64_t deadline=esp_timer_get_time()+5000000;
            for(;;){c=getchar();if(c=='\n')break;if(c==EOF){clearerr(stdin);if(esp_timer_get_time()>deadline){overflow=true;discard_line=true;break;}vTaskDelay(pdMS_TO_TICKS(5));continue;}if(raw && used<1023)raw[used++]=(char)c;else overflow=true;}
            if(raw)raw[used]=0;
            bool ok=!overflow && wn_configure_json(raw);
            if(raw){memset(raw,0,1024);free(raw);}
            printf("WM_CONFIG_STACK %u\n",(unsigned)uxTaskGetStackHighWaterMark(NULL));
            printf("WM_CONFIGURED %s\n",ok?"true":"false");fflush(stdout);
            if(ok){vTaskDelay(pdMS_TO_TICKS(200));esp_restart();}continue;
        }
        if(c=='x'){wn_command(WN_DROP_PROBE,0);continue;}
        if(c=='z'){wa_command(WA_NETWORK,1);wn_command(WN_PROBE,0);continue;}
        if(strchr("udoUDO?csj",c)) { input_t in={(char)c,false}; xQueueSend(inputs,&in,portMAX_DELAY); }
    }
}

static void capture_event(lv_event_t *e) {
    if (!capture_pixels) return;
    const lv_area_t *area = lv_event_get_param(e);
    lv_draw_buf_t *buf = lv_display_get_buf_active(display);
    if (!area || !buf || !buf->data) return;
    for (int y = area->y1; y <= area->y2; ++y) {
        if (y < capture_start_y || y >= capture_start_y + CAPTURE_LINES || y >= BSP_LCD_H) continue;
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x < 0 || x >= BSP_LCD_W) continue;
            uint16_t pixel;
            memcpy(&pixel, buf->data + (y - area->y1) * buf->header.stride + (x - area->x1) * 2, 2);
            capture_pixels[(y - capture_start_y) * BSP_LCD_W + x] = pixel;
        }
        if (area->x1 == 0 && area->x2 == BSP_LCD_W - 1) ++capture_rows;
    }
}

static void capture(void) {
    // Capture one stripe at a time. Full-frame buffers compete with MP3 scratch RAM.
    uint16_t *pixels=calloc(BSP_LCD_W*CAPTURE_LINES,sizeof(uint16_t));
    if(!pixels) { puts("WM_CAPTURE_ERROR memory"); return; }
    static const char hex[]="0123456789abcdef";
    char row[BSP_LCD_W*4+1];
    printf("WM_FRAME_BEGIN %d %d\n",BSP_LCD_W,BSP_LCD_H);
    for(int top=0;top<BSP_LCD_H;top+=CAPTURE_LINES) {
        if(!bsp_lvgl_lock(2000)) { free(pixels);puts("WM_CAPTURE_ERROR lock");return; }
        capture_start_y=top;capture_rows=0;capture_pixels=pixels;
        lv_obj_invalidate(screen);lv_refr_now(display);
        capture_pixels=NULL;bsp_lvgl_unlock();
        for(int y=top;y<top+CAPTURE_LINES && y<BSP_LCD_H;++y) {
            for(int x=0;x<BSP_LCD_W;++x) {
                uint16_t p=pixels[(y-top)*BSP_LCD_W+x];
                for(unsigned n=0;n<4;++n) row[x*4+n]=hex[(p>>(12-n*4))&15];
            }
            row[BSP_LCD_W*4]=0;printf("WM_ROW %03d %s\n",y,row);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    puts("WM_FRAME_END");fflush(stdout);free(pixels);
}

void app_main(void) {
    usb_serial_jtag_driver_config_t usb={.tx_buffer_size=2048,.rx_buffer_size=256};
    if(usb_serial_jtag_driver_install(&usb)==ESP_OK) usb_serial_jtag_vfs_use_driver();
    ESP_LOGI(TAG,"WALKMAN 1.0 BOOT");
    wm_player initial; wm_init(&initial);
    esp_err_t err=nvs_flash_init();
    if(err==ESP_OK) err=nvs_open("walkman",NVS_READWRITE,&storage);
    storage_ready=err==ESP_OK;
    if(storage_ready) { uint8_t data[WM_SAVE_SIZE]; size_t n=sizeof(data);
        if(nvs_get_blob(storage,"settings",data,&n)==ESP_OK) (void)wm_load(&initial,data,n);
    }
    wm_save(&initial,last_saved);
    ESP_ERROR_CHECK(bsp_i2c_init()); ESP_ERROR_CHECK(bsp_display_init());
    display=bsp_lvgl_init(); if(!display) return;
    if(bsp_battery_init()==ESP_OK) battery=bsp_battery_soc();
    inputs=xQueueCreate(24,sizeof(input_t)); if(!inputs) abort();
    if(!bsp_lvgl_lock(1000)) abort();
    screen=lv_obj_create(NULL); lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen,BSP_LCD_W,BSP_LCD_H);
    lv_obj_set_style_bg_color(screen,lv_color_hex(BG),0);
    lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
    lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen,draw,LV_EVENT_DRAW_MAIN,NULL);
    lv_display_add_event_cb(display,capture_event,LV_EVENT_FLUSH_START,NULL);
    lv_screen_load(screen); bsp_lvgl_unlock();
    wn_start();
    if(!wa_start(&initial)) ESP_LOGE(TAG,"Audio worker unavailable");
    refresh(); bsp_display_backlight(65);
    ESP_ERROR_CHECK(bsp_button_init(on_button,NULL));
    if(xTaskCreate(serial_task,"wm_serial",3072,NULL,3,NULL)!=pdPASS) abort();
    ESP_LOGI(TAG,"READY tracks=%u storage=%d battery=%d",wm_track_count,storage_ready,battery);
    int64_t redraw=0,battery_due=esp_timer_get_time()+30000000;
    for(;;) {
        input_t in;
        if(xQueueReceive(inputs,&in,pdMS_TO_TICKS(30))==pdTRUE) {
            if(in.command=='?') status();
            else if(in.command=='c') capture();
            else if(in.command=='s') save();
            else {
                if(in.physical) { physical_seen|=1u<<(in.command%16); ESP_LOGI(TAG,"PHYSICAL %c",in.command); }
                key(in.command); save_due=esp_timer_get_time()+1500000; redraw=0;
            }
        }
        int64_t now=esp_timer_get_time();
        if(save_due && now>=save_due) { save(); save_due=save_failed?now+5000000:0; }
        if(now>=battery_due) { battery=bsp_battery_soc(); battery_due=now+30000000; }
        if(now>=redraw) { refresh(); redraw=now+250000; }
    }
}
