#include "seedclaw_config.h"
#include "wifi.h"
#include "discord.h"
#include "llm.h"
#include "gpio_ctrl.h"
#include "tools.h"
#include "cli.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "seedclaw";

static void main_loop(void)
{
    ESP_LOGI(TAG, "Starting main loop");
    int auto_counter = 0;
    int backoff_ms = 0;

    while (1) {
        // STEP 1: Discordポーリング
        discord_message_t msgs[SEEDCLAW_MAX_POLL_MSGS];
        int msg_count = discord_poll(msgs, SEEDCLAW_MAX_POLL_MSGS);

        // STEP 2: 各メッセージを処理
        if (msg_count > 0) {
            backoff_ms = 0;
            for (int i = 0; i < msg_count; i++) {
                ESP_LOGI(TAG, "Processing message: %s", msgs[i].content);

                char *reply = react_loop(msgs[i].content);
                if (reply != NULL) {
                    discord_send_webhook(reply);
                    free(reply);
                }
            }
        } else if (msg_count == 0) {
            backoff_ms = 0;
        } else {
            // ネットワークエラー — 指数バックオフ
            if (backoff_ms == 0) {
                backoff_ms = 3000;
            } else {
                backoff_ms *= 2;
                if (backoff_ms > 60000) backoff_ms = 60000;
            }
            ESP_LOGW(TAG, "Discord polling failed, backoff %dms", backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            continue;
        }

        // STEP 3: 自律チェック
        int interval = auto_interval_get();
        if (interval > 0 && rules_count() > 0) {
            auto_counter++;
            if (auto_counter >= interval) {
                char *report = autonomous_check();
                if (report != NULL) {
                    discord_send_webhook(report);
                    free(report);
                }
                auto_counter = 0;
            }
        }

        // 次のポーリングまで待機
        vTaskDelay(pdMS_TO_TICKS(SEEDCLAW_POLL_INTERVAL_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "SeedClaw starting...");

    // NVS初期化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // イベントループ作成
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // WiFi初期化＆接続
    ESP_LOGI(TAG, "Initializing WiFi...");
    ESP_ERROR_CHECK(wifi_init());
    ret = wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed. Please configure WiFi via CLI and restart.");
    } else {
        ESP_LOGI(TAG, "WiFi connected: %s", wifi_get_ip());
    }

    // GPIO制御初期化
    ESP_LOGI(TAG, "Initializing GPIO control...");
    ESP_ERROR_CHECK(gpio_ctrl_init());

    // Discord初期化
    ESP_LOGI(TAG, "Initializing Discord...");
    ESP_ERROR_CHECK(discord_init());

    // LLM初期化
    ESP_LOGI(TAG, "Initializing LLM...");
    ESP_ERROR_CHECK(llm_init());

    // ツールモジュール初期化
    ESP_LOGI(TAG, "Initializing tools...");
    tools_init();

    // CLI起動
    ESP_LOGI(TAG, "Starting CLI...");
    ESP_ERROR_CHECK(cli_init());

    // メインループ開始
    ESP_LOGI(TAG, "=== SeedClaw initialized successfully ===");

    // 起動通知をDiscordに送信（TLS/DNS安定のため少し待機）
    if (wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_err_t send_err = discord_send_webhook("🌱 **SeedClaw 起動完了！** GPIO制御の準備ができました。メッセージを送ってください。");
        if (send_err == ESP_OK) {
            ESP_LOGI(TAG, "Startup notification sent to Discord");
        } else {
            ESP_LOGE(TAG, "Failed to send startup notification: %s", esp_err_to_name(send_err));
        }
    }

    ESP_LOGI(TAG, "Discord polling will start now");

    main_loop();
}
