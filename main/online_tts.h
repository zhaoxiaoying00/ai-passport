#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Remove the potentially huge cumulative words array while visiting one
 * bounded word object at a time. Other metadata remains valid JSON. */
typedef bool (*online_tts_word_sink)(const char *prefix,const char *word,void *context);
typedef struct {
    char *json; size_t capacity,used,start,word_used;
    unsigned depth,words_depth,word_depth;
    bool string,escape,words_key,words,failed;
    char word[512];
    online_tts_word_sink sink; void *context;
} online_tts_stream;
void online_tts_begin(online_tts_stream *s,char *json,size_t capacity,online_tts_word_sink sink,void *context);
bool online_tts_feed(online_tts_stream *s,const char *data,size_t length);
bool online_tts_end(online_tts_stream *s);
