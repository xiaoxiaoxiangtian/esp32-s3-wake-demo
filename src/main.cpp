#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"

#include "i2s_mic.h"
#include "lcd_disp.h"

static const char *TAG = "wake_demo";
static const int WAKE_COOLDOWN_MS = 500;

static bool tick_before(TickType_t a, TickType_t b)
{
    return (int32_t)(a - b) < 0;
}

extern "C" void app_main(void)
{
    // ---- Init NVS ----
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // ---- Init I2S Mic ----
    if (!i2s_mic_init()) {
        ESP_LOGE(TAG, "Failed to init I2S mic");
        return;
    }

    // ---- Init LCD ----
    if (!lcd_disp_init()) {
        ESP_LOGE(TAG, "Failed to init LCD (continuing without display)");
    }
    lcd_disp_show_startup();
    vTaskDelay(pdMS_TO_TICKS(1500));

    // ---- Init ESP-SR Model System ----
    ESP_LOGI(TAG, "Loading models from 'model' partition...");
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models || models->num <= 0) {
        ESP_LOGE(TAG, "No models found in 'model' partition!");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "Found %d model(s) in partition", models->num);

    // Print all model names for debug
    for (int i = 0; i < models->num; i++) {
        const char *mname = models->model_name ? models->model_name[i] : "?";
        const char *minfo = models->model_info ? models->model_info[i] : "?";
        ESP_LOGI(TAG, "  model[%d]: name=%s info=%s", i, mname ? mname : "null", minfo ? minfo : "null");
    }

    // ---- Select WakeNet Model ----
    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "xiaoaitongxue");
    if (!model_name) {
        model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    }
    if (!model_name) {
        ESP_LOGE(TAG, "No WakeNet model found!");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "Selected model: %s", model_name);

    // Get human-readable wake word
    char *wake_word = esp_srmodel_get_wake_words(models, model_name);
    ESP_LOGI(TAG, "Wake word: %s", wake_word ? wake_word : model_name);

    // ---- Get WakeNet Interface ----
    const esp_wn_iface_t *wakenet = (const esp_wn_iface_t *)esp_wn_handle_from_name(model_name);
    if (!wakenet) {
        ESP_LOGE(TAG, "Failed to get WakeNet interface for %s", model_name);
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // ---- Create WakeNet Instance ----
    model_iface_data_t *wn_data = wakenet->create(model_name, DET_MODE_95);
    if (!wn_data) {
        ESP_LOGE(TAG, "Failed to create WakeNet instance");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // ---- Query Audio Parameters ----
    int sample_rate = wakenet->get_samp_rate(wn_data);
    int chunk_size = wakenet->get_samp_chunksize(wn_data);
    int channels = wakenet->get_channel_num(wn_data);
    int word_count = wakenet->get_word_num(wn_data);

    ESP_LOGI(TAG, "WakeNet ready: rate=%d chunk=%d channels=%d words=%d",
             sample_rate, chunk_size, channels, word_count);

    // Print all wake words with thresholds
    for (int i = 1; i <= word_count; i++) {
        char *word = wakenet->get_word_name(wn_data, i);
        float thresh = wakenet->get_det_threshold ? wakenet->get_det_threshold(wn_data, i) : 0.0f;
        ESP_LOGI(TAG, "  word[%d]: %s (threshold=%.3f)", i, word ? word : "?", thresh);
    }

    // Lower threshold for the standalone demo. ESP-SR allows 0.4 as the minimum.
    int threshold_ok = wakenet->set_det_threshold(wn_data, 0.4f, 1);
    ESP_LOGI(TAG, "WakeNet threshold set to 0.400: %s", threshold_ok ? "ok" : "failed");

    // ---- Allocate Detection Buffer ----
    int16_t *detect_buf = (int16_t *)heap_caps_malloc(
        chunk_size * channels * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!detect_buf) {
        detect_buf = (int16_t *)malloc(chunk_size * channels * sizeof(int16_t));
    }
    if (!detect_buf) {
        ESP_LOGE(TAG, "Failed to allocate detect buffer");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // ---- Mic Read Buffer ----
    int16_t *mic_buf = (int16_t *)malloc(I2S_MIC_BUFFER_SAMPLES * sizeof(int16_t));
    if (!mic_buf) {
        ESP_LOGE(TAG, "Failed to allocate mic buffer");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // ---- Show Idle Screen ----
    lcd_disp_show_idle();
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "Listening for \"%s\"...", wake_word ? wake_word : model_name);
    ESP_LOGI(TAG, "=========================================");

    // ---- Detection Loop ----
    int chunk_pos = 0;
    uint32_t detections = 0;
    uint32_t detect_chunks = 0;
    TickType_t last_audio_log = xTaskGetTickCount();
    TickType_t last_meter_update = xTaskGetTickCount();
    TickType_t detect_resume_at = 0;

    while (1) {
        // Read audio from I2S mic
        int frames = i2s_mic_read(mic_buf, I2S_MIC_BUFFER_SAMPLES);
        TickType_t now = xTaskGetTickCount();
        if (frames <= 0) {
            if ((now - last_meter_update) >= pdMS_TO_TICKS(100)) {
                lcd_disp_update_mic_level(0.0f);
                last_meter_update = now;
            }
            if ((now - last_audio_log) >= pdMS_TO_TICKS(1000)) {
                i2s_mic_stats_t stats;
                i2s_mic_get_stats(&stats);
                ESP_LOGW(TAG,
                         "Audio: no data err=%d bytes=%u frames=%d chunks=%lu",
                         (int)stats.last_error,
                         (unsigned)stats.bytes_read,
                         stats.frames,
                         (unsigned long)detect_chunks);
                last_audio_log = now;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        i2s_mic_stats_t stats;
        i2s_mic_get_stats(&stats);
        if ((now - last_meter_update) >= pdMS_TO_TICKS(100)) {
            lcd_disp_update_mic_level(stats.output_peak);
            last_meter_update = now;
        }

        if ((now - last_audio_log) >= pdMS_TO_TICKS(1000)) {
            ESP_LOGI(TAG,
                     "Audio: ch=%s frames=%d chunks=%lu in_rms(L/R)=%.5f/%.5f in_peak(L/R)=%.5f/%.5f out_rms=%.5f out_peak=%.5f dc=%.5f",
                     i2s_mic_channel_name(stats.channel),
                     stats.frames,
                     (unsigned long)detect_chunks,
                     (double)stats.rms_left,
                     (double)stats.rms_right,
                     (double)stats.peak_left,
                     (double)stats.peak_right,
                     (double)stats.output_rms,
                     (double)stats.output_peak,
                     (double)stats.dc_offset);
            last_audio_log = now;
        }

        if (detect_resume_at != 0) {
            if (tick_before(now, detect_resume_at)) {
                chunk_pos = 0;
                memset(detect_buf, 0, chunk_size * channels * sizeof(int16_t));
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            detect_resume_at = 0;
            ESP_LOGI(TAG, "WakeNet detection resumed");
        }

        // Feed into detect buffer
        for (int i = 0; i < frames; i++) {
            for (int ch = 0; ch < channels; ch++) {
                detect_buf[chunk_pos * channels + ch] = (ch == 0) ? mic_buf[i] : 0;
            }
            chunk_pos++;

            if (chunk_pos >= chunk_size) {
                // Full chunk - run detection
                int result = wakenet->detect(wn_data, detect_buf);
                detect_chunks++;
                chunk_pos = 0;
                memset(detect_buf, 0, chunk_size * channels * sizeof(int16_t));

                if (result > 0) {
                    detections++;
                    char *triggered_word = wakenet->get_word_name(wn_data, result);
                    ESP_LOGI(TAG, "=========================================");
                    ESP_LOGI(TAG, "DETECTED! #%lu  Wake word: %s (index=%d)",
                             (unsigned long)detections,
                             triggered_word ? triggered_word : "unknown", result);
                    ESP_LOGI(TAG, "=========================================");

                    // Show on LCD
                    lcd_disp_show_detected();
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    lcd_disp_show_idle();

                    // wakenet->clean() crashes this WakeNet9 build after detect().
                    // Keep the model running and discard a short tail of audio instead.
                    chunk_pos = 0;
                    memset(detect_buf, 0, chunk_size * channels * sizeof(int16_t));
                    detect_resume_at = xTaskGetTickCount() + pdMS_TO_TICKS(WAKE_COOLDOWN_MS);
                    ESP_LOGI(TAG, "WakeNet clean skipped; discarding %d ms of post-wake audio", WAKE_COOLDOWN_MS);
                    break;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // ---- Cleanup (unreachable) ----
    free(mic_buf);
    free(detect_buf);
    wakenet->destroy(wn_data);
    esp_srmodel_deinit(models);
    i2s_mic_deinit();
    lcd_disp_deinit();
}
