#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "online_state.h"
enum { WN_UNCONFIGURED, WN_CONNECTING, WN_READY, WN_LISTENING, WN_THINKING, WN_SPEAKING, WN_ERROR, WN_SETUP };
enum { WN_RECORD, WN_STOP, WN_QUICK, WN_CANCEL, WN_CONFIGURE, WN_PROBE, WN_DROP_PROBE };
enum { WN_ERR_NONE, WN_ERR_SYSTEM, WN_ERR_CONTROL, WN_ERR_MEMORY, WN_ERR_TRANSPORT, WN_ERR_SEND, WN_ERR_PROTOCOL, WN_ERR_PLAYBACK_QUEUE, WN_ERR_CAPTURE_QUEUE, WN_ERR_SHORT_RECORD, WN_ERR_TIMEOUT, WN_ERR_PROVIDER, WN_ERR_AUDIO };
typedef struct { unsigned reply_id,reply_played; uint32_t page_samples[ONLINE_CAPTION_PAGES]; unsigned net_stack,ws_stack,error_code,failures,phase,seconds,dropped,played,starves,wifi_reason,captured,uploaded,upload_ms; bool reply_finished; bool configured,wifi,recording,testing,connected,prepared; char message[128],text[1801]; } wn_state;
void wn_start(void);
void wn_command(unsigned kind,unsigned value);
wn_state wn_status(void);
/* Called only by the player audio worker while network mode owns the codec. */
bool wn_audio_step(unsigned volume);
bool wn_configure_json(const char *json);
