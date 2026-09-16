#include "online_state.h"
bool online_restore_send(online_restore *r) {
    if(!r->configured || r->sent!=r->confirmed || r->sent>=r->total)return false;
    ++r->sent;return true;
}
void online_restore_ack(online_restore *r,unsigned item) {
    if(item==r->sent && item==r->confirmed+1)r->confirmed=item;
}
bool online_restore_ready(const online_restore *r) {
    return r->configured && r->confirmed==r->total;
}
#include <stdio.h>
#include <string.h>
static bool bounded(const char *s,size_t n) {return memchr(s,0,n)!=NULL;}
bool online_config_valid(const online_config_t *c) {
    static const char prefix[]="wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen-";
    if(!c || c->version!=1 || !bounded(c->ssid,sizeof(c->ssid)) || !bounded(c->password,sizeof(c->password)) ||
       !bounded(c->url,sizeof(c->url)) || !bounded(c->token,sizeof(c->token)) || !c->ssid[0])return false;
    size_t pass=strlen(c->password);if(pass && (pass<8 || pass>63))return false;
    if(strncmp(c->url,prefix,sizeof(prefix)-1) || strlen(c->token)<12 || strpbrk(c->token,"\r\n\t "))return false;
    const char *model=c->url+sizeof(prefix)-1;
    return *model && strspn(model,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.")==strlen(model);
}
bool online_fragment(size_t *used,size_t cap,size_t off,size_t len,size_t total) {
    if(!off)*used=0;
    if(total>=cap || off!=*used || off>total || len>total-off){*used=0;return false;}
    *used+=len;return true;
}
void online_setup_password(uint32_t random_value,char out[9]) {
    snprintf(out,9,"%08lu",(unsigned long)(random_value%100000000u));
}
static void caption_store(unsigned row,const char *line,unsigned first,
                          char (*lines)[ONLINE_LINE_BYTES],unsigned cap) {
    if(row>=first && row-first<cap)snprintf(lines[row-first],ONLINE_LINE_BYTES,"%s",line);
}
static bool caption_punctuation(const unsigned char *p) {
    unsigned c=*p;
    if(c<128)return (c>=33 && c<=47) || (c>=58 && c<=64) ||
                    (c>=91 && c<=96) || (c>=123 && c<=126);
    if((c&0xf0)!=0xe0 || !p[1] || !p[2])return false;
    c=((c&15)<<12)|((p[1]&63)<<6)|(p[2]&63);
    return (c>=0x2010 && c<=0x2027) || (c>=0x3001 && c<=0x301f) ||
           (c>=0xff01 && c<=0xff0f) || (c>=0xff1a && c<=0xff20) ||
           (c>=0xff3b && c<=0xff40) || (c>=0xff5b && c<=0xff65);
}
static size_t caption_previous(const char *s,size_t at) {
    if(at){do{--at;}while(at && ((unsigned char)s[at]&0xc0)==0x80);}
    return at;
}
static bool caption_opening(const unsigned char *p) {
    if(*p<128)return *p && strchr("([{<\"'",*p)!=NULL;
    if(p[0]==0xc2 && p[1]==0xab)return true;
    if((p[0]&0xf0)!=0xe0 || !p[1] || !p[2])return false;
    unsigned c=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63);
    return c==0x2018 || c==0x201c || c==0x3008 || c==0x300a ||
           c==0x300c || c==0x300e || c==0x3010 || c==0x3014 ||
           c==0x3016 || c==0x3018 || c==0x301a || c==0xff08 ||
           c==0xff3b || c==0xff5b;
}
static size_t caption_break_before(const char *s,size_t at) {
    while(at) {
        size_t prev=caption_previous(s,at);
        if(!caption_punctuation((const unsigned char *)s+at) && s[at]!=' ' &&
           !caption_opening((const unsigned char *)s+prev))break;
        at=prev;
    }
    return at;
}
unsigned online_caption_slice(const char *text,unsigned first,char (*lines)[ONLINE_LINE_BYTES],unsigned cap) {
    unsigned row=0,chars=0,bytes=0;
    char current[ONLINE_LINE_BYTES]={0},previous[ONLINE_LINE_BYTES]={0};
    if(cap)memset(lines,0,cap*ONLINE_LINE_BYTES);
    for(const unsigned char *p=(const unsigned char *)text;*p;) {
        unsigned n=*p<128?1:(*p&0xe0)==0xc0?2:(*p&0xf0)==0xe0?3:4;
        bool complete=true;
        for(unsigned i=1;i<n;++i)if(!p[i] || (p[i]&0xc0)!=0x80){complete=false;break;}
        if(!complete)break;
        if(n==4){p+=n;continue;}
        if(*p=='\r' || *p=='\t' || (!chars && *p==' ')){++p;continue;}
        if(*p=='\n') {
            const unsigned char *next=p+1;
            while(*next=='\n' || *next=='\r' || *next==' ' || *next=='\t')++next;
            if(caption_punctuation(next)){p=next;continue;}
        }
        if(chars==14 && (caption_punctuation(p) ||
           caption_opening((const unsigned char *)current+caption_previous(current,bytes)))) {
            size_t at=caption_previous(current,bytes);
            at=caption_break_before(current,at);
            /* An overlong punctuation-only run cannot fit on another line. */
            if(!at){p+=n;continue;}
            char carry[ONLINE_LINE_BYTES];snprintf(carry,sizeof(carry),"%s",current+at);
            current[at]=0;caption_store(row,current,first,lines,cap);
            memcpy(previous,current,sizeof(previous));++row;
            snprintf(current,sizeof(current),"%s",carry);bytes=strlen(current);chars=0;
            for(unsigned i=0;i<bytes;++i)if(((unsigned char)current[i]&0xc0)!=0x80)++chars;
        }
        if(*p=='\n' || chars==14) {
            if(chars) {
                caption_store(row,current,first,lines,cap);memcpy(previous,current,sizeof(previous));
                ++row;bytes=chars=0;current[0]=0;
            }
            if(*p=='\n'){++p;continue;}
        }
        /* Keep opening quotes/brackets intact, including at the start of a
         * reply. Dropping them leaves an unmatched closing mark later. */
        if(!chars && caption_punctuation(p) && !caption_opening(p)){p+=n;continue;}
        memcpy(current+bytes,p,n);bytes+=n;current[bytes]=0;++chars;p+=n;
    }
    /* Keep a word plus punctuation from becoming a tiny trailing row. */
    if(row && chars && chars<4) {
        size_t len=strlen(previous),at=len;unsigned moved=0;
        while(at && moved<4-chars) {
            at=caption_previous(previous,at);
            ++moved;
        }
        at=caption_break_before(previous,at);
        unsigned remaining=0;
        for(size_t i=0;i<at;++i)if(((unsigned char)previous[i]&0xc0)!=0x80)++remaining;
        if(remaining>=4) {
            size_t n=len-at;memmove(current+n,current,strlen(current)+1);
            memcpy(current,previous+at,n);previous[at]=0;
            caption_store(row-1,previous,first,lines,cap);
        }
    }
    if(chars)caption_store(row,current,first,lines,cap);
    return row+(chars?1:0);
}
unsigned online_caption_lines(const char *text,char (*lines)[ONLINE_LINE_BYTES],unsigned cap) {
    unsigned count=online_caption_slice(text,0,lines,cap);
    return count<cap?count:cap;
}

unsigned online_caption_top(unsigned rows) {
    if(rows>3)rows=3;
    return 173+(3-rows)*10;
}
static unsigned caption_page_first(unsigned rows,unsigned page) {
    unsigned pages=(rows+2)/3;
    if(pages && page>=pages)page=pages-1;
    return page*3;
}
unsigned online_caption_page(const char *text,unsigned page,char lines[3][ONLINE_LINE_BYTES]) {
    unsigned rows=online_caption_slice(text,0,NULL,0),pages=(rows+2)/3;
    online_caption_slice(text,caption_page_first(rows,page),lines,3);
    return pages;
}

unsigned online_caption_boundary(const char *text,unsigned page) {
    if(!page)return 0;
    char row[1][ONLINE_LINE_BYTES];const char *p=text;
    /* Match each wrapped row in order, preserving repeated phrases and the
     * raw offsets of skipped emoji, whitespace and punctuation. */
    unsigned rows=online_caption_slice(text,0,NULL,0);
    unsigned first=caption_page_first(rows,page);
    for(unsigned i=0;i<first;++i) {
        online_caption_slice(text,i,row,1);
        for(const unsigned char *q=(const unsigned char *)row[0];*q;) {
            unsigned n=*q<128?1:(*q&0xe0)==0xc0?2:3;
            while(*p && strncmp(p,(const char *)q,n))++p;
            if(!*p)return (unsigned)strlen(text);
            p+=n;q+=n;
        }
    }
    return (unsigned)(p-text);
}
void online_caption_update(online_caption_cursor *c,unsigned reply,unsigned played,
                           bool speaking,uint32_t now,const uint32_t *starts,unsigned count) {
    if(!c->initialized || c->reply!=reply)
        *c=(online_caption_cursor){.initialized=true,.reply=reply};
    c->count=count;
    if(c->active>=count)c->active=count?count-1:0;
    if(c->manual) {
        if(now-c->manual_since<6000 || !speaking)return;
        c->manual=false;
    }
    unsigned page=0;
    while(page+1<count && starts[page+1]!=UINT32_MAX && played>=starts[page+1])++page;
    c->active=page;
}
void online_caption_move(online_caption_cursor *c,int direction,uint32_t now) {
    if(!c->count)return;
    if(direction<0 && c->active)--c->active;
    if(direction>0 && c->active+1<c->count)++c->active;
    c->manual=true;c->manual_since=now;
}

static void json_space(const char **p,const char *end) {
    while(*p<end && (**p==' ' || **p=='\t' || **p=='\r' || **p=='\n'))++*p;
}
static bool json_string(const char **p,const char *end) {
    if(*p==end || *(*p)++!='"')return false;
    while(*p<end) {
        unsigned char c=(unsigned char)*(*p)++;
        if(c=='"')return true;
        if(c<32)return false;
        if(c=='\\') {
            if(*p==end)return false;
            char escaped=*(*p)++;
            if(escaped=='u') {
                for(unsigned i=0;i<4;++i){if(*p==end || !strchr("0123456789abcdefABCDEF",**p))return false;++*p;}
            } else if(!strchr("\"\\/bfnrt",escaped))return false;
        }
    }
    return false;
}
static bool json_value(const char **p,const char *end,unsigned depth) {
    if(depth>8 || *p==end)return false;
    if(**p=='"')return json_string(p,end);
    if(**p=='{' || **p=='[') {
        bool object=**p=='{';char close=object?'}':']';++*p;json_space(p,end);
        if(*p<end && **p==close){++*p;return true;}
        for(;;) {
            if(object){if(!json_string(p,end))return false;json_space(p,end);if(*p==end || *(*p)++!=':')return false;json_space(p,end);}
            if(!json_value(p,end,depth+1))return false;
            json_space(p,end);if(*p==end)return false;
            char c=*(*p)++;if(c==close)return true;if(c!=',')return false;json_space(p,end);
        }
    }
    const char *start=*p;
    while(*p<end && !strchr(" \t\r\n,}]",**p))++*p;
    return *p>start;
}
int online_audio_event(char *json,size_t length,char **audio,size_t *audio_length) {
    const char *p=json,*end=json+length,*delta=NULL;size_t delta_length=0;bool is_audio=false,seen_type=false,seen_delta=false;
    *audio=NULL;*audio_length=0;json_space(&p,end);if(p==end || *p++!='{')return -1;json_space(&p,end);
    if(p<end && *p=='}')return 0;
    for(;;) {
        const char *key=p;if(!json_string(&p,end))return -1;size_t key_length=(size_t)(p-key);
        json_space(&p,end);if(p==end || *p++!=':')return -1;json_space(&p,end);
        const char *value=p;if(!json_value(&p,end,0))return -1;size_t value_length=(size_t)(p-value);
        if(key_length==6 && !memcmp(key,"\"type\"",6)) {
            if(seen_type)return -1;
            seen_type=true;is_audio=value_length==22 && !memcmp(value,"\"response.audio.delta\"",22);
        } else if(key_length==7 && !memcmp(key,"\"delta\"",7)) {
            if(seen_delta)return -1;
            seen_delta=true;if(value_length>=2 && *value=='"'){delta=value+1;delta_length=value_length-2;}
        }
        json_space(&p,end);if(p==end)return -1;char c=*p++;
        if(c=='}')break;
        if(c!=',')return -1;
        json_space(&p,end);
    }
    json_space(&p,end);if(p!=end)return -1;
    if(!is_audio)return 0;
    if(!delta)return -1;
    char *out=(char*)delta;size_t n=0;
    for(size_t i=0;i<delta_length;++i) {
        unsigned char c=(unsigned char)delta[i];
        if(c=='\\'){if(++i>=delta_length || delta[i]!='/')return -1;c='/';}
        if(!((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='+' || c=='/' || c=='='))return -1;
        out[n++]=(char)c;
    }
    if(n%4)return -1;
    *audio=out;*audio_length=n;return 1;
}
