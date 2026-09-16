#include "online_upload.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned value(char c) {
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *found=strchr(alphabet,c);assert(found);return (unsigned)(found-alphabet);
}
int main(void) {
    uint8_t pcm[ONLINE_UPLOAD_PCM_MAX];for(size_t i=0;i<sizeof(pcm);++i)pcm[i]=(uint8_t)(i*73+19);
    for(size_t length=2;length<=sizeof(pcm);length+=2) {
        online_upload s;online_upload_begin(&s);
        for(size_t at=0;at<length;){size_t n=length-at;if(n>17)n=17;assert(online_upload_append(&s,pcm+at,n));at+=n;}
        size_t bytes=online_upload_finish(&s);assert(bytes>0 && bytes==strlen(s.json));assert(online_upload_finish(&s)==bytes);
        assert(bytes+8<=ONLINE_UPLOAD_WIRE_MAX); /* Include the masked WS header. */
        assert(!online_upload_append(&s,pcm,1));
        const char *b=strstr(s.json,"\"audio\":\"");assert(b);b+=9;size_t out=0;
        while(*b!='"') {
            unsigned a=value(b[0]),c=value(b[1]),d=b[2]=='='?0:value(b[2]),e=b[3]=='='?0:value(b[3]);
            assert(out<length && pcm[out++]==(uint8_t)((a<<2)|(c>>4)));
            if(b[2]!='=')assert(out<length && pcm[out++]==(uint8_t)((c<<4)|(d>>2)));
            if(b[3]!='=')assert(out<length && pcm[out++]==(uint8_t)((d<<6)|e));
            b+=4;
        }
        assert(out==length && !strcmp(b,"\"}"));
    }
    online_upload s;online_upload_begin(&s);assert(online_upload_finish(&s)==0);
    assert(online_upload_append(&s,pcm,1));assert(online_upload_finish(&s)==0);
    assert(!online_upload_append(&s,pcm,sizeof(pcm)));assert(s.samples_bytes==1);
    assert(online_upload_append(&s,pcm+1,sizeof(pcm)-1));assert(online_upload_finish(&s)>0);
    puts("Online upload: all even PCM lengths, split quartets, capacity and padding PASS");
}
