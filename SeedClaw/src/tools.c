#include "tools.h"
#include "seedclaw_config.h"
#include "llm.h"
#include "gpio_ctrl.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tools";

// ツール定義JSON
static const char *TOOLS_JSON =
"["
  "{"
    "\"name\":\"gpio_read\","
    "\"description\":\"GPIOピンのデジタル値を読み取る。HIGH=1, LOW=0。ボタンやスイッチの状態確認に使う。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{"
        "\"pin\":{\"type\":\"integer\",\"description\":\"GPIOピン番号 (XIAO: D0=2,D1=3,D2=4,D3=5,D4=6,D5=7,D6=21,D7=20,D8=8,D10=10)\"}"
      "},"
      "\"required\":[\"pin\"]"
    "}"
  "},"
  "{"
    "\"name\":\"gpio_write\","
    "\"description\":\"GPIOピンにデジタル値を出力する。LED、リレーのON/OFF制御に使う。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{"
        "\"pin\":{\"type\":\"integer\",\"description\":\"GPIOピン番号\"},"
        "\"value\":{\"type\":\"integer\",\"enum\":[0,1],\"description\":\"0=LOW(OFF), 1=HIGH(ON)\"}"
      "},"
      "\"required\":[\"pin\",\"value\"]"
    "}"
  "},"
  "{"
    "\"name\":\"adc_read\","
    "\"description\":\"GPIOピンのアナログ値を読み取る (0-4095)。温度、光、距離などのアナログセンサーに使う。使用可能ピン: D0/A0=GPIO2, D1/A1=GPIO3, D2/A2=GPIO4。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{"
        "\"pin\":{\"type\":\"integer\",\"description\":\"ADC対応GPIOピン番号 (2,3,4のみ)\"}"
      "},"
      "\"required\":[\"pin\"]"
    "}"
  "},"
  "{"
    "\"name\":\"pwm_set\","
    "\"description\":\"GPIOピンにPWM信号を出力する。LEDの明るさ調整やモーターの速度制御に使う。duty=0で停止、duty=100で全開。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{"
        "\"pin\":{\"type\":\"integer\",\"description\":\"GPIOピン番号\"},"
        "\"duty\":{\"type\":\"integer\",\"description\":\"デューティ比 0-100(%)\"},"
        "\"freq\":{\"type\":\"integer\",\"description\":\"PWM周波数(Hz)。LED:1000, サーボ:50, モーター:25000。省略時1000Hz\"}"
      "},"
      "\"required\":[\"pin\",\"duty\"]"
    "}"
  "},"
  "{"
    "\"name\":\"gpio_status\","
    "\"description\":\"設定済み全GPIOピンの現在状態を取得する。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{},"
      "\"required\":[]"
    "}"
  "},"
  "{"
    "\"name\":\"web_fetch\","
    "\"description\":\"指定URLからWebページやAPIのデータを取得する。天気、ニュース、価格情報などの取得に使う。HTMLは先頭部分のみ取得される。JSONのAPIレスポンスが最も適している。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{"
        "\"url\":{\"type\":\"string\",\"description\":\"取得するURL (https://で始まるURL)\"},"
        "\"max_bytes\":{\"type\":\"integer\",\"description\":\"最大取得バイト数 (省略時4096, 最大8192)\"}"
      "},"
      "\"required\":[\"url\"]"
    "}"
  "},"
  "{"
    "\"name\":\"rule_add\","
    "\"description\":\"自律監視ルールを追加。定期的にこのルールに従いセンサー確認やGPIO制御を自動実行する。set_auto_intervalも同時に呼ぶこと。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{"
        "\"text\":{\"type\":\"string\",\"description\":\"監視ルールの内容\"}"
      "},"
      "\"required\":[\"text\"]"
    "}"
  "},"
  "{"
    "\"name\":\"rule_remove\","
    "\"description\":\"指定番号の自律監視ルールを削除する。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{"
        "\"index\":{\"type\":\"integer\",\"description\":\"削除するルール番号 (0始まり)\"}"
      "},"
      "\"required\":[\"index\"]"
    "}"
  "},"
  "{"
    "\"name\":\"rule_clear\","
    "\"description\":\"全ての自律監視ルールを削除し監視を停止する。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{},"
      "\"required\":[]"
    "}"
  "},"
  "{"
    "\"name\":\"set_auto_interval\","
    "\"description\":\"自律監視の実行間隔を設定。0で無効化。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{"
        "\"interval\":{\"type\":\"integer\",\"description\":\"チェック間隔(ポーリング回数。1回=約3秒。3=約9秒ごと)\"}"
      "},"
      "\"required\":[\"interval\"]"
    "}"
  "},"
  "{"
    "\"name\":\"get_rules\","
    "\"description\":\"現在の自律監視ルール一覧と設定を取得する。\","
    "\"input_schema\":{"
      "\"type\":\"object\","
      "\"properties\":{},"
      "\"required\":[]"
    "}"
  "}"
"]";

// 会話履歴
typedef struct {
    char role[16];         // "user", "assistant"
    char content[1024];    // メッセージ内容 or JSON
    char tool_use_id[32];
    char tool_name[20];
    bool is_tool_use;
    bool is_tool_result;
} history_entry_t;

static history_entry_t s_history[SEEDCLAW_MAX_HISTORY * 2 + 6];
static int s_history_count = 0;

// ── 監視ルール ──
static char s_rules[SEEDCLAW_MAX_RULES][SEEDCLAW_MAX_RULE_LEN];
static int s_rules_count = 0;
static int s_auto_interval = 0;  // 0 = 無効

int rules_count(void)
{
    return s_rules_count;
}

esp_err_t rules_add(const char *rule_text)
{
    if (s_rules_count >= SEEDCLAW_MAX_RULES) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(s_rules[s_rules_count], rule_text, SEEDCLAW_MAX_RULE_LEN - 1);
    s_rules[s_rules_count][SEEDCLAW_MAX_RULE_LEN - 1] = '\0';
    s_rules_count++;
    return ESP_OK;
}

esp_err_t rules_remove(int index)
{
    if (index < 0 || index >= s_rules_count) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = index; i < s_rules_count - 1; i++) {
        memcpy(s_rules[i], s_rules[i + 1], SEEDCLAW_MAX_RULE_LEN);
    }
    s_rules_count--;
    if (s_rules_count == 0) {
        s_auto_interval = 0;
    }
    return ESP_OK;
}

void rules_list(void)
{
    if (s_rules_count == 0) {
        printf("No monitoring rules defined.\n");
        return;
    }
    for (int i = 0; i < s_rules_count; i++) {
        printf("  [%d] %s\n", i, s_rules[i]);
    }
}

void rules_clear(void)
{
    s_rules_count = 0;
    s_auto_interval = 0;
    memset(s_rules, 0, sizeof(s_rules));
}

int auto_interval_get(void)
{
    return s_auto_interval;
}

void auto_interval_set(int interval)
{
    s_auto_interval = interval;
}

void tools_init(void)
{
    s_history_count = 0;
    s_rules_count = 0;
    s_auto_interval = 0;
    ESP_LOGI(TAG, "Tools initialized");
}

// 履歴先頭がtool_result/tool_useで始まっていたら除去（ペア整合性を保つ）
static void trim_history_safe(int need_slots)
{
    int max_entries = SEEDCLAW_MAX_HISTORY * 2 + 6;
    while (s_history_count + need_slots > max_entries && s_history_count > 0) {
        // 2エントリずつ削除
        int trim = (s_history_count >= 2) ? 2 : 1;
        memmove(&s_history[0], &s_history[trim], sizeof(history_entry_t) * (s_history_count - trim));
        s_history_count -= trim;
    }
    // 先頭がtool_result or tool_useなら孤立エントリを除去
    while (s_history_count > 0 &&
           (s_history[0].is_tool_result || s_history[0].is_tool_use)) {
        memmove(&s_history[0], &s_history[1], sizeof(history_entry_t) * (s_history_count - 1));
        s_history_count--;
    }
}

static void add_user_message(const char *content)
{
    trim_history_safe(1);

    strcpy(s_history[s_history_count].role, "user");
    strncpy(s_history[s_history_count].content, content, sizeof(s_history[s_history_count].content) - 1);
    s_history[s_history_count].is_tool_use = false;
    s_history[s_history_count].is_tool_result = false;
    s_history_count++;
}

static void add_assistant_text(const char *text)
{
    trim_history_safe(1);

    strcpy(s_history[s_history_count].role, "assistant");
    strncpy(s_history[s_history_count].content, text, sizeof(s_history[s_history_count].content) - 1);
    s_history[s_history_count].is_tool_use = false;
    s_history[s_history_count].is_tool_result = false;
    s_history_count++;
}

static char *build_messages_json(void)
{
    cJSON *messages = cJSON_CreateArray();
    int i = 0;

    while (i < s_history_count) {
        if (s_history[i].is_tool_use) {
            // 連続するassistant tool_useを1メッセージにマージ
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", "assistant");
            cJSON *content_array = cJSON_CreateArray();

            while (i < s_history_count && s_history[i].is_tool_use) {
                cJSON *tool_use = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_use, "type", "tool_use");
                cJSON_AddStringToObject(tool_use, "id", s_history[i].tool_use_id);
                cJSON_AddStringToObject(tool_use, "name", s_history[i].tool_name);
                cJSON *input = cJSON_Parse(s_history[i].content);
                if (input == NULL) input = cJSON_CreateObject();
                cJSON_AddItemToObject(tool_use, "input", input);
                cJSON_AddItemToArray(content_array, tool_use);
                i++;
            }

            cJSON_AddItemToObject(msg, "content", content_array);
            cJSON_AddItemToArray(messages, msg);
        } else if (s_history[i].is_tool_result) {
            // 連続するuser tool_resultを1メッセージにマージ
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", "user");
            cJSON *content_array = cJSON_CreateArray();

            while (i < s_history_count && s_history[i].is_tool_result) {
                cJSON *tool_result = cJSON_CreateObject();
                cJSON_AddStringToObject(tool_result, "type", "tool_result");
                cJSON_AddStringToObject(tool_result, "tool_use_id", s_history[i].tool_use_id);
                cJSON_AddStringToObject(tool_result, "content", s_history[i].content);
                cJSON_AddItemToArray(content_array, tool_result);
                i++;
            }

            cJSON_AddItemToObject(msg, "content", content_array);
            cJSON_AddItemToArray(messages, msg);
        } else {
            // 通常のテキスト
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", s_history[i].role);
            cJSON_AddStringToObject(msg, "content", s_history[i].content);
            cJSON_AddItemToArray(messages, msg);
            i++;
        }
    }

    char *json_str = cJSON_PrintUnformatted(messages);
    cJSON_Delete(messages);
    return json_str;
}

// 不正なUTF-8バイトを '?' に置換（Claude APIがstrict UTF-8を要求するため）
static void sanitize_utf8(char *buf)
{
    unsigned char *p = (unsigned char *)buf;
    while (*p) {
        if (*p < 0x80) {
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) == 0x80) { p += 2; }
            else { *p = '?'; p++; }
        } else if ((*p & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                // サロゲートペア (U+D800-U+DFFF) を除外
                unsigned int cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                if (cp >= 0xD800 && cp <= 0xDFFF) {
                    p[0] = '?'; p[1] = '?'; p[2] = '?'; p += 3;
                } else {
                    p += 3;
                }
            } else { *p = '?'; p++; }
        } else if ((*p & 0xF8) == 0xF0) {
            if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) { p += 4; }
            else { *p = '?'; p++; }
        } else {
            *p = '?'; p++;
        }
    }
}

static char *execute_tool(const char *name, const char *input_json)
{
    cJSON *input = cJSON_Parse(input_json);
    if (input == NULL) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid tool input JSON");
        char *error_str = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        return error_str;
    }

    cJSON *result = cJSON_CreateObject();

    if (strcmp(name, "gpio_read") == 0) {
        cJSON *pin_obj = cJSON_GetObjectItem(input, "pin");
        if (pin_obj == NULL || !cJSON_IsNumber(pin_obj)) {
            cJSON_AddStringToObject(result, "error", "Missing or invalid 'pin' parameter");
        } else {
            int pin = pin_obj->valueint;
            int value = gpio_ctrl_read(pin);
            if (value < 0) {
                char error_msg[100];
                snprintf(error_msg, sizeof(error_msg), "Failed to read GPIO%d (not allowed or error)", pin);
                cJSON_AddStringToObject(result, "error", error_msg);
            } else {
                cJSON_AddNumberToObject(result, "pin", pin);
                cJSON_AddNumberToObject(result, "value", value);
                cJSON_AddStringToObject(result, "state", value ? "HIGH" : "LOW");
            }
        }
    } else if (strcmp(name, "gpio_write") == 0) {
        cJSON *pin_obj = cJSON_GetObjectItem(input, "pin");
        cJSON *value_obj = cJSON_GetObjectItem(input, "value");
        if (pin_obj == NULL || value_obj == NULL || !cJSON_IsNumber(pin_obj) || !cJSON_IsNumber(value_obj)) {
            cJSON_AddStringToObject(result, "error", "Missing or invalid 'pin' or 'value' parameter");
        } else {
            int pin = pin_obj->valueint;
            int value = value_obj->valueint;
            esp_err_t err = gpio_ctrl_write(pin, value);
            if (err == ESP_OK) {
                cJSON_AddNumberToObject(result, "pin", pin);
                cJSON_AddNumberToObject(result, "value", value);
                cJSON_AddStringToObject(result, "state", value ? "HIGH" : "LOW");
                cJSON_AddBoolToObject(result, "ok", true);
            } else {
                char error_msg[100];
                snprintf(error_msg, sizeof(error_msg), "Failed to write GPIO%d", pin);
                cJSON_AddStringToObject(result, "error", error_msg);
            }
        }
    } else if (strcmp(name, "adc_read") == 0) {
        cJSON *pin_obj = cJSON_GetObjectItem(input, "pin");
        if (pin_obj == NULL || !cJSON_IsNumber(pin_obj)) {
            cJSON_AddStringToObject(result, "error", "Missing or invalid 'pin' parameter");
        } else {
            int pin = pin_obj->valueint;
            adc_result_t adc_result;
            esp_err_t err = gpio_ctrl_adc_read(pin, &adc_result);
            if (err == ESP_OK) {
                cJSON_AddNumberToObject(result, "pin", pin);
                cJSON_AddNumberToObject(result, "raw", adc_result.raw);
                cJSON_AddNumberToObject(result, "voltage_mv", adc_result.voltage_mv);
                cJSON_AddNumberToObject(result, "percentage", adc_result.percentage);
            } else {
                char error_msg[100];
                snprintf(error_msg, sizeof(error_msg), "Failed to read ADC on GPIO%d (not an ADC pin or error)", pin);
                cJSON_AddStringToObject(result, "error", error_msg);
            }
        }
    } else if (strcmp(name, "pwm_set") == 0) {
        cJSON *pin_obj = cJSON_GetObjectItem(input, "pin");
        cJSON *duty_obj = cJSON_GetObjectItem(input, "duty");
        cJSON *freq_obj = cJSON_GetObjectItem(input, "freq");
        if (pin_obj == NULL || duty_obj == NULL || !cJSON_IsNumber(pin_obj) || !cJSON_IsNumber(duty_obj)) {
            cJSON_AddStringToObject(result, "error", "Missing or invalid 'pin' or 'duty' parameter");
        } else {
            int pin = pin_obj->valueint;
            int duty = duty_obj->valueint;
            int freq = (freq_obj != NULL && cJSON_IsNumber(freq_obj)) ? freq_obj->valueint : 1000;
            esp_err_t err = gpio_ctrl_pwm_set(pin, duty, freq);
            if (err == ESP_OK) {
                cJSON_AddNumberToObject(result, "pin", pin);
                cJSON_AddNumberToObject(result, "duty", duty);
                cJSON_AddNumberToObject(result, "freq", freq);
                cJSON_AddBoolToObject(result, "ok", true);
            } else {
                char error_msg[100];
                snprintf(error_msg, sizeof(error_msg), "Failed to set PWM on GPIO%d", pin);
                cJSON_AddStringToObject(result, "error", error_msg);
            }
        }
    } else if (strcmp(name, "gpio_status") == 0) {
        char *status = gpio_ctrl_status_json();
        cJSON_Delete(result);
        cJSON_Delete(input);
        return status;
    } else if (strcmp(name, "web_fetch") == 0) {
        cJSON *url_obj = cJSON_GetObjectItem(input, "url");
        cJSON *max_bytes_obj = cJSON_GetObjectItem(input, "max_bytes");
        if (url_obj == NULL || !cJSON_IsString(url_obj)) {
            cJSON_AddStringToObject(result, "error", "Missing or invalid 'url' parameter");
        } else {
            const char *url = url_obj->valuestring;
            int max_bytes = 4096;
            if (max_bytes_obj != NULL && cJSON_IsNumber(max_bytes_obj)) {
                max_bytes = max_bytes_obj->valueint;
                if (max_bytes > 8192) max_bytes = 8192;
                if (max_bytes < 256) max_bytes = 256;
            }

            // HTTPレスポンスバッファ
            char *resp_buf = calloc(1, max_bytes + 1);
            if (resp_buf == NULL) {
                cJSON_AddStringToObject(result, "error", "Memory allocation failed");
            } else {
                typedef struct { char *buffer; size_t size; size_t len; } web_resp_t;
                web_resp_t web_resp = { .buffer = resp_buf, .size = max_bytes, .len = 0 };

                esp_http_client_config_t http_cfg = {
                    .url = url,
                    .method = HTTP_METHOD_GET,
                    .timeout_ms = 10000,
                    .crt_bundle_attach = esp_crt_bundle_attach,
                    .event_handler = NULL,
                    .user_data = &web_resp,
                };

                // イベントハンドラ用のインラインデータ受信
                esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
                esp_err_t http_err = esp_http_client_open(client, 0);
                if (http_err == ESP_OK) {
                    int content_length = esp_http_client_fetch_headers(client);
                    int status_code = esp_http_client_get_status_code(client);
                    int read_len = esp_http_client_read(client, resp_buf, max_bytes);
                    if (read_len >= 0) {
                        resp_buf[read_len] = '\0';
                    }
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);

                    cJSON_AddStringToObject(result, "url", url);
                    cJSON_AddNumberToObject(result, "status_code", status_code);
                    cJSON_AddNumberToObject(result, "bytes_read", read_len > 0 ? read_len : 0);
                    if (content_length > 0) {
                        cJSON_AddNumberToObject(result, "content_length", content_length);
                    }
                    if (read_len > 0) {
                        sanitize_utf8(resp_buf);
                        cJSON_AddStringToObject(result, "body", resp_buf);
                    } else {
                        cJSON_AddStringToObject(result, "body", "(empty)");
                    }
                } else {
                    esp_http_client_cleanup(client);
                    char err_msg[128];
                    snprintf(err_msg, sizeof(err_msg), "HTTP request failed: %s", esp_err_to_name(http_err));
                    cJSON_AddStringToObject(result, "error", err_msg);
                }
                free(resp_buf);
            }
        }
    } else if (strcmp(name, "rule_add") == 0) {
        cJSON *text_obj = cJSON_GetObjectItem(input, "text");
        if (text_obj == NULL || !cJSON_IsString(text_obj)) {
            cJSON_AddStringToObject(result, "error", "Missing 'text' parameter");
        } else {
            esp_err_t err = rules_add(text_obj->valuestring);
            if (err == ESP_OK) {
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "total_rules", s_rules_count);
                cJSON_AddNumberToObject(result, "auto_interval", s_auto_interval);
            } else {
                char msg[80];
                snprintf(msg, sizeof(msg), "Max rules reached (%d)", SEEDCLAW_MAX_RULES);
                cJSON_AddStringToObject(result, "error", msg);
            }
        }
    } else if (strcmp(name, "rule_remove") == 0) {
        cJSON *idx_obj = cJSON_GetObjectItem(input, "index");
        if (idx_obj == NULL || !cJSON_IsNumber(idx_obj)) {
            cJSON_AddStringToObject(result, "error", "Missing 'index' parameter");
        } else {
            esp_err_t err = rules_remove(idx_obj->valueint);
            if (err == ESP_OK) {
                cJSON_AddBoolToObject(result, "ok", true);
                cJSON_AddNumberToObject(result, "remaining_rules", s_rules_count);
            } else {
                cJSON_AddStringToObject(result, "error", "Invalid rule index");
            }
        }
    } else if (strcmp(name, "rule_clear") == 0) {
        rules_clear();
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddStringToObject(result, "message", "All rules cleared, monitoring stopped");
    } else if (strcmp(name, "set_auto_interval") == 0) {
        cJSON *intv_obj = cJSON_GetObjectItem(input, "interval");
        if (intv_obj == NULL || !cJSON_IsNumber(intv_obj)) {
            cJSON_AddStringToObject(result, "error", "Missing 'interval' parameter");
        } else {
            int interval = intv_obj->valueint;
            if (interval < 0) interval = 0;
            auto_interval_set(interval);
            cJSON_AddBoolToObject(result, "ok", true);
            cJSON_AddNumberToObject(result, "interval", interval);
            if (interval > 0) {
                cJSON_AddNumberToObject(result, "check_every_seconds", interval * 3);
            }
        }
    } else if (strcmp(name, "get_rules") == 0) {
        cJSON *rules_arr = cJSON_CreateArray();
        for (int ri = 0; ri < s_rules_count; ri++) {
            cJSON_AddItemToArray(rules_arr, cJSON_CreateString(s_rules[ri]));
        }
        cJSON_AddItemToObject(result, "rules", rules_arr);
        cJSON_AddNumberToObject(result, "total_rules", s_rules_count);
        cJSON_AddNumberToObject(result, "auto_interval", s_auto_interval);
    } else {
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "Unknown tool: %s", name);
        cJSON_AddStringToObject(result, "error", error_msg);
    }

    cJSON_Delete(input);
    char *result_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return result_str;
}

char *react_loop(const char *user_message)
{
    add_user_message(user_message);

    char *llm_out_buf = malloc(SEEDCLAW_LLM_RESP_BUF_SIZE);
    if (llm_out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LLM output buffer");
        return strdup("エラー: メモリ不足");
    }

    for (int i = 0; i < SEEDCLAW_MAX_TOOL_CALLS; i++) {
        size_t free_heap = esp_get_free_heap_size();
        if (free_heap < 40000) {
            ESP_LOGW(TAG, "Low heap: %u bytes, clearing history", (unsigned)free_heap);
            s_history_count = 0;
            add_user_message(user_message);
        }

        char *messages_json = build_messages_json();
        if (messages_json == NULL) {
            free(llm_out_buf);
            return strdup("エラー: メッセージJSON作成失敗");
        }
        sanitize_utf8(messages_json);

        llm_response_type_t resp_type;
        esp_err_t err = llm_chat(messages_json, TOOLS_JSON, llm_out_buf, SEEDCLAW_LLM_RESP_BUF_SIZE, &resp_type);
        free(messages_json);

        if (err != ESP_OK || resp_type == LLM_RESP_ERROR) {
            // llm_out_bufにエラー詳細が入っている可能性がある
            char *error_reply;
            if (strlen(llm_out_buf) > 0) {
                error_reply = strdup(llm_out_buf);
            } else {
                error_reply = strdup("エラー: LLM API呼び出し失敗");
            }
            free(llm_out_buf);
            return error_reply;
        }

        if (resp_type == LLM_RESP_TEXT) {
            // 最終応答
            add_assistant_text(llm_out_buf);
            char *reply = strdup(llm_out_buf);
            free(llm_out_buf);
            return reply;
        } else if (resp_type == LLM_RESP_TOOL_USE) {
            // ツール呼び出し（JSON配列で複数tool_useに対応）
            cJSON *tool_calls = cJSON_Parse(llm_out_buf);
            if (tool_calls == NULL) {
                free(llm_out_buf);
                return strdup("エラー: ツール呼び出しJSON解析失敗");
            }

            int num_calls = cJSON_IsArray(tool_calls) ? cJSON_GetArraySize(tool_calls) : 0;
            if (num_calls == 0) {
                cJSON_Delete(tool_calls);
                free(llm_out_buf);
                return strdup("エラー: ツール呼び出し情報不足");
            }
            if (num_calls > SEEDCLAW_MAX_TOOL_CALLS) num_calls = SEEDCLAW_MAX_TOOL_CALLS;

            // 履歴スペース確保
            trim_history_safe(num_calls * 2);

            // Phase 1: 全ツール実行 + assistant tool_useエントリ追加
            char *results[SEEDCLAW_MAX_TOOL_CALLS];
            char *ids[SEEDCLAW_MAX_TOOL_CALLS];
            int valid_count = 0;
            memset(results, 0, sizeof(results));
            memset(ids, 0, sizeof(ids));

            for (int tc = 0; tc < num_calls; tc++) {
                cJSON *call = cJSON_GetArrayItem(tool_calls, tc);
                cJSON *id_j = cJSON_GetObjectItem(call, "tool_use_id");
                cJSON *name_j = cJSON_GetObjectItem(call, "name");
                cJSON *input_j = cJSON_GetObjectItem(call, "input");
                if (!id_j || !name_j || !input_j) continue;

                char *input_str = cJSON_PrintUnformatted(input_j);
                results[valid_count] = execute_tool(name_j->valuestring, input_str);
                ids[valid_count] = strdup(id_j->valuestring);

                // assistant tool_useエントリ
                strcpy(s_history[s_history_count].role, "assistant");
                strncpy(s_history[s_history_count].tool_use_id, id_j->valuestring,
                        sizeof(s_history[0].tool_use_id) - 1);
                strncpy(s_history[s_history_count].tool_name, name_j->valuestring,
                        sizeof(s_history[0].tool_name) - 1);
                strncpy(s_history[s_history_count].content, input_str,
                        sizeof(s_history[0].content) - 1);
                s_history[s_history_count].is_tool_use = true;
                s_history[s_history_count].is_tool_result = false;
                s_history_count++;

                free(input_str);
                valid_count++;
            }

            // Phase 2: user tool_resultエントリ追加
            for (int tc = 0; tc < valid_count; tc++) {
                strcpy(s_history[s_history_count].role, "user");
                strncpy(s_history[s_history_count].tool_use_id, ids[tc],
                        sizeof(s_history[0].tool_use_id) - 1);
                strncpy(s_history[s_history_count].content, results[tc],
                        sizeof(s_history[0].content) - 1);
                s_history[s_history_count].is_tool_use = false;
                s_history[s_history_count].is_tool_result = true;
                s_history_count++;

                free(ids[tc]);
                free(results[tc]);
            }

            cJSON_Delete(tool_calls);
            i += (valid_count - 1); // 複数ツール分のカウント調整
        }
    }

    free(llm_out_buf);
    return strdup("操作が複雑すぎます。もう少し簡単にお願いします。");
}

char *autonomous_check(void)
{
    if (s_rules_count == 0) {
        return NULL;
    }

    ESP_LOGI(TAG, "Running autonomous check (%d rules)", s_rules_count);

    // ルールからプロンプトを構築
    char prompt[1536];
    int offset = snprintf(prompt, sizeof(prompt),
        "自律監視モード。以下のルールに従いツールでセンサーを確認し、"
        "アクション実行や異常検知時のみ簡潔に報告。変化なければ「変化なし」とだけ答えて。\n\n"
        "ルール:\n");

    for (int i = 0; i < s_rules_count && offset < (int)sizeof(prompt) - 260; i++) {
        offset += snprintf(prompt + offset, sizeof(prompt) - offset,
                           "%d. %s\n", i + 1, s_rules[i]);
    }

    // 会話履歴を一時退避
    int saved_history_count = s_history_count;
    s_history_count = 0;

    char *result = react_loop(prompt);

    // 会話履歴を復元
    s_history_count = saved_history_count;

    if (result == NULL) {
        return NULL;
    }

    // 「変化なし」なら報告不要
    if (strstr(result, "変化なし") != NULL) {
        free(result);
        return NULL;
    }

    // プレフィックス付加
    size_t result_len = strlen(result);
    char *report = malloc(strlen("**[自律監視]** ") + result_len + 1);
    if (report == NULL) {
        return result;
    }
    sprintf(report, "**[自律監視]** %s", result);
    free(result);
    return report;
}
