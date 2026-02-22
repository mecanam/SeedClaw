#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    char id[24];           // Discordメッセージ ID (snowflake文字列)
    char content[512];     // メッセージ本文 (512バイトに制限)
    char author_id[24];    // 著者のユーザーID
    bool author_is_bot;    // ボットかどうか
} discord_message_t;

/**
 * @brief Discordモジュールを初期化 (NVSから設定を読み込み)
 */
esp_err_t discord_init(void);

/**
 * @brief Discordから新しいメッセージをポーリング
 * @param out_msgs 出力バッファ
 * @param max_msgs 最大取得数
 * @return 取得したメッセージ数 (0〜max_msgs)、エラー時は -1
 */
int discord_poll(discord_message_t *out_msgs, int max_msgs);

/**
 * @brief Webhookでメッセージを送信
 * @param text 送信するテキスト
 */
esp_err_t discord_send_webhook(const char *text);

/**
 * @brief Bot TokenをNVSに保存
 */
esp_err_t discord_set_token(const char *token);

/**
 * @brief チャンネルIDをNVSに保存
 */
esp_err_t discord_set_channel(const char *channel_id);

/**
 * @brief Webhook URLをNVSに保存
 */
esp_err_t discord_set_webhook(const char *url);
