#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ONLINE_UPLOAD_PCM_MAX 1280
#define ONLINE_UPLOAD_WIRE_MAX 2048
/* Encodes 40 ms of PCM directly into one TLS record, without a PCM copy. */
typedef struct {
    size_t samples_bytes,used;
    unsigned carry_used;
    uint8_t carry[3];
    bool finished;
    char json[1800];
} online_upload;
void online_upload_begin(online_upload *s);
bool online_upload_append(online_upload *s,const uint8_t *pcm,size_t length);
size_t online_upload_finish(online_upload *s);
