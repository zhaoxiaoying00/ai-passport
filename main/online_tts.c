#include "online_tts.h"
#include <string.h>
void online_tts_begin(online_tts_stream *s,char *json,size_t capacity,online_tts_word_sink sink,void *context) {
    memset(s,0,sizeof(*s));s->json=json;s->capacity=capacity;s->sink=sink;s->context=context;
}
static bool byte(online_tts_stream *s,char c) {
    if(s->words) {
        if(!s->word_depth) {
            if(c==']'){s->words=false;--s->depth;}
            else if(c=='{'){s->word_depth=1;s->word_used=0;s->word[s->word_used++]=c;return true;}
            else return c==' ' || c=='\r' || c=='\n' || c=='\t' || c==',';
        } else {
            if(s->word_used+1>=sizeof(s->word))return false;
            s->word[s->word_used++]=c;
            if(s->string){if(s->escape)s->escape=false;else if(c=='\\')s->escape=true;else if(c=='"')s->string=false;}
            else if(c=='"')s->string=true;
            else if(c=='{')++s->word_depth;
            else if(c=='}' && !--s->word_depth) {
                s->word[s->word_used]=0;s->json[s->used]=0;
                if(!s->sink(s->json,s->word,s->context))return false;
            }
            return true;
        }
        /* Emit the closing bracket for the stripped, now empty array. */
        if(s->used+1>=s->capacity)return false;
        s->json[s->used++]=c;return true;
    }
    if(s->used+1>=s->capacity)return false;
    s->json[s->used++]=c;
    if(s->string) {
        if(s->escape)s->escape=false;
        else if(c=='\\')s->escape=true;
        else if(c=='"'){s->string=false;s->words_key=s->used-s->start==7 && !memcmp(s->json+s->start,"\"words\"",7);}
    } else if(c=='"'){s->start=s->used-1;s->string=true;}
    else if(c=='{' || c=='['){if(++s->depth>8)return false;if(c=='[' && s->words_key){s->words=true;s->words_depth=s->depth;}s->words_key=false;}
    else if(c=='}' || c==']'){if(!s->depth)return false;--s->depth;s->words_key=false;}
    else if(c!=':' && c!=' ' && c!='\r' && c!='\n' && c!='\t')s->words_key=false;
    return true;
}
bool online_tts_feed(online_tts_stream *s,const char *data,size_t length) {
    if(s->failed)return false;
    for(size_t i=0;i<length;++i)if(!byte(s,data[i])){s->failed=true;return false;}
    return true;
}
bool online_tts_end(online_tts_stream *s) {
    if(s->failed || s->string || s->depth || s->words || s->word_depth)return false;
    s->json[s->used]=0;return true;
}
