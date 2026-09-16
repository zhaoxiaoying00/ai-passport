#include "online_tts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned words;
static bool word(const char *prefix,const char *text,void *context) {
    (void)context;assert(strstr(prefix,"\"index\":0"));
    assert(!strcmp(text,"{\"text\":\"\\\"你\\\"好\",\"begin_time\":120}"));++words;return true;
}
int main(void) {
    const char *prefix="{\"header\":{\"event\":\"result-generated\"},\"payload\":{\"output\":{\"sentence\":{\"index\":0,\"words\":[";
    const char *item="{\"text\":\"\\\"你\\\"好\",\"begin_time\":120}";
    const char *suffix="]},\"type\":\"sentence-synthesis\"}}}";
    for(unsigned chunk=1;chunk<50;++chunk) {
        online_tts_stream s;char json[512];words=0;online_tts_begin(&s,json,sizeof(json),word,NULL);
        assert(online_tts_feed(&s,prefix,strlen(prefix)));
        /* Cumulative timestamp messages exceed the 4 KiB device buffer. */
        for(unsigned i=0;i<700;++i){if(i)assert(online_tts_feed(&s,",",1));for(size_t at=0;at<strlen(item);){size_t n=strlen(item)-at;if(n>chunk)n=chunk;assert(online_tts_feed(&s,item+at,n));at+=n;}}
        assert(online_tts_feed(&s,suffix,strlen(suffix)));assert(online_tts_end(&s));assert(words==700);
        char expected[512];snprintf(expected,sizeof(expected),"%s%s",prefix,suffix);assert(!strcmp(json,expected));
    }
    online_tts_stream s;char json[512];online_tts_begin(&s,json,sizeof(json),word,NULL);
    assert(online_tts_feed(&s,prefix,strlen(prefix)));assert(!online_tts_end(&s));
    puts("Bounded streaming word timestamps PASS");
}
