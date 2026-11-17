#pragma once
#include <stdbool.h>
#include <stddef.h>

#define AUDIO_BUILTIN_COUNT 4
#define AUDIO_KEY_MAX 80

/* Sound keys persisted to NVS: "builtin:0" .. "builtin:3",
 * or "sd:filename.wav/mp3/rtttl" for removable TF audio. */
void audio_init(void);
void audio_stop(void);
void audio_play_key(const char *key, bool loop);
bool audio_playing(void);
bool audio_file_supported(const char *name);
const char *audio_builtin_name(int id);
void audio_builtin_key(int id, char *out, size_t n);
void audio_sd_key(const char *filename, char *out, size_t n);
void audio_key_label(const char *key, char *out, size_t n);
