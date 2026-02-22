#pragma once

#include "esp_err.h"

typedef enum {
    LLM_RESP_TEXT,       // テキスト応答（最終的な返答）
    LLM_RESP_TOOL_USE,   // ツール呼び出し要求
    LLM_RESP_ERROR,      // エラー
} llm_response_type_t;

/**
 * @brief LLMモジュールを初期化 (NVSから設定を読み込み)
 */
esp_err_t llm_init(void);

/**
 * @brief LLM APIを呼び出す
 * @param messages_json JSON配列文字列: [{"role":"user","content":"..."},...]
 * @param tools_json ツール定義JSON配列文字列（NULLならツールなし）
 * @param out_buf 出力バッファ（テキスト応答 or tool_use JSON）
 * @param out_buf_size 出力バッファサイズ
 * @param out_type 応答タイプ
 */
esp_err_t llm_chat(
    const char *messages_json,
    const char *tools_json,
    char *out_buf,
    size_t out_buf_size,
    llm_response_type_t *out_type
);

/**
 * @brief LLM API KeyをNVSに保存
 */
esp_err_t llm_set_api_key(const char *key);

/**
 * @brief LLMモデル名をNVSに保存
 */
esp_err_t llm_set_model(const char *model);

/**
 * @brief LLMプロバイダーをNVSに保存
 */
esp_err_t llm_set_provider(const char *provider);

/**
 * @brief システムプロンプトをNVSに保存
 */
esp_err_t llm_set_system_prompt(const char *prompt);
