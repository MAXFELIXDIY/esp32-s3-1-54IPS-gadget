#pragma once

typedef enum {
    AUDIO_STOPPED,
    AUDIO_CONNECTING,
    AUDIO_BUFFERING,
    AUDIO_PLAYING,
    AUDIO_ERROR,
} audio_state_t;

void audio_init(void);
void audio_play(const char *url);       /* нескінченний потік (радіо) */
void audio_play_clip(const char *url);  /* скінченний файл: грати після повного завантаження */
void audio_stop(void);

audio_state_t audio_state(void);
const char *audio_info(void);       /* рядок типу "128 кбіт/с - 44 кГц" */

void audio_set_volume(int v);       /* 0..256 */
int  audio_get_volume(void);
