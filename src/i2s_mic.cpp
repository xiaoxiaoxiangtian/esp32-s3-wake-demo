#include "i2s_mic.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <string.h>

// Pin assignments (cast -D int defines to gpio_num_t)
#define MIC_SCK  ((gpio_num_t)I2S_MIC_SCK)
#define MIC_WS   ((gpio_num_t)I2S_MIC_WS)
#define MIC_SD   ((gpio_num_t)I2S_MIC_SD)

static const char *TAG = "i2s_mic";
static bool mic_ready = false;
static int32_t raw_buf[I2S_MIC_BUFFER_SAMPLES * 2];
static i2s_mic_stats_t last_stats = {
    .last_error = ESP_FAIL,
    .bytes_read = 0,
    .frames = 0,
    .channel = I2S_MIC_CHANNEL_NONE,
};

#ifndef I2S_MIC_GAIN
#define I2S_MIC_GAIN 6.0f
#endif

static float raw_to_unit(int32_t raw)
{
    float value = (float)raw / 2147483648.0f;
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return value;
}

static float soft_limit(float value)
{
    const float sign = value < 0.0f ? -1.0f : 1.0f;
    float mag = value < 0.0f ? -value : value;

    if (mag > 0.85f) {
        float over = mag - 0.85f;
        mag = 0.85f + over / (1.0f + over * 3.0f);
    }
    if (mag > 0.995f) mag = 0.995f;
    return sign * mag;
}

static int16_t raw_to_pcm(int32_t raw, double dc_offset)
{
    double centered = (double)raw - dc_offset;
    if (centered > 2147483647.0) centered = 2147483647.0;
    if (centered < -2147483648.0) centered = -2147483648.0;
    float value = soft_limit(((float)centered / 2147483648.0f) * I2S_MIC_GAIN);
    return (int16_t)(value * 32767.0f);
}

static float abs_unit(int32_t raw)
{
    float value = raw_to_unit(raw);
    return value < 0.0f ? -value : value;
}

bool i2s_mic_init(void)
{
    i2s_driver_uninstall(I2S_NUM_0);

    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_32BIT,
    };

    i2s_pin_config_t pin_cfg = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = MIC_SCK,
        .ws_io_num = MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_SD,
    };

    esp_err_t ret = i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, NULL);
    if (ret == ESP_OK) {
        ret = i2s_set_pin(I2S_NUM_0, &pin_cfg);
    }
    if (ret == ESP_OK) {
        i2s_zero_dma_buffer(I2S_NUM_0);
    }

    mic_ready = (ret == ESP_OK);
    last_stats.last_error = ret;
    ESP_LOGI(TAG,
             "I2S mic %s: legacy RX 16kHz 32-bit stereo WS=%d SCK=%d SD=%d gain=%.1f err=%d",
             mic_ready ? "initialized" : "init failed",
             (int)MIC_WS,
             (int)MIC_SCK,
             (int)MIC_SD,
             (double)I2S_MIC_GAIN,
             (int)ret);
    return mic_ready;
}

int i2s_mic_read(int16_t *samples, int max_samples)
{
    if (!mic_ready || !samples || max_samples <= 0) {
        last_stats.last_error = ESP_ERR_INVALID_ARG;
        last_stats.bytes_read = 0;
        last_stats.frames = 0;
        last_stats.channel = I2S_MIC_CHANNEL_NONE;
        return 0;
    }

    int bytes_to_read = max_samples * 2 * sizeof(int32_t);
    if (bytes_to_read > (int)sizeof(raw_buf))
        bytes_to_read = sizeof(raw_buf);

    size_t bytes_read = 0;
    esp_err_t ret = i2s_read(I2S_NUM_0, raw_buf, bytes_to_read, &bytes_read, pdMS_TO_TICKS(100));
    last_stats.last_error = ret;
    last_stats.bytes_read = bytes_read;
    last_stats.frames = 0;
    last_stats.output_rms = 0.0f;
    last_stats.output_peak = 0.0f;
    last_stats.dc_offset = 0.0f;
    if (ret != ESP_OK || bytes_read == 0) {
        return 0;
    }

    int frames = (int)(bytes_read / (2 * sizeof(int32_t)));
    if (frames > max_samples) frames = max_samples;

    if (frames <= 0) {
        return 0;
    }

    double sum_sq_left = 0.0;
    double sum_sq_right = 0.0;
    float peak_left = 0.0f;
    float peak_right = 0.0f;

    for (int i = 0; i < frames; i++) {
        float left = abs_unit(raw_buf[i * 2]);
        float right = abs_unit(raw_buf[i * 2 + 1]);

        sum_sq_left += (double)left * (double)left;
        sum_sq_right += (double)right * (double)right;
        if (left > peak_left) peak_left = left;
        if (right > peak_right) peak_right = right;
    }

    last_stats.frames = frames;
    last_stats.rms_left = sqrtf((float)(sum_sq_left / (double)frames));
    last_stats.rms_right = sqrtf((float)(sum_sq_right / (double)frames));
    last_stats.peak_left = peak_left;
    last_stats.peak_right = peak_right;
    last_stats.channel = (last_stats.rms_right > last_stats.rms_left) ? I2S_MIC_CHANNEL_RIGHT : I2S_MIC_CHANNEL_LEFT;

    const int selected_offset = (last_stats.channel == I2S_MIC_CHANNEL_RIGHT) ? 1 : 0;
    double sum_raw = 0.0;
    for (int i = 0; i < frames; i++) {
        sum_raw += (double)raw_buf[i * 2 + selected_offset];
    }
    const double dc_offset = sum_raw / (double)frames;
    last_stats.dc_offset = (float)(dc_offset / 2147483648.0);

    double sum_sq_out = 0.0;
    float peak_out = 0.0f;

    for (int i = 0; i < frames; i++) {
        int16_t sample = raw_to_pcm(raw_buf[i * 2 + selected_offset], dc_offset);
        float abs_sample = fabsf((float)sample / 32768.0f);
        samples[i] = sample;
        sum_sq_out += (double)abs_sample * (double)abs_sample;
        if (abs_sample > peak_out) peak_out = abs_sample;
    }

    last_stats.output_rms = sqrtf((float)(sum_sq_out / (double)frames));
    last_stats.output_peak = peak_out;
    return frames;
}

void i2s_mic_get_stats(i2s_mic_stats_t *stats)
{
    if (stats) {
        memcpy(stats, &last_stats, sizeof(last_stats));
    }
}

const char *i2s_mic_channel_name(i2s_mic_channel_t channel)
{
    switch (channel) {
    case I2S_MIC_CHANNEL_LEFT:
        return "LEFT";
    case I2S_MIC_CHANNEL_RIGHT:
        return "RIGHT";
    case I2S_MIC_CHANNEL_NONE:
    default:
        return "NONE";
    }
}

void i2s_mic_deinit(void)
{
    if (mic_ready) {
        i2s_driver_uninstall(I2S_NUM_0);
        mic_ready = false;
    }
}
