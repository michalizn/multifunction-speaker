#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "board.h"
#include "periph_touch.h"
#include "periph_adc_button.h"
#include "periph_button.h"
#include "audio_mem.h"
#include "bluetooth_service.h"
#include "fatfs_stream.h" 
#include "periph_sdcard.h"
#include "sdcard_list.h"
#include "sdcard_scan.h"
#include <stdio.h>
#include "audio_error.h"
#include "tone_stream.h"
#include "audio_tone_uri.h"
#include "equalizer.h"
#include "esp_netif.h"
#include "sdcard.h"
#include "driver/gpio.h"
#include <unistd.h>
#include "esp_timer.h"
#include "periph_led.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "a2dp_stream.h"
#include "audio_alc.h"

#define INIT_VOLUME         50
#define VOL_BOTTOM_TRH      0
#define VOL_UPPER_TRH       100
#define VOL_TRH             100
#define VOL_INCREASE_LOW    5
#define VOL_DECREASE_LOW    5
#define VOL_INCREASE_UP     5
#define VOL_DECREASE_UP     5

#define SHUTDOWN_GPIO       22
#define PA_GPIO             21
#define LOW_LVL             0
#define HIGH_LVL            1

#define ALC_VOLUME_SET      (0)

static const char *TAG = "MULTIFUNCTION_SPEAKER";

audio_pipeline_handle_t pipeline_init_mode;
audio_pipeline_handle_t pipeline_sd;
audio_pipeline_handle_t pipeline_http;
audio_pipeline_handle_t pipeline_bt;
audio_element_handle_t tone_stream_reader, http_stream_reader, fatfs_stream_reader, bt_stream_reader, i2s_stream_writer, mp3_decoder, equalizer, alc_el;

playlist_operator_handle_t sdcard_list_handle = NULL;
bool sd_card_cb = false;

static const char *radio_stations[] = {
	"http://icecast7.play.cz:8000/casradio128.mp3",
    "https://ice.actve.net/fm-evropa2-128",	 
    "https://icecast4.play.cz/kissjc128.mp3",
};

typedef enum {
    SD_CARD_DET = 1,
    SD_MODE_INIT,
    SD_MODE,
    BT_MODE_INIT,
    BT_MODE,
    WIFI_MODE_INIT,
    WIFI_MODE,
    RESTART_MODE
} service_mode_t;

void sdcard_url_save_cb(void *user_data, char *url)
{
    playlist_operator_handle_t sdcard_handle = (playlist_operator_handle_t)user_data;
    esp_err_t ret = sdcard_list_save(sdcard_handle, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to save sdcard url to sdcard playlist");
    }
}

static void bt_app_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p_param)
{
    esp_avrc_ct_cb_param_t *rc = p_param;
    switch (event) {
        case ESP_AVRC_CT_METADATA_RSP_EVT: {
            uint8_t *tmp = audio_calloc(1, rc->meta_rsp.attr_length + 1);
            memcpy(tmp, rc->meta_rsp.attr_text, rc->meta_rsp.attr_length);
            ESP_LOGI(TAG, "AVRC metadata rsp: attribute id 0x%x, %s", rc->meta_rsp.attr_id, tmp);
            audio_free(tmp);
            break;
        }
        default:
            break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "[ 0.0 ] Configure the IOMUX register for SHUTDOWN_GPIO");
    gpio_pad_select_gpio(SHUTDOWN_GPIO);
    gpio_set_direction(SHUTDOWN_GPIO, GPIO_MODE_OUTPUT);

    ESP_LOGI(TAG, "[ 0.1 ] Turn the SHUTDOWN pin LOW");
    gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
    gpio_set_level(PA_GPIO, LOW_LVL);

    ESP_LOGI(TAG, "[ 0.2 ] SD card detection");
    if (gpio_get_level(34) == 0) {
        sd_card_cb = true;
    }
    // bool sd_card_cb = sdcard_is_exist();
    ESP_LOGI(TAG, "%i", sd_card_cb);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "[ 1.0 ] Initialize peripherals management");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    ESP_LOGI(TAG, "[ 2.0 ] Start audio codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[ 3.0 ] Setup the volume");
    int player_volume;
    audio_hal_get_volume(board_handle->audio_hal, &player_volume);
    player_volume = INIT_VOLUME;
    audio_hal_set_volume(board_handle->audio_hal, player_volume);
    ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);

    while (1) {
        static service_mode_t mode = SD_CARD_DET;
        switch (mode) {
            case SD_CARD_DET: {
                if (sd_card_cb == true) {
                    ESP_LOGI(TAG, "Initialize and start peripherals");
                    audio_board_sdcard_init(set, SD_MODE_1_LINE);
                    audio_board_key_init(set);

                    ESP_LOGI(TAG, "Set up a sdcard playlist and scan sdcard music save to it");
                    sdcard_list_create(&sdcard_list_handle);
                    sdcard_scan(sdcard_url_save_cb, "/sdcard", 0, (const char *[]) {"mp3"}, 1, sdcard_list_handle);
                    sdcard_list_show(sdcard_list_handle);

                    mode = SD_MODE_INIT;
                }
                if (sd_card_cb == false) {
                    mode = BT_MODE_INIT;
                }
                break;
            }
            case SD_MODE_INIT: {
                ESP_LOGI(TAG, "SD MODE INIT");
                player_volume = INIT_VOLUME;
                audio_hal_set_volume(board_handle->audio_hal, player_volume);
                ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
                
                ESP_LOGI(TAG, "[ 1.0 ] Create audio pipeline for playback");
                audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
                pipeline_init_mode = audio_pipeline_init(&pipeline_cfg);
                AUDIO_NULL_CHECK(TAG, pipeline_init_mode, return);

                ESP_LOGI(TAG, "[ 1.1 ] Create tone stream to read data from flash");
                tone_stream_cfg_t tone_cfg = TONE_STREAM_CFG_DEFAULT();
                tone_cfg.type = AUDIO_STREAM_READER;
                tone_stream_reader = tone_stream_init(&tone_cfg);
                AUDIO_NULL_CHECK(TAG, tone_stream_reader, return);

                ESP_LOGI(TAG, "[ 1.2 ] Create i2s stream to write data to codec chip");
                i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
                i2s_cfg.type = AUDIO_STREAM_WRITER;
                i2s_stream_writer = i2s_stream_init(&i2s_cfg);
                AUDIO_NULL_CHECK(TAG, i2s_stream_writer, return);

                ESP_LOGI(TAG, "[ 1.3 ] Create mp3 decoder to decode mp3 file");
                mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
                mp3_decoder = mp3_decoder_init(&mp3_cfg);
                AUDIO_NULL_CHECK(TAG, mp3_decoder, return);

                ESP_LOGI(TAG, "[ 2.0 ] Register all elements to audio pipeline");
                audio_pipeline_register(pipeline_init_mode, tone_stream_reader, "tone");
                audio_pipeline_register(pipeline_init_mode, mp3_decoder, "mp3");
                audio_pipeline_register(pipeline_init_mode, i2s_stream_writer, "i2s");

                ESP_LOGI(TAG, "[ 2.1 ] Link it together [flash]-->tone_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
                const char *link_tag[3] = {"tone", "mp3", "i2s"};
                audio_pipeline_link(pipeline_init_mode, &link_tag[0], 3);

                ESP_LOGI(TAG, "[ 2.2 ] Set up  uri (file as tone_stream, mp3 as mp3 decoder, and default output is i2s)");
                audio_element_set_uri(tone_stream_reader, tone_uri[TONE_TYPE_SD_MODE]);

                ESP_LOGI(TAG, "[ 3 ] Set up event listener");
                audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
                audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

                ESP_LOGI(TAG, "[ 3.1 ] Listening event from all elements of pipeline");
                audio_pipeline_set_listener(pipeline_init_mode, evt);

                ESP_LOGI(TAG, "[ 3.2 ] Turn the SHUTDOWN HIGH");
                gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);

                ESP_LOGI(TAG, "[ 4 ] Start audio_pipeline");
                audio_pipeline_run(pipeline_init_mode);

                ESP_LOGI(TAG, "[ 5 ] Listen for all pipeline events");
                while (1) {
                    audio_event_iface_msg_t msg = { 0 };
                    esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
                        continue;
                    }

                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) mp3_decoder
                        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                        audio_element_info_t music_info = {0};
                        audio_element_getinfo(mp3_decoder, &music_info);

                        ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                                music_info.sample_rates, music_info.bits, music_info.channels);

                        audio_element_setinfo(i2s_stream_writer, &music_info);
                        i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
                        continue;
                    }

                    /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
                        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                        && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
                        ESP_LOGW(TAG, "[ * ] Stop event received");
                        break;
                    }
                }
                ESP_LOGI(TAG, "[ 6 ] Turn the SHUTDOWN LOW");
                gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);

                ESP_LOGI(TAG, "[ 7 ] Stop audio_pipeline");
                audio_pipeline_stop(pipeline_init_mode);
                audio_pipeline_wait_for_stop(pipeline_init_mode);
                audio_pipeline_terminate(pipeline_init_mode);

                audio_pipeline_unregister(pipeline_init_mode, tone_stream_reader);
                audio_pipeline_unregister(pipeline_init_mode, i2s_stream_writer);
                audio_pipeline_unregister(pipeline_init_mode, mp3_decoder);

                /* Terminal the pipeline before removing the listener */
                audio_pipeline_remove_listener(pipeline_init_mode);

                /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
                audio_event_iface_destroy(evt);

                /* Release all resources */
                audio_pipeline_deinit(pipeline_init_mode);
                audio_element_deinit(tone_stream_reader);
                audio_element_deinit(i2s_stream_writer);
                audio_element_deinit(mp3_decoder);
                mode = SD_MODE;
                break;
            }
            case SD_MODE: {
                ESP_LOGI(TAG, "SD CARD MODE");
                ESP_LOGI(TAG, "[ 1.0 ] Create audio pipeline for playback");
                audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
                pipeline_sd = audio_pipeline_init(&pipeline_cfg);
                mem_assert(pipeline_sd);

                ESP_LOGI(TAG, "[ 1.1 ] Create i2s stream to write data to codec chip");
                i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
                i2s_cfg.type = AUDIO_STREAM_WRITER;
                i2s_cfg.use_alc = true;
                i2s_stream_writer = i2s_stream_init(&i2s_cfg);

                ESP_LOGI(TAG, "[ 1.2 ] Create mp3 decoder to decode mp3 file");
                mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
                mp3_decoder = mp3_decoder_init(&mp3_cfg);

                ESP_LOGI(TAG, "[ 1.3 ] Create ALC");
                alc_volume_setup_cfg_t alc_cfg = DEFAULT_ALC_VOLUME_SETUP_CONFIG();
                alc_el = alc_volume_setup_init(&alc_cfg);

                ESP_LOGI(TAG, "[ 1.4 ] Create equalizer");
                equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
                /*                  31   62  125 250  500  1k   2k   4k   8k  16k  31   62  125 250  500  1k   2k   4k   8k  16k  [Hz]                   */
                int set_gain[] = { 4, 6, 6, 3, 2, -50, -89, -89, -50, 2, 4, 6, 6, 3, 2, -50, -89, -89, -50, 2};
                eq_cfg.set_gain =
                    set_gain; // The size of gain array should be the multiplication of NUMBER_BAND and number channels of audio stream data. The minimum of gain is -10 dB.
                equalizer = equalizer_init(&eq_cfg);

                ESP_LOGI(TAG, "[ 1.5 ] Create fatfs stream to read data from sdcard");
                char *url = NULL;
                sdcard_list_current(sdcard_list_handle, &url);
                fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
                fatfs_cfg.type = AUDIO_STREAM_READER;
                fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);
                audio_element_set_uri(fatfs_stream_reader, url);

                ESP_LOGI(TAG, "[ 2.0 ] Register all elements to audio pipeline");
                audio_pipeline_register(pipeline_sd, fatfs_stream_reader, "file");
                audio_pipeline_register(pipeline_sd, mp3_decoder, "mp3");
                audio_pipeline_register(pipeline_sd, equalizer, "equalizer");
                audio_pipeline_register(pipeline_sd, alc_el, "alc");
                audio_pipeline_register(pipeline_sd, i2s_stream_writer, "i2s");

                ESP_LOGI(TAG, "[ 2.1 ] Link it together [sdcard]-->fatfs_stream-->mp3_decoder-->resample-->i2s_stream-->[codec_chip]");
                const char *link_tag[5] = {"file", "mp3", "equalizer", "alc", "i2s"};
                audio_pipeline_link(pipeline_sd, &link_tag[0], 5);

                ESP_LOGI(TAG, "[ 3.0 ] Set up  event listener");
                audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
                audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

                ESP_LOGI(TAG, "[ 3.1 ] Listen for all pipeline events");
                audio_pipeline_set_listener(pipeline_sd, evt);

                ESP_LOGI(TAG, "[ 4 ] Listening event from peripherals");
                audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

                ESP_LOGI(TAG, "[ 4.1 ] Turn the SHUTDOWN HIGH");
                if (player_volume == 0) {
                    gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                } else {
                    gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);
                }

                ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
                audio_pipeline_run(pipeline_sd);

                while (mode == SD_MODE) {
                    /* Handle event interface messages from pipeline
                    to set music info and to advance to the next song
                    */
                    audio_event_iface_msg_t msg;
                    esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
                        continue;
                    }
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) {
                        // Set music info for a new song to be played
                        if (msg.source == (void *) mp3_decoder
                            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                            audio_element_info_t music_info = {0};
                            audio_element_getinfo(mp3_decoder, &music_info);
                            ESP_LOGI(TAG, "[ * ] Received music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                                    music_info.sample_rates, music_info.bits, music_info.channels);
                            audio_element_setinfo(i2s_stream_writer, &music_info);

                            alc_volume_setup_set_channel(alc_el, music_info.channels);
                            alc_volume_setup_set_volume(alc_el, ALC_VOLUME_SET);

                            continue;
                        }
                        // Advance to the next song when previous finishes
                        if (msg.source == (void *) i2s_stream_writer
                            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS) {
                            audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
                            if (el_state == AEL_STATE_FINISHED) {
                                ESP_LOGI(TAG, "[ * ] Finished, advancing to the next song");
                                sdcard_list_next(sdcard_list_handle, 1, &url);
                                ESP_LOGW(TAG, "URL: %s", url);
                                audio_element_set_uri(fatfs_stream_reader, url);
                                audio_pipeline_reset_ringbuffer(pipeline_sd);
                                audio_pipeline_reset_elements(pipeline_sd);
                                audio_pipeline_change_state(pipeline_sd, AEL_STATE_INIT);
                                audio_pipeline_run(pipeline_sd);
                            }
                            continue;
                        }
                    }
                    if ((msg.source_type == PERIPH_ID_TOUCH || msg.source_type == PERIPH_ID_BUTTON || msg.source_type == PERIPH_ID_ADC_BTN)
                        && (msg.cmd == PERIPH_TOUCH_TAP || msg.cmd == PERIPH_BUTTON_PRESSED || msg.cmd == PERIPH_ADC_BUTTON_PRESSED)) {

                        if ((int) msg.data == get_input_mode_id()) {
                            ESP_LOGI(TAG, "[ * ] [Mode] touch tap event");
                            gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                            mode = BT_MODE_INIT;
                        } else if ((int) msg.data == get_input_play_id()) {
                            ESP_LOGI(TAG, "[ * ] [Play] touch tap event");
                            audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
                            switch (el_state) {
                                case AEL_STATE_INIT :
                                    ESP_LOGI(TAG, "[ * ] Starting audio pipeline");
                                    if (player_volume == 0) {
                                        gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                                    } else {
                                        gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);
                                    }
                                    audio_pipeline_run(pipeline_sd);
                                    break;
                                case AEL_STATE_RUNNING :
                                    ESP_LOGI(TAG, "[ * ] Pausing audio pipeline");
                                    gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                                    audio_pipeline_pause(pipeline_sd);
                                    break;
                                case AEL_STATE_PAUSED :
                                    ESP_LOGI(TAG, "[ * ] Resuming audio pipeline");
                                    if (player_volume == 0) {
                                        gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                                    } else {
                                        gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);
                                    }
                                    audio_pipeline_resume(pipeline_sd);
                                    break;
                                default :
                                    ESP_LOGI(TAG, "[ * ] Not supported state %d", el_state);
                            }
                        } else if ((int) msg.data == get_input_rec_id()) {
                            ESP_LOGI(TAG, "[ * ] [Rec] touch tap event");
                            ESP_LOGI(TAG, "[ * ] Stopped, advancing to the prev song");
                            gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                            char *url = NULL;
                            audio_pipeline_stop(pipeline_sd);
                            audio_pipeline_wait_for_stop(pipeline_sd);
                            audio_pipeline_terminate(pipeline_sd);
                            sdcard_list_prev(sdcard_list_handle, 1, &url);
                            ESP_LOGW(TAG, "URL: %s", url);
                            audio_element_set_uri(fatfs_stream_reader, url);
                            audio_pipeline_reset_ringbuffer(pipeline_sd);
                            audio_pipeline_reset_elements(pipeline_sd);
                            audio_pipeline_run(pipeline_sd);
                            break;
                        } else if ((int) msg.data == get_input_set_id()) {
                            ESP_LOGI(TAG, "[ * ] [Set] touch tap event");
                            ESP_LOGI(TAG, "[ * ] Stopped, advancing to the next song");
                            gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                            char *url = NULL;
                            audio_pipeline_stop(pipeline_sd);
                            audio_pipeline_wait_for_stop(pipeline_sd);
                            audio_pipeline_terminate(pipeline_sd);
                            sdcard_list_next(sdcard_list_handle, 1, &url);
                            ESP_LOGW(TAG, "URL: %s", url);
                            audio_element_set_uri(fatfs_stream_reader, url);
                            audio_pipeline_reset_ringbuffer(pipeline_sd);
                            audio_pipeline_reset_elements(pipeline_sd);
                            audio_pipeline_run(pipeline_sd);
                            break;
                        } else if ((int) msg.data == get_input_volup_id()) {
                            ESP_LOGI(TAG, "[ * ] [Vol+] touch tap event");
                            audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
                            if (el_state == AEL_STATE_PAUSED) {
                                gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                            } else {
                                gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);
                            }
                            if (player_volume == 0) {
                                player_volume = VOL_BOTTOM_TRH;
                            }
                            if (player_volume <= VOL_TRH) {
                                player_volume += VOL_INCREASE_LOW;
                            } else {
                                player_volume += VOL_INCREASE_UP;
                            }
                            if (player_volume >= VOL_UPPER_TRH) {
                                player_volume = VOL_UPPER_TRH;
                            }
                            audio_hal_set_volume(board_handle->audio_hal, player_volume);
                            ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
                        } else if ((int) msg.data == get_input_voldown_id()) {
                            ESP_LOGI(TAG, "[ * ] [Vol-] touch tap event");
                            if (player_volume <= VOL_TRH) {
                                player_volume -= VOL_DECREASE_LOW;
                            } else {
                                player_volume -= VOL_DECREASE_UP;
                            }
                            if (player_volume <= VOL_BOTTOM_TRH) {
                                player_volume = 0;
                                gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                            }
                            audio_hal_set_volume(board_handle->audio_hal, player_volume);
                            ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
                        }
                    }
                }

                ESP_LOGI(TAG, "[ 7 ] Stop audio_pipeline");
                audio_pipeline_stop(pipeline_sd);
                audio_pipeline_wait_for_stop(pipeline_sd);
                audio_pipeline_terminate(pipeline_sd);

                audio_pipeline_unregister(pipeline_sd, mp3_decoder);
                audio_pipeline_unregister(pipeline_sd, alc_el);
                audio_pipeline_unregister(pipeline_sd, equalizer);
                audio_pipeline_unregister(pipeline_sd, i2s_stream_writer);

                /* Terminate the pipeline before removing the listener */
                audio_pipeline_remove_listener(pipeline_sd);

                /* Stop all peripherals before removing the listener */
                audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

                /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
                audio_event_iface_destroy(evt);

                /* Release all resources */
                audio_pipeline_deinit(pipeline_sd);
                audio_element_deinit(i2s_stream_writer);
                audio_element_deinit(mp3_decoder);
                audio_element_deinit(alc_el);
                audio_element_deinit(equalizer);

                if (mode != SD_MODE) {
                    ESP_LOGW(TAG, "[ * ] SD card destroyed");
                    sdcard_list_destroy(sdcard_list_handle);
                    esp_periph_set_destroy(set);
                }
                break;
            }
            case BT_MODE_INIT: {
                ESP_LOGI(TAG, "BT MODE INIT");
                ESP_LOGI(TAG, "[ 0.0 ] Setup the volume");
                player_volume = INIT_VOLUME;
                audio_hal_set_volume(board_handle->audio_hal, player_volume);
                ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);

                ESP_LOGI(TAG, "[ 1.0 ] Create audio pipeline for playback");
                audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
                pipeline_init_mode = audio_pipeline_init(&pipeline_cfg);
                AUDIO_NULL_CHECK(TAG, pipeline_init_mode, return);

                ESP_LOGI(TAG, "[ 1.1 ] Create tone stream to read data from flash");
                tone_stream_cfg_t tone_cfg = TONE_STREAM_CFG_DEFAULT();
                tone_cfg.type = AUDIO_STREAM_READER;
                tone_stream_reader = tone_stream_init(&tone_cfg);
                AUDIO_NULL_CHECK(TAG, tone_stream_reader, return);

                ESP_LOGI(TAG, "[ 1.2 ] Create i2s stream to write data to codec chip");
                i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
                i2s_cfg.type = AUDIO_STREAM_WRITER;
                i2s_stream_writer = i2s_stream_init(&i2s_cfg);
                AUDIO_NULL_CHECK(TAG, i2s_stream_writer, return);

                ESP_LOGI(TAG, "[ 1.3 ] Create mp3 decoder to decode mp3 file");
                mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
                mp3_decoder = mp3_decoder_init(&mp3_cfg);
                AUDIO_NULL_CHECK(TAG, mp3_decoder, return);

                ESP_LOGI(TAG, "[ 2.0 ] Register all elements to audio pipeline");
                audio_pipeline_register(pipeline_init_mode, tone_stream_reader, "tone");
                audio_pipeline_register(pipeline_init_mode, mp3_decoder, "mp3");
                audio_pipeline_register(pipeline_init_mode, i2s_stream_writer, "i2s");

                ESP_LOGI(TAG, "[ 2.1 ] Link it together [flash]-->tone_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
                const char *link_tag[3] = {"tone", "mp3", "i2s"};
                audio_pipeline_link(pipeline_init_mode, &link_tag[0], 3);

                ESP_LOGI(TAG, "[ 2.2 ] Set up  uri (file as tone_stream, mp3 as mp3 decoder, and default output is i2s)");
                audio_element_set_uri(tone_stream_reader, tone_uri[TONE_TYPE_BT_MODE]);

                ESP_LOGI(TAG, "[ 3 ] Set up event listener");
                audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
                audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

                ESP_LOGI(TAG, "[ 3.1 ] Listening event from all elements of pipeline");
                audio_pipeline_set_listener(pipeline_init_mode, evt);

                ESP_LOGI(TAG, "[ 3.2 ] Turn the SHUTDOWN HIGH");
                gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);

                ESP_LOGI(TAG, "[ 4 ] Start audio_pipeline");
                audio_pipeline_run(pipeline_init_mode);

                ESP_LOGI(TAG, "[ 5 ] Listen for all pipeline events");
                while (1) {
                    audio_event_iface_msg_t msg = { 0 };
                    esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
                        continue;
                    }

                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) mp3_decoder
                        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                        audio_element_info_t music_info = {0};
                        audio_element_getinfo(mp3_decoder, &music_info);

                        ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                                music_info.sample_rates, music_info.bits, music_info.channels);

                        audio_element_setinfo(i2s_stream_writer, &music_info);
                        i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
                        continue;
                    }

                    /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
                        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                        && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
                        ESP_LOGW(TAG, "[ * ] Stop event received");
                        break;
                    }
                }

                ESP_LOGI(TAG, "[ 6 ] Turn the SHUTDOWN LOW");
                gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);

                ESP_LOGI(TAG, "[ 7 ] Stop audio_pipeline");
                audio_pipeline_stop(pipeline_init_mode);
                audio_pipeline_wait_for_stop(pipeline_init_mode);
                audio_pipeline_terminate(pipeline_init_mode);

                audio_pipeline_unregister(pipeline_init_mode, tone_stream_reader);
                audio_pipeline_unregister(pipeline_init_mode, i2s_stream_writer);
                audio_pipeline_unregister(pipeline_init_mode, mp3_decoder);

                /* Terminal the pipeline before removing the listener */
                audio_pipeline_remove_listener(pipeline_init_mode);

                /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
                audio_event_iface_destroy(evt);

                /* Release all resources */
                audio_pipeline_deinit(pipeline_init_mode);
                audio_element_deinit(tone_stream_reader);
                audio_element_deinit(i2s_stream_writer);
                audio_element_deinit(mp3_decoder);
                mode = BT_MODE;
                break;
            }
            case BT_MODE: {
                ESP_LOGI(TAG, "BT MODE");
                ESP_LOGI(TAG, "[ 1.0 ] Create Bluetooth service");
                bluetooth_service_cfg_t bt_cfg = {
                    .device_name = "MULTIFUNCTION-SPEAKER",
                    .mode = BLUETOOTH_A2DP_SINK,
                    .user_callback.user_avrc_ct_cb = bt_app_avrc_ct_cb,
                    .audio_hal = board_handle->audio_hal,
                };
                bluetooth_service_start(&bt_cfg);

                ESP_LOGI(TAG, "[1.1] Initialize peripherals management");
                esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
                esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

                ESP_LOGI(TAG, "[ 1.2 ] Initialize and start peripherals");
                audio_board_key_init(set);

                ESP_LOGI(TAG, "[ 2.0 ] Create audio pipeline for playback");
                audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
                pipeline_bt = audio_pipeline_init(&pipeline_cfg);

                ESP_LOGI(TAG, "[ 2.2 ] Create ALC");
                alc_volume_setup_cfg_t alc_cfg = DEFAULT_ALC_VOLUME_SETUP_CONFIG();
                alc_el = alc_volume_setup_init(&alc_cfg);

                ESP_LOGI(TAG, "[ 2.1 ] Create equalizer");
                equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
                /*                  31   62  125 250  500  1k   2k   4k   8k  16k  31   62  125 250  500  1k   2k   4k   8k  16k  [Hz]                   */
                //int set_gain[] = { -26, -16, -4, -8, -14, -89, -89, -89, -66, -4, -26, -16, -4, -8, -14, -89, -89, -89, -66, -4};                     // 1st version
                //int set_gain[] = { -4, -4, 2, -6, -14, -89, -89, -89, -50, 0, -4, -4, 2, -6, -14, -89, -89, -89, -50, 0};                             // 2nd version
                int set_gain[] = { 4, 6, 6, 3, 2, -50, -89, -89, -50, 2, 4, 6, 6, 3, 2, -50, -89, -89, -50, 2};                                         // final ?   
                eq_cfg.set_gain =
                    set_gain; // The size of gain array should be the multiplication of NUMBER_BAND and number channels of audio stream data. The minimum of gain is -10 dB.
                equalizer = equalizer_init(&eq_cfg);

                ESP_LOGI(TAG, "[ 2.2 ] Create i2s stream to write data to codec chip");
                i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
                i2s_cfg.type = AUDIO_STREAM_WRITER;
                i2s_stream_writer = i2s_stream_init(&i2s_cfg);

                ESP_LOGI(TAG, "[ 2.3 ] Get Bluetooth stream");
                bt_stream_reader = bluetooth_service_create_stream();

                ESP_LOGI(TAG, "[ 3.0 ] Register all elements to audio pipeline");
                audio_pipeline_register(pipeline_bt, bt_stream_reader, "bt");
                audio_pipeline_register(pipeline_bt, equalizer, "equalizer");
                audio_pipeline_register(pipeline_bt, alc_el, "alc");
                audio_pipeline_register(pipeline_bt, i2s_stream_writer, "i2s");

                ESP_LOGI(TAG, "[ 3.1 ] Link it together [Bluetooth]-->bt_stream_reader-->i2s_stream_writer-->[codec_chip]");
                const char *link_tag[4] = {"bt", "equalizer", "alc", "i2s"};
                audio_pipeline_link(pipeline_bt, &link_tag[0], 4);

                ESP_LOGI(TAG, "[ 4.0 ] Create Bluetooth peripheral");
                esp_periph_handle_t bt_periph = bluetooth_service_create_periph();

                ESP_LOGI(TAG, "[ 4.1 ] Start all peripherals");
                esp_periph_start(set, bt_periph);

                ESP_LOGI(TAG, "[ 5.0 ] Set up  event listener");
                audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
                audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

                ESP_LOGI(TAG, "[ 5.1 ] Listening event from all elements of pipeline");
                audio_pipeline_set_listener(pipeline_bt, evt);

                ESP_LOGI(TAG, "[ 5.2 ] Listening event from peripherals");
                audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

                ESP_LOGI(TAG, "[ 6 ] Start audio_pipeline");
                audio_pipeline_run(pipeline_bt);

                ESP_LOGI(TAG, "[ 7 ] Listen for all pipeline events");
                while (mode == BT_MODE) {
                    audio_event_iface_msg_t msg;
                    esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
                        continue;
                    }

                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) bt_stream_reader
                        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                        audio_element_info_t music_info = {0};
                        audio_element_getinfo(bt_stream_reader, &music_info);

                        ESP_LOGI(TAG, "[ * ] Receive music info from Bluetooth, sample_rates=%d, bits=%d, ch=%d",
                                music_info.sample_rates, music_info.bits, music_info.channels);

                        audio_element_setinfo(i2s_stream_writer, &music_info);

                        alc_volume_setup_set_channel(alc_el, music_info.channels);
                        alc_volume_setup_set_volume(alc_el, ALC_VOLUME_SET);

                        i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
                        continue;
                    }

                    if ((msg.source_type == PERIPH_ID_TOUCH || msg.source_type == PERIPH_ID_BUTTON || msg.source_type == PERIPH_ID_ADC_BTN)
                        && (msg.cmd == PERIPH_TOUCH_TAP || msg.cmd == PERIPH_BUTTON_PRESSED || msg.cmd == PERIPH_ADC_BUTTON_PRESSED)) {
                        if ((int) msg.data == get_input_mode_id()) {
                            ESP_LOGI(TAG, "[ * ] [Mode] touch tap event");
                            periph_bluetooth_stop(bt_periph);
                            gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                            mode = WIFI_MODE_INIT;
                        } else if ((int) msg.data == get_input_play_id()) {
                            ESP_LOGI(TAG, "[ * ] [Play] touch tap event");
                            periph_bluetooth_play_pause(bt_periph);
                        } else if ((int) msg.data == get_input_rec_id()) {
                            ESP_LOGI(TAG, "[ * ] [Rec] touch tap event");
                            periph_bluetooth_prev(bt_periph);
                        } else if ((int) msg.data == get_input_set_id()) {
                            ESP_LOGI(TAG, "[ * ] [Set] touch tap event");
                            periph_bluetooth_next(bt_periph);
                        } else if ((int) msg.data == get_input_volup_id()) {
                            ESP_LOGI(TAG, "[ * ] [Vol+] touch tap event");
                            periph_bluetooth_volume_up(bt_periph);
                        } else if ((int) msg.data == get_input_voldown_id()) {
                            ESP_LOGI(TAG, "[ * ] [Vol-] touch tap event");
                            periph_bluetooth_volume_down(bt_periph);
                        }
                    }

                    /* Stop when the Bluetooth is disconnected or suspended */
                    if (msg.source_type == PERIPH_ID_BLUETOOTH
                        && msg.source == (void *)bt_periph) {
                        if (msg.cmd == PERIPH_BLUETOOTH_DISCONNECTED) {
                            ESP_LOGW(TAG, "[ * ] Bluetooth disconnected");
                            break;
                        }
                    }
                    /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
                        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                        && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
                        ESP_LOGW(TAG, "[ * ] Stop event received");
                        break;
                    }
                }

                ESP_LOGI(TAG, "[ 8 ] Stop audio_pipeline");
                audio_pipeline_stop(pipeline_bt);
                audio_pipeline_wait_for_stop(pipeline_bt);
                audio_pipeline_terminate(pipeline_bt);

                audio_pipeline_unregister(pipeline_bt, bt_stream_reader);
                audio_pipeline_unregister(pipeline_bt, alc_el);
                audio_pipeline_unregister(pipeline_bt, equalizer);
                audio_pipeline_unregister(pipeline_bt, i2s_stream_writer);

                /* Terminate the pipeline before removing the listener */
                audio_pipeline_remove_listener(pipeline_bt);

                /* Stop all peripherals before removing the listener */
                esp_periph_set_stop_all(set);
                audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

                /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
                audio_event_iface_destroy(evt);

                /* Release all resources */
                audio_pipeline_deinit(pipeline_bt);
                audio_element_deinit(bt_stream_reader);
                audio_element_deinit(alc_el);
                audio_element_deinit(equalizer);
                audio_element_deinit(i2s_stream_writer);
                esp_periph_set_destroy(set);
                bluetooth_service_destroy();
                ESP_LOGW(TAG, "[ * ] Bluetooth destroyed");
                break;
            }
            case WIFI_MODE_INIT: {
                ESP_LOGI(TAG, "WIFI MODE INIT");

                ESP_LOGI(TAG, "[ 0.0 ] Setup the volume");
                player_volume = INIT_VOLUME;
                audio_hal_set_volume(board_handle->audio_hal, player_volume);
                ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);

                ESP_LOGI(TAG, "[ 1.0 ] Create audio pipeline for playback");
                audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
                pipeline_init_mode = audio_pipeline_init(&pipeline_cfg);
                AUDIO_NULL_CHECK(TAG, pipeline_init_mode, return);

                ESP_LOGI(TAG, "[ 1.1 ] Create tone stream to read data from flash");
                tone_stream_cfg_t tone_cfg = TONE_STREAM_CFG_DEFAULT();
                tone_cfg.type = AUDIO_STREAM_READER;
                tone_stream_reader = tone_stream_init(&tone_cfg);
                AUDIO_NULL_CHECK(TAG, tone_stream_reader, return);

                ESP_LOGI(TAG, "[ 1.2 ] Create i2s stream to write data to codec chip");
                i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
                i2s_cfg.type = AUDIO_STREAM_WRITER;
                i2s_stream_writer = i2s_stream_init(&i2s_cfg);
                AUDIO_NULL_CHECK(TAG, i2s_stream_writer, return);

                ESP_LOGI(TAG, "[ 1.3 ] Create mp3 decoder to decode mp3 file");
                mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
                mp3_decoder = mp3_decoder_init(&mp3_cfg);
                AUDIO_NULL_CHECK(TAG, mp3_decoder, return);

                ESP_LOGI(TAG, "[ 2.0 ] Register all elements to audio pipeline");
                audio_pipeline_register(pipeline_init_mode, tone_stream_reader, "tone");
                audio_pipeline_register(pipeline_init_mode, mp3_decoder, "mp3");
                audio_pipeline_register(pipeline_init_mode, i2s_stream_writer, "i2s");

                ESP_LOGI(TAG, "[ 2.1 ] Link it together [flash]-->tone_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
                const char *link_tag[3] = {"tone", "mp3", "i2s"};
                audio_pipeline_link(pipeline_init_mode, &link_tag[0], 3);

                ESP_LOGI(TAG, "[ 2.2 ] Set up  uri (file as tone_stream, mp3 as mp3 decoder, and default output is i2s)");
                audio_element_set_uri(tone_stream_reader, tone_uri[TONE_TYPE_WIFI_MODE]);

                ESP_LOGI(TAG, "[ 3.0 ] Set up event listener");
                audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
                audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

                ESP_LOGI(TAG, "[ 3.1 ] Listening event from all elements of pipeline");
                audio_pipeline_set_listener(pipeline_init_mode, evt);

                ESP_LOGI(TAG, "[ 3.2 ] Turn the SHUTDOWN HIGH");
                gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);

                ESP_LOGI(TAG, "[ 4 ] Start audio_pipeline");
                audio_pipeline_run(pipeline_init_mode);

                ESP_LOGI(TAG, "[ 5 ] Listen for all pipeline events");
                while (1) {
                    audio_event_iface_msg_t msg = { 0 };
                    esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
                        continue;
                    }

                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) mp3_decoder
                        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                        audio_element_info_t music_info = {0};
                        audio_element_getinfo(mp3_decoder, &music_info);

                        ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                                music_info.sample_rates, music_info.bits, music_info.channels);

                        audio_element_setinfo(i2s_stream_writer, &music_info);
                        i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
                        continue;
                    }

                    /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
                        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                        && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
                        ESP_LOGW(TAG, "[ * ] Stop event received");
                        break;
                    }
                }

                ESP_LOGI(TAG, "[ 6 ] Turn the SHUTDOWN LOW");
                gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);

                ESP_LOGI(TAG, "[ 7 ] Stop audio_pipeline");
                audio_pipeline_stop(pipeline_init_mode);
                audio_pipeline_wait_for_stop(pipeline_init_mode);
                audio_pipeline_terminate(pipeline_init_mode);

                audio_pipeline_unregister(pipeline_init_mode, tone_stream_reader);
                audio_pipeline_unregister(pipeline_init_mode, i2s_stream_writer);
                audio_pipeline_unregister(pipeline_init_mode, mp3_decoder);

                /* Terminal the pipeline before removing the listener */
                audio_pipeline_remove_listener(pipeline_init_mode);

                /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
                audio_event_iface_destroy(evt);

                /* Release all resources */
                audio_pipeline_deinit(pipeline_init_mode);
                audio_element_deinit(tone_stream_reader);
                audio_element_deinit(i2s_stream_writer);
                audio_element_deinit(mp3_decoder);
                mode = WIFI_MODE;
                break;
            }
            case WIFI_MODE: {
                ESP_LOGI(TAG, "WIFI MODE");
                ESP_LOGI(TAG, "[ 0.0 ] Initialize variables");
                static uint8_t radio_stations_count = (sizeof(radio_stations)/sizeof(char*));
                static uint8_t radio_station_index = 0;

                ESP_LOGI(TAG, "[ 1.0 ] Initialize peripherals management");
                esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
                esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

                ESP_LOGI(TAG, "[ 1.1 ] Initialize and start peripherals");
                audio_board_key_init(set);

                ESP_LOGI(TAG, "[ 1.2 ] Start and wait for Wi-Fi network");
                periph_wifi_cfg_t wifi_cfg = {
                    .ssid = CONFIG_WIFI_SSID,
                    .password = CONFIG_WIFI_PASSWORD,
                };

                ESP_LOGI(TAG, "[ 1.3 ] Init periph");
                esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
                esp_periph_start(set, wifi_handle);
                periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
                
                ESP_LOGI(TAG, "[ 2.0 ] Create audio pipeline for playback");
                audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
                pipeline_http = audio_pipeline_init(&pipeline_cfg);
                mem_assert(pipeline_http);

                ESP_LOGI(TAG, "[ 2.1 ] Create http stream to read data");
                http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
                http_stream_reader = http_stream_init(&http_cfg);

                ESP_LOGI(TAG, "[ 2.2 ] Create i2s stream to write data to codec chip");
                i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
                i2s_cfg.type = AUDIO_STREAM_WRITER;
                i2s_stream_writer = i2s_stream_init(&i2s_cfg);

                ESP_LOGI(TAG, "[ 2.3 ] Create mp3 decoder to decode mp3 file");
                mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
                mp3_decoder = mp3_decoder_init(&mp3_cfg);

                ESP_LOGI(TAG, "[ 3.0 ] Register all elements to audio pipeline");
                audio_pipeline_register(pipeline_http, http_stream_reader, "http");
                audio_pipeline_register(pipeline_http, mp3_decoder, "mp3");
                audio_pipeline_register(pipeline_http, i2s_stream_writer, "i2s");

                ESP_LOGI(TAG, "[ 3.1 ] Link it together http_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
                const char *link_tag[3] = {"http", "mp3", "i2s"};
                audio_pipeline_link(pipeline_http, &link_tag[0], 3);

                ESP_LOGI(TAG, "[ 3.2 ] Set up  uri (http as http_stream, mp3 as mp3 decoder, and default output is i2s)");
                audio_element_set_uri(http_stream_reader, radio_stations[radio_station_index]);
                ESP_LOGI(TAG, "Station: %s and index position is: %d", radio_stations[radio_station_index], radio_station_index);
                
                ESP_LOGI(TAG, "[ 4.0 ] Set up  event listener");
                audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
                audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

                ESP_LOGI(TAG, "[ 4.1 ] Listening event from all elements of pipeline");
                audio_pipeline_set_listener(pipeline_http, evt);

                ESP_LOGI(TAG, "[ 4.2 ] Listening event from peripherals");
                audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

                ESP_LOGI(TAG, "[ 4.3 ] Turn the SHUTDOWN HIGH");
                if (player_volume == 0) {
                    gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                } else {
                    gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);
                }

                ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
                audio_pipeline_run(pipeline_http);

                while (mode == WIFI_MODE) {
                    audio_event_iface_msg_t msg;
                    esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
                        continue;
                    }

                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                        && msg.source == (void *) mp3_decoder
                        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                        audio_element_info_t music_info = {0};
                        audio_element_getinfo(mp3_decoder, &music_info);

                        ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                                music_info.sample_rates, music_info.bits, music_info.channels);

                        audio_element_setinfo(i2s_stream_writer, &music_info);
                        i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
                        if (player_volume == 0) {
                            gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                        } else {
                            gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);
                        }
                        continue;
                    }

                    if ((msg.source_type == PERIPH_ID_TOUCH || msg.source_type == PERIPH_ID_BUTTON || msg.source_type == PERIPH_ID_ADC_BTN)
                        && (msg.cmd == PERIPH_TOUCH_TAP || msg.cmd == PERIPH_BUTTON_PRESSED || msg.cmd == PERIPH_ADC_BUTTON_PRESSED)) {

                        if ((int) msg.data == get_input_mode_id()) {
                            ESP_LOGI(TAG, "[ * ] [Mode] touch tap event");
                            gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                            mode = RESTART_MODE;
                        } else if ((int) msg.data == get_input_play_id()) {
                            ESP_LOGI(TAG, "[ * ] [Play] touch tap event");
                            audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
                            switch (el_state) {
                                case AEL_STATE_INIT :
                                    ESP_LOGI(TAG, "[ * ] Starting audio pipeline");
                                    if (player_volume == 0) {
                                        gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                                    } else {
                                        gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);
                                    }
                                    audio_pipeline_run(pipeline_http);
                                    break;
                                case AEL_STATE_RUNNING :
                                    ESP_LOGI(TAG, "[ * ] Pausing audio pipeline");
                                    gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                                    audio_pipeline_pause(pipeline_http);
                                    break;
                                case AEL_STATE_PAUSED :
                                    ESP_LOGI(TAG, "[ * ] Resuming audio pipeline");
                                    if (player_volume == 0) {
                                        gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                                    } else {
                                        gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);
                                    }
                                    audio_pipeline_resume(pipeline_http);
                                    break;
                                default :
                                    ESP_LOGI(TAG, "[ * ] Not supported state %d", el_state);
                            }
                        } else if ((int) msg.data == get_input_rec_id()) {
                            ESP_LOGI(TAG, "[ * ] [Rec] touch tap event");
                            gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                            audio_pipeline_stop(pipeline_http);
                            audio_pipeline_wait_for_stop(pipeline_http);
                            audio_pipeline_terminate(pipeline_http);
                            if (radio_station_index > 0) {
                                radio_station_index--;
                                audio_element_set_uri(http_stream_reader, radio_stations[radio_station_index]);
                                ESP_LOGI(TAG, "Station: %s and index position is: %d", radio_stations[radio_station_index], radio_station_index);
                            } else if (radio_station_index == 0) {
                                radio_station_index = radio_stations_count - 1;
                                audio_element_set_uri(http_stream_reader, radio_stations[radio_station_index]);
                                ESP_LOGI(TAG, "Station: %s and index position is: %d", radio_stations[radio_station_index], radio_station_index);
                            }
                            audio_pipeline_reset_ringbuffer(pipeline_http);
                            audio_pipeline_reset_elements(pipeline_http);
                            audio_pipeline_change_state(pipeline_http, AEL_STATE_INIT);
                            audio_pipeline_run(pipeline_http);
                        } else if ((int) msg.data == get_input_set_id()) {
                            ESP_LOGI(TAG, "[ * ] [Set] touch tap event");
                            gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                            audio_pipeline_stop(pipeline_http);
                            audio_pipeline_wait_for_stop(pipeline_http);
                            audio_pipeline_terminate(pipeline_http);
                            if (radio_station_index < radio_stations_count && radio_station_index != (radio_stations_count - 1)) {
                                radio_station_index++;
                                audio_element_set_uri(http_stream_reader, radio_stations[radio_station_index]);
                                ESP_LOGI(TAG, "Station: %s and index position is: %d", radio_stations[radio_station_index], radio_station_index);
                            } else {
                                radio_station_index = 0;
                                audio_element_set_uri(http_stream_reader, radio_stations[radio_station_index]);
                                ESP_LOGI(TAG, "Station: %s and index position is: %d", radio_stations[radio_station_index], radio_station_index);
                            }
                            audio_pipeline_reset_ringbuffer(pipeline_http);
                            audio_pipeline_reset_elements(pipeline_http);
                            audio_pipeline_change_state(pipeline_http, AEL_STATE_INIT);
                            audio_pipeline_run(pipeline_http);
                        } else if ((int) msg.data == get_input_volup_id()) {
                            ESP_LOGI(TAG, "[ * ] [Vol+] touch tap event");
                            audio_element_state_t el_state = audio_element_get_state(i2s_stream_writer);
                            if (el_state == AEL_STATE_PAUSED) {
                                gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                            } else {
                                gpio_set_level(SHUTDOWN_GPIO, HIGH_LVL);
                            }
                            if (player_volume == 0) {
                                player_volume = VOL_BOTTOM_TRH;
                            }
                            if (player_volume <= VOL_TRH) {
                                player_volume += VOL_INCREASE_LOW;
                            } else {
                                player_volume += VOL_INCREASE_UP;
                            }
                            if (player_volume >= VOL_UPPER_TRH) {
                                player_volume = VOL_UPPER_TRH;
                            }
                            audio_hal_set_volume(board_handle->audio_hal, player_volume);
                            ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
                        } else if ((int) msg.data == get_input_voldown_id()) {
                            ESP_LOGI(TAG, "[ * ] [Vol-] touch tap event");
                            if (player_volume <= VOL_TRH) {
                                player_volume -= VOL_DECREASE_LOW;
                            } else {
                                player_volume -= VOL_DECREASE_UP;
                            }
                            if (player_volume <= VOL_BOTTOM_TRH) {
                                gpio_set_level(SHUTDOWN_GPIO, LOW_LVL);
                                player_volume = 0;
                            }
                            audio_hal_set_volume(board_handle->audio_hal, player_volume);
                            ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
                        }
                    }

                    /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
                    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
                        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                        && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
                        ESP_LOGW(TAG, "[ * ] Stop event received");
                        break;
                    }
                }

                ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");
                audio_pipeline_stop(pipeline_http);
                audio_pipeline_wait_for_stop(pipeline_http);
                audio_pipeline_terminate(pipeline_http);

                /* Terminate the pipeline before removing the listener */
                audio_pipeline_unregister(pipeline_http, http_stream_reader);
                audio_pipeline_unregister(pipeline_http, i2s_stream_writer);
                audio_pipeline_unregister(pipeline_http, mp3_decoder);

                audio_pipeline_remove_listener(pipeline_http);

                /* Stop all peripherals before removing the listener */
                audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

                /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
                audio_event_iface_destroy(evt);

                /* Release all resources */
                audio_pipeline_deinit(pipeline_http);
                audio_element_deinit(http_stream_reader);
                audio_element_deinit(i2s_stream_writer);
                audio_element_deinit(mp3_decoder);
                if (mode != WIFI_MODE) {
                    esp_periph_set_stop_all(set);
                    esp_periph_set_destroy(set);
                    ESP_LOGW(TAG, "[ * ] Wi-Fi destroyed");
                }
                break;
            }
            case RESTART_MODE: {
                ESP_LOGI(TAG, "Restarting...");
                esp_restart();
                break;
            }
            default: {
                ESP_LOGW(TAG, "Not supported mode");
                esp_restart();
                break;
            }
        }
    }
}
