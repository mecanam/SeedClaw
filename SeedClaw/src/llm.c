#include "llm.h"
#include "seedclaw_config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "llm";

static char s_api_key[128] = SEEDCLAW_DEFAULT_API_KEY;
static char s_provider[16] = SEEDCLAW_LLM_DEFAULT_PROVIDER;
static char s_model[64] = SEEDCLAW_LLM_DEFAULT_MODEL;
static char s_system_prompt[1024] = SEEDCLAW_DEFAULT_SYSTEM_PROMPT;

typedef struct {
    char *buffer;
    size_t size;
    size_t len;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (resp->buffer != NULL) {
                size_t available = resp->size - resp->len - 1;
                size_t copy_len = (evt->data_len < available) ? evt->data_len : available;
                if (copy_len > 0) {
                    memcpy(resp->buffer + resp->len, evt->data, copy_len);
                    resp->len += copy_len;
                    resp->buffer[resp->len] = '\0';
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t llm_init(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SEEDCLAW_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t len;

        len = sizeof(s_api_key);
        nvs_get_str(nvs_handle, "api_key", s_api_key, &len);

        len = sizeof(s_provider);
        nvs_get_str(nvs_handle, "provider", s_provider, &len);

        len = sizeof(s_model);
        nvs_get_str(nvs_handle, "model", s_model, &len);

        len = sizeof(s_system_prompt);
        nvs_get_str(nvs_handle, "sys_prompt", s_system_prompt, &len);

        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "LLM initialized (provider: %s, model: %s)", s_provider, s_model);
    return ESP_OK;
}

static esp_err_t llm_chat_anthropic(const char *messages_json, const char *tools_json,
                                     char *out_buf, size_t out_buf_size,
                                     llm_response_type_t *out_type)
{
    // リクエストJSON作成
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", s_model);
    cJSON_AddNumberToObject(req, "max_tokens", SEEDCLAW_LLM_MAX_TOKENS);
    cJSON_AddBoolToObject(req, "stream", false);
    cJSON_AddStringToObject(req, "system", s_system_prompt);

    // messages
    cJSON *messages = cJSON_Parse(messages_json);
    if (messages == NULL) {
        cJSON_Delete(req);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_AddItemToObject(req, "messages", messages);

    // tools
    if (tools_json != NULL) {
        cJSON *tools = cJSON_Parse(tools_json);
        if (tools != NULL) {
            cJSON_AddItemToObject(req, "tools", tools);
        }
    }

    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    if (req_str == NULL) {
        ESP_LOGE(TAG, "Failed to create request JSON");
        return ESP_ERR_NO_MEM;
    }

    // HTTPリクエスト
    char *response_buffer = malloc(SEEDCLAW_LLM_RESP_BUF_SIZE);
    if (response_buffer == NULL) {
        free(req_str);
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        return ESP_ERR_NO_MEM;
    }

    http_response_t response = {
        .buffer = response_buffer,
        .size = SEEDCLAW_LLM_RESP_BUF_SIZE,
        .len = 0
    };

    esp_http_client_config_t config = {
        .url = SEEDCLAW_ANTHROPIC_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = SEEDCLAW_LLM_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "x-api-key", s_api_key);
    esp_http_client_set_header(client, "anthropic-version", SEEDCLAW_ANTHROPIC_VERSION);
    esp_http_client_set_header(client, "content-type", "application/json");
    esp_http_client_set_post_field(client, req_str, strlen(req_str));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    free(req_str);

    if (err == ESP_ERR_HTTP_FETCH_HEADER || err == ESP_ERR_HTTP_CONNECT) {
        ESP_LOGE(TAG, "HTTP connection failed: %s", esp_err_to_name(err));
        snprintf(out_buf, out_buf_size, "ネットワーク接続エラー。WiFi接続を確認してください。");
        free(response_buffer);
        *out_type = LLM_RESP_ERROR;
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        snprintf(out_buf, out_buf_size, "LLM API応答タイムアウト。しばらく待ってから再試行してください。");
        free(response_buffer);
        *out_type = LLM_RESP_ERROR;
        return err;
    }

    if (status_code == 429) {
        ESP_LOGW(TAG, "LLM API rate limited (429)");
        snprintf(out_buf, out_buf_size, "APIレート制限中です。10秒後に再試行してください。");
        free(response_buffer);
        *out_type = LLM_RESP_ERROR;
        vTaskDelay(pdMS_TO_TICKS(10000));
        return ESP_FAIL;
    }

    if (status_code != 200) {
        ESP_LOGE(TAG, "LLM API error: %d, body: %.500s", status_code, response_buffer);
        snprintf(out_buf, out_buf_size, "LLM API エラー (HTTP %d): %.200s", status_code, response_buffer);
        free(response_buffer);
        *out_type = LLM_RESP_ERROR;
        return ESP_FAIL;
    }

    // レスポンスパース
    cJSON *root = cJSON_Parse(response_buffer);
    free(response_buffer);

    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse LLM response");
        *out_type = LLM_RESP_ERROR;
        return ESP_FAIL;
    }

    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (content == NULL || !cJSON_IsArray(content)) {
        cJSON_Delete(root);
        *out_type = LLM_RESP_ERROR;
        return ESP_FAIL;
    }

    // stop_reasonを確認してツール呼び出しかどうか判定
    cJSON *stop_reason = cJSON_GetObjectItem(root, "stop_reason");
    bool is_tool_use = (stop_reason != NULL && cJSON_IsString(stop_reason) &&
                        strcmp(stop_reason->valuestring, "tool_use") == 0);

    // content配列から全tool_useブロックとtextブロックを収集
    cJSON *tool_use_array = cJSON_CreateArray();
    cJSON *text_block = NULL;
    int content_size = cJSON_GetArraySize(content);
    for (int ci = 0; ci < content_size; ci++) {
        cJSON *block = cJSON_GetArrayItem(content, ci);
        cJSON *btype = cJSON_GetObjectItem(block, "type");
        if (btype == NULL || !cJSON_IsString(btype)) continue;

        if (strcmp(btype->valuestring, "tool_use") == 0) {
            cJSON *id = cJSON_GetObjectItem(block, "id");
            cJSON *name = cJSON_GetObjectItem(block, "name");
            cJSON *input = cJSON_GetObjectItem(block, "input");
            if (id && name && input) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "tool_use_id", id->valuestring);
                cJSON_AddStringToObject(item, "name", name->valuestring);
                cJSON_AddItemToObject(item, "input", cJSON_Duplicate(input, true));
                cJSON_AddItemToArray(tool_use_array, item);
            }
        } else if (strcmp(btype->valuestring, "text") == 0) {
            text_block = block;
        }
    }

    int tool_count = cJSON_GetArraySize(tool_use_array);

    if (is_tool_use && tool_count > 0) {
        // ツール呼び出し（JSON配列で全tool_useを返す）
        char *result_str = cJSON_PrintUnformatted(tool_use_array);
        cJSON_Delete(tool_use_array);

        if (result_str != NULL) {
            strncpy(out_buf, result_str, out_buf_size - 1);
            out_buf[out_buf_size - 1] = '\0';
            free(result_str);
            *out_type = LLM_RESP_TOOL_USE;
        } else {
            *out_type = LLM_RESP_ERROR;
        }
    } else {
        cJSON_Delete(tool_use_array);
        if (text_block != NULL) {
            // テキスト応答
            cJSON *text = cJSON_GetObjectItem(text_block, "text");
            if (text != NULL && cJSON_IsString(text)) {
                strncpy(out_buf, text->valuestring, out_buf_size - 1);
                out_buf[out_buf_size - 1] = '\0';
                *out_type = LLM_RESP_TEXT;
            } else {
                *out_type = LLM_RESP_ERROR;
            }
        } else {
            *out_type = LLM_RESP_ERROR;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t llm_chat(const char *messages_json, const char *tools_json,
                   char *out_buf, size_t out_buf_size,
                   llm_response_type_t *out_type)
{
    if (strlen(s_api_key) == 0) {
        ESP_LOGE(TAG, "API key not configured");
        *out_type = LLM_RESP_ERROR;
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(s_provider, "anthropic") == 0) {
        return llm_chat_anthropic(messages_json, tools_json, out_buf, out_buf_size, out_type);
    } else {
        ESP_LOGE(TAG, "Unsupported provider: %s (Phase 1 supports anthropic only)", s_provider);
        *out_type = LLM_RESP_ERROR;
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t llm_set_api_key(const char *key)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SEEDCLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs_handle, "api_key", key);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
        strncpy(s_api_key, key, sizeof(s_api_key) - 1);
    }
    nvs_close(nvs_handle);
    return err;
}

esp_err_t llm_set_model(const char *model)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SEEDCLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs_handle, "model", model);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
        strncpy(s_model, model, sizeof(s_model) - 1);
    }
    nvs_close(nvs_handle);
    return err;
}

esp_err_t llm_set_provider(const char *provider)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SEEDCLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs_handle, "provider", provider);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
        strncpy(s_provider, provider, sizeof(s_provider) - 1);
    }
    nvs_close(nvs_handle);
    return err;
}

esp_err_t llm_set_system_prompt(const char *prompt)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SEEDCLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs_handle, "sys_prompt", prompt);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
        strncpy(s_system_prompt, prompt, sizeof(s_system_prompt) - 1);
    }
    nvs_close(nvs_handle);
    return err;
}
