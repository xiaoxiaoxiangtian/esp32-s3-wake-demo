#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_MIC_BUFFER_SAMPLES  1024

typedef enum {
    I2S_MIC_CHANNEL_NONE = 0,
    I2S_MIC_CHANNEL_LEFT,
    I2S_MIC_CHANNEL_RIGHT,
} i2s_mic_channel_t;

typedef struct {
    esp_err_t last_error;
    size_t bytes_read;
    int frames;
    i2s_mic_channel_t channel;
    float rms_left;
    float rms_right;
    float peak_left;
    float peak_right;
    float output_rms;
    float output_peak;
    float dc_offset;
} i2s_mic_stats_t;

bool i2s_mic_init(void);
int  i2s_mic_read(int16_t *samples, int max_samples);
void i2s_mic_get_stats(i2s_mic_stats_t *stats);
const char *i2s_mic_channel_name(i2s_mic_channel_t channel);
void i2s_mic_deinit(void);

#ifdef __cplusplus
}
#endif
