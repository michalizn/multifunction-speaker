#ifndef __AUDIO_TONEURI_H__
#define __AUDIO_TONEURI_H__

extern const char* tone_uri[];

typedef enum {
    TONE_TYPE_BT_MODE,
    TONE_TYPE_INTRO_MODE,
    TONE_TYPE_SD_MODE,
    TONE_TYPE_WIFI_MODE,
    TONE_TYPE_MAX,
} tone_type_t;

int get_tone_uri_num();

#endif
