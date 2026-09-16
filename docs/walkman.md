<p align="right"><a href="walkman.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Pocket Sunshine

A voice companion with an offline audio player for AI Passport, with nine lively Mandarin pep talks and three original instrumental tracks. Speech uses Microsoft Xiaoyi neural synthesis with extended pauses shortened. Public copy and speech use generic greetings; personal recordings are kept outside the repository. Firmware uses a dedicated `walkman` NVS namespace, preserving the Starbridge namespace during segmented flashing.

## Controls

| Input | Player | Settings |
| --- | --- | --- |
| UP / DOWN | Previous / next track | Select an item |
| OK | Play / pause | Change the selected setting |
| Hold UP / DOWN | Raise / lower volume | Raise / lower volume |
| Hold OK | Open settings | Return to player |

Volume has five levels from mute to 100%. Playback supports playlist repeat, track repeat, and play-through. The 15/30/60 minute timer measures actual playback time and pauses when playback pauses; reaching its deadline pauses audio rather than powering down the device. Track selection, volume and repeat mode are saved after 1.5 seconds of input inactivity. Boot starts paused, with the selected track at its beginning; timer and play position are not restored.

## Import local music

Create a local JSON array of objects with `file`, `title`, and optional `caption`. Paths are relative to the JSON file. MP3, M4A, WAV and other FFmpeg-readable files are supported. Use `--append` to keep the existing pep talks and add songs; without it, import replaces the playlist. Source audio remains outside generated firmware metadata; do not add private file paths to source control.

```json
[
  {"file": "music.wav", "title": "My favorite song", "caption": "A little sunshine"}
]
```

```bash
python3 tools/generate_walkman_assets.py --append --playlist /absolute/path/playlist.json
python3 tools/generate_walkman_fonts.py --font /absolute/path/LXGWWenKaiScreen.ttf
./tools/validate.sh
```

Use the pinned LXGW WenKai Screen v1.522 font described in [assets](../assets/README.md). Regenerate font subsets whenever titles or captions change. Import normalizes loudness, adds short edge fades and encodes 96 kbps, 22,050 Hz mono MP3. Maximum: 24 tracks, each shorter than eight minutes, combined encoded pack at most 6 MiB. Firmware layout verification is the final capacity check. Unsupported/oversized files fail without truncating the playlist. Playback streams 256-sample chunks from flash and never loads whole songs into RAM.

Run `python3 tools/generate_walkman_assets.py` to repack the source playlist with FFmpeg. To regenerate speech, install `edge-tts` and run `tools/generate_walkman_voices.py`; it sends the stored pep-talk text to Microsoft for synthesis. Local playback is offline; voice conversation requires Wi-Fi and the Qwen service. Regenerate original instrumentals with `tools/compose_walkman_music.py`. See [asset index](../assets/README.md) for provenance. The online companion is an update to the original community project. Its release branch is `feature/energy-walkman`, with development also available in `feature/energy-walkman-online`.

## Validation

`tests/test_walkman.c` covers decoder saturation, navigation, repeat/end behavior, pause, mute, timer expiry, chunk invariance, persistence corruption and repeated operation. `tools/check_walkman_assets.py` checks pack bounds, decodes every complete track, and compares the MP3 decoder with FFmpeg when available. The complete gate builds and verifies the merged image.

USB diagnostic commands: `?` state, `u/d/o` short buttons, `U/D/O` long buttons, `c` frame capture, `s` save. Physical callbacks and diagnostic commands enter the same input queue. Audio errors remain visible in the player instead of reporting successful playback. Host tests and successful compilation do not establish physical sound quality or battery life.

The fixed-point MP3 decoder is vendored from [Helix](https://github.com/pschatzmann/codec-helix), with original notices and RPSL/RCSL license files in `main/vendor/helix`. Exact upstream blob revisions are recorded in `provenance.json`; the local memory adapter uses the internal heap. Playback uses one decoder and a 6 KiB worker stack; display capture uses 20-line stripes to avoid a full-frame allocation competing with audio memory.

Voice captions use the synthesis service's word timing. Extended silence cuts are mapped onto that timing, and MP3 encoder delay is compensated. The active phrase appears between the previous and next phrases; pausing or changing tracks also pauses or resets captions. Music tracks without a caption file display their title and progress.

Caption reflow keeps short endings with their sentence and retains the original phrase start time. The headphone-cloud illustration has a fixed size for speech and music alike. Caption rows have bounded heights and do not resize the artwork.

All subtitle rows use a fixed 14 px font. Color alone identifies the active line; long lines never shrink the text or resize the illustration. Local song captions are transcribed from the audio, checked against its timing, and stored as millisecond cues. Cue files are JSON arrays of `ms` and `text`, referenced by the playlist entry’s `cues` field. Keep each line within 14 Chinese characters.

Convert a local LRC file with `python3 tools/import_walkman_lrc.py song.lrc song.cues.json`, then add `"cues": "song.cues.json"` to its local playlist entry. LRC offsets and repeated timestamps for repeated lines are supported. Lines exceeding 14 characters fail with an explanation; split them into separately timed phrases. Import regenerates the pack; regenerate fonts and run the complete gate before flashing. Community builds contain the 12 bundled tracks only.

## Online companion

See the root README for the new home and conversation controls. Direct TLS/WebSocket calls use the Qwen-Audio Realtime protocol. Audio recording is 16 kHz mono PCM; replies are 24 kHz mono PCM. The existing audio worker exclusively owns the codec and switches its format when entering or leaving conversation mode. Entering a conversation pauses and rewinds the local track. Bluetooth is disabled and Wi-Fi buffers are limited to leave room for TLS; TLS buffers remain allocated throughout each connection to avoid heap fragmentation. Capture and playback share a bounded FIFO in half-duplex operation. Capture uses 32 packets. Playback adds 16 packets after releasing the upload buffer, providing roughly 640 ms of initial buffering without reducing capture memory. Each extra packet is allocated separately so fragmented free memory can be used; a single contiguous 10 KiB allocation is not required. The extra storage is released before the next recording or connection close. Voice failures remain visible; the user can return to local playback.

A 14 px WenKai subset covers the BMP CJK range for generated replies. Online replies display up to three equally emphasized 14 px lines per page, starting at the first page. Incoming text does not push the view to the last page. Dialogue uses the realtime model in text mode, then synthesizes that reply with `qwen-audio-3.0-tts-plus`, keeping the `longanlingxin` voice and requesting word timestamps. Pages advance when consumed PCM reaches the timestamp of the first new row, with a 60 ms allowance for the codec DMA queue. Unknown timestamps never advance a page by guessed character speed. The two TLS connections are opened sequentially, and TTS incurs its own service usage. Intentional socket shutdown wakes the read poll immediately instead of waiting up to one second. After playback, synthesis storage is released and one dialogue connection is prepared for up to 20 seconds; this never starts recording or generates a reply. Preparation failures are optional and the next explicit request can reconnect normally. The next turn can reuse that prepared connection. Text still appears as it arrives; rendering is not artificially delayed to hide network latency. A bounded 2 KiB RAM history preserves recent text context between switches and is cleared on reboot. Line wrapping keeps punctuation with preceding text rather than at the beginning of a row. Opening quotes stay attached to their text; curly quotes, em dashes and ellipses are included in the font. The footer shows only the browsing hint and volume, without an automatic-mode label or page counter. Short trailing rows are balanced with the preceding row. Pages contain three consecutive rows without repeating context; only the final page may have fewer rows, vertically centered within the same subtitle area. Finishing the reply does not force a jump to its last page. Up/Down changes whole pages and pauses automatic paging for six seconds during playback; after playback, the chosen page stays until the next reply. All pages of the bounded reply remain accessible, including mixed-language responses. Local pep talks retain their timed captions. Generic greetings avoid personal names. Synthesis PCM is streamed from binary WebSocket fragments using a 4 KiB metadata buffer and a bounded PCM queue. Cumulative timestamp arrays are consumed one word object at a time, retaining only page boundaries. Whole encoded audio events are never retained in RAM. Wire messages are limited to 128 KiB; compact JSON metadata is limited to 4 KiB. Microphone uploads combine 100 ms of PCM per message. A 4608-byte WebSocket buffer keeps each append in one frame; WebSocket send/receive buffers are reserved before connecting, with a separate transmit lock and TCP_NODELAY. PCM is encoded directly into the upload message instead of retaining another raw upload copy. Wi-Fi buffers are bounded, and the network scan list is allocated only during provisioning.

Run `python3 tools/walkman_caption_probe.py --port /dev/cu.usbmodemXXXXX --output /private/path/caption-check` to play two cloud-generated stories without opening the microphone. It checks actual page-change sample positions against provider cues, checks underruns and failures, and verifies that the final page stays after the synthesis connection closes. The output contains counters and a last-page screenshot. Protocol references: [realtime client events](https://help.aliyun.com/en/model-studio/fun-audiochat-client-events), [synthesis client events](https://help.aliyun.com/zh/model-studio/cosyvoice-client-events), and [synthesis server events](https://help.aliyun.com/zh/model-studio/cosyvoice-server-events).

Run `python3 tools/walkman_network_probe.py --port /dev/cu.usbmodemXXXXX --output /private/path/network-probe.json` to check a 20-second upload on the connected device. The USB `z` diagnostic generates silence without reading the microphone. It exercises the same 16 kHz codec setup as a real recording, then closes the connection without committing audio or requesting a response. Status exposes captured/uploaded sample counts and maximum append duration. A passing probe does not verify speech recognition or audible playback. `tools/walkman_recovery_probe.py` uses the same arguments to inject a socket shutdown during synthetic upload and verify resource cleanup and recovery from one new request. The USB `x` command is ignored outside synthetic diagnostics. USB `j` requests a longer story for multi-page caption synchronization checks without opening the microphone. Intentional cancellation and idle closure do not report a connection failure; unexpected failures retain the first error category, release the stale connection and leave the microphone off. `tools/walkman_online_soak.py` performs six audible quick replies and three synthetic uploads, including switching from a reply to upload on the same connection. It must be announced before use; it never opens the microphone. Conversation currently uses manual recording and send controls; automatic turn detection and continuous listening are not implemented.

USB configuration requires Python and pyserial. Supply a private environment file containing `DASHSCOPE_API_KEY` and optionally `QWEN_AUDIO_REALTIME_MODEL`. Use `--setup` to save the default key privately and open the Wi-Fi picker. Connect a phone to the hotspot shown on the device, visit `192.168.4.1`, select a scanned network, and enter its password. Rescan refreshes nearby networks; hidden networks support manual entry. The key field stays blank and reuses the saved default unless a replacement is supplied. Use `--reuse-wifi` to reuse the device’s existing `wm_online` or legacy `bean_online` Wi-Fi settings, or pass a private `--wifi-file` containing `ssid` and `password`. Neither credentials nor raw serial logs are printed. The tool verifies online firmware before sending anything.

```bash
python3 tools/configure_walkman_online.py --port /dev/cu.usbmodemXXXXX --env /private/path/config.env --setup
```

Configuration is a bounded, versioned NVS blob; offline `walkman` and Starbridge settings remain separate. Cloud endpoints are restricted to the official DashScope TLS hostname, with certificate validation. Network setup uses a random WPA2 hotspot password and a per-boot form nonce. API availability, Wi-Fi signal, cloud latency and physical microphone/speaker quality affect live conversation.
