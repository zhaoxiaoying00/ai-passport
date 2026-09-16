#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Replay history one acknowledged item at a time before opening capture. */
typedef struct { unsigned total,sent,confirmed; bool configured; } online_restore;
bool online_restore_send(online_restore *restore);
void online_restore_ack(online_restore *restore,unsigned item);
bool online_restore_ready(const online_restore *restore);
typedef struct { uint32_t version; char ssid[33], password[65], url[256], token[193]; } online_config_t;
bool online_config_valid(const online_config_t *config);
bool online_fragment(size_t *used,size_t capacity,size_t offset,size_t length,size_t total);
void online_setup_password(uint32_t random_value,char out[9]);
#define ONLINE_LINE_BYTES 43
unsigned online_caption_lines(const char *text,char (*lines)[ONLINE_LINE_BYTES],unsigned capacity);
/* Return the full line count while retaining only the requested window. */
unsigned online_caption_slice(const char *text,unsigned first,char (*lines)[ONLINE_LINE_BYTES],unsigned capacity);
/* Return the page count and three rows of the requested page. */
unsigned online_caption_page(const char *text,unsigned page,char lines[3][ONLINE_LINE_BYTES]);
/* Vertical centering within the fixed three-row subtitle area. */
unsigned online_caption_top(unsigned rows);
#define ONLINE_CAPTION_PAGES 65
/* Unknown timing never advances a page. Page starts come from the TTS
 * word timestamps and are compared against consumed 24 kHz PCM. */
typedef struct {
    unsigned reply, active, count;
    uint32_t manual_since;
    bool initialized, manual;
} online_caption_cursor;
void online_caption_update(online_caption_cursor *cursor,unsigned reply,unsigned played,
                           bool speaking,uint32_t now,const uint32_t *starts,unsigned count);
/* Byte offset of the first row on the page; pages never repeat rows. */
unsigned online_caption_boundary(const char *text,unsigned page);
void online_caption_move(online_caption_cursor *cursor,int direction,uint32_t now);
/* In-place extraction for the provider's large base64 audio events; no heap allocation.
 * Returns 1 for audio, 0 for another event, and -1 for malformed input. */
int online_audio_event(char *json,size_t length,char **audio,size_t *audio_length);
