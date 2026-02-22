#pragma once

#include "esp_err.h"

/**
 * @brief ツールモジュールを初期化
 */
void tools_init(void);

/**
 * @brief ReActループを実行
 * @param user_message ユーザーメッセージ
 * @return Discord に送信するテキスト（呼び出し元が free()）。NULLならエラー。
 */
char *react_loop(const char *user_message);

/**
 * @brief 自律チェックを実行
 * @return Discord に報告するテキスト（NULL なら報告不要）
 */
char *autonomous_check(void);

/* ── 監視ルール管理 ── */

int rules_count(void);
esp_err_t rules_add(const char *rule_text);
esp_err_t rules_remove(int index);
void rules_list(void);
void rules_clear(void);

int auto_interval_get(void);
void auto_interval_set(int interval);
