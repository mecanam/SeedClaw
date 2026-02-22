#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief WiFiモジュールを初期化
 */
esp_err_t wifi_init(void);

/**
 * @brief WiFiに接続（ブロッキング、最大30秒）
 */
esp_err_t wifi_connect(void);

/**
 * @brief WiFi接続状態を確認
 */
bool wifi_is_connected(void);

/**
 * @brief 取得したIPアドレスを返す
 */
const char *wifi_get_ip(void);

/**
 * @brief WiFi認証情報をNVSに保存
 */
esp_err_t wifi_set_credentials(const char *ssid, const char *pass);
