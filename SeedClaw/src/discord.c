#include "discord.h"
#include "seedclaw_config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "discord";

static char s_bot_token[128] = SEEDCLAW_DEFAULT_BOT_TOKEN;
static char s_channel_id[24] = SEEDCLAW_DEFAULT_CHANNEL_ID;
static char s_webhook_url[192] = SEEDCLAW_DEFAULT_WEBHOOK_URL;
static char s_last_msg_id[24] = "0";

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

esp_err_t discord_init(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SEEDCLAW_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t len;

        len = sizeof(s_bot_token);
        nvs_get_str(nvs_handle, "bot_token", s_bot_token, &len);

        len = sizeof(s_channel_id);
        nvs_get_str(nvs_handle, "channel_id", s_channel_id, &len);

        len = sizeof(s_webhook_url);
        nvs_get_str(nvs_handle, "webhook_url", s_webhook_url, &len);

        len = sizeof(s_last_msg_id);
        nvs_get_str(nvs_handle, "last_msg_id", s_last_msg_id, &len);

        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Discord initialized (channel: %s)", s_channel_id);
    return ESP_OK;
}

int discord_poll(discord_message_t *out_msgs, int max_msgs)
{
    if (strlen(s_bot_token) == 0 || strlen(s_channel_id) == 0) {
        return 0;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://discord.com/api/v10/channels/%s/messages?after=%s&limit=%d",
             s_channel_id, s_last_msg_id, max_msgs);

    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Bot %s", s_bot_token);

    char *response_buffer = malloc(SEEDCLAW_HTTP_BUF_SIZE);
    if (response_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        return -1;
    }

    http_response_t response = {
        .buffer = response_buffer,
        .size = SEEDCLAW_HTTP_BUF_SIZE,
        .len = 0
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = SEEDCLAW_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(response_buffer);
        return -1;
    }

    if (status_code == 401) {
        ESP_LOGE(TAG, "Invalid bot token (401 Unauthorized)");
        free(response_buffer);
        return 0;
    } else if (status_code == 403) {
        ESP_LOGE(TAG, "Missing permissions (403 Forbidden)");
        free(response_buffer);
        return 0;
    } else if (status_code == 429) {
        // retry_afterをパースして待機
        float retry_after = 5.0f;
        cJSON *err_root = cJSON_Parse(response_buffer);
        if (err_root != NULL) {
            cJSON *ra = cJSON_GetObjectItem(err_root, "retry_after");
            if (ra != NULL && cJSON_IsNumber(ra)) {
                retry_after = (float)ra->valuedouble;
            }
            cJSON_Delete(err_root);
        }
        int wait_ms = (int)(retry_after * 1000);
        if (wait_ms < 1000) wait_ms = 1000;
        if (wait_ms > 60000) wait_ms = 60000;
        ESP_LOGW(TAG, "Rate limited (429), waiting %dms", wait_ms);
        free(response_buffer);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        return 0;
    } else if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP error: %d", status_code);
        free(response_buffer);
        return -1;
    }

    // JSONパース
    cJSON *root = cJSON_Parse(response_buffer);
    free(response_buffer);

    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return -1;
    }

    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Response is not an array");
        cJSON_Delete(root);
        return -1;
    }

    int msg_count = 0;
    int array_size = cJSON_GetArraySize(root);

    // 配列は新→古の順なので、逆順で処理（古い方から）
    for (int i = array_size - 1; i >= 0 && msg_count < max_msgs; i--) {
        cJSON *msg = cJSON_GetArrayItem(root, i);
        if (msg == NULL) continue;

        cJSON *id = cJSON_GetObjectItem(msg, "id");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        cJSON *author = cJSON_GetObjectItem(msg, "author");

        if (id == NULL || content == NULL || author == NULL) {
            continue;
        }

        cJSON *author_id = cJSON_GetObjectItem(author, "id");
        cJSON *is_bot = cJSON_GetObjectItem(author, "bot");

        // ボットのメッセージはスキップ
        if (is_bot != NULL && cJSON_IsTrue(is_bot)) {
            // 最新のメッセージIDは更新
            strncpy(s_last_msg_id, id->valuestring, sizeof(s_last_msg_id) - 1);
            continue;
        }

        // 空のコンテンツはスキップ
        if (!cJSON_IsString(content) || strlen(content->valuestring) == 0) {
            strncpy(s_last_msg_id, id->valuestring, sizeof(s_last_msg_id) - 1);
            continue;
        }

        // メッセージを格納
        strncpy(out_msgs[msg_count].id, id->valuestring, sizeof(out_msgs[msg_count].id) - 1);
        strncpy(out_msgs[msg_count].content, content->valuestring, sizeof(out_msgs[msg_count].content) - 1);
        if (author_id != NULL && cJSON_IsString(author_id)) {
            strncpy(out_msgs[msg_count].author_id, author_id->valuestring, sizeof(out_msgs[msg_count].author_id) - 1);
        }
        out_msgs[msg_count].author_is_bot = false;

        strncpy(s_last_msg_id, id->valuestring, sizeof(s_last_msg_id) - 1);
        msg_count++;
    }

    cJSON_Delete(root);

    // last_msg_id をNVSに保存
    if (msg_count > 0) {
        nvs_handle_t nvs_handle;
        if (nvs_open(SEEDCLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_set_str(nvs_handle, "last_msg_id", s_last_msg_id);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }
    }

    ESP_LOGI(TAG, "Polled %d new messages", msg_count);
    return msg_count;
}

esp_err_t discord_send_webhook(const char *text)
{
    if (strlen(s_webhook_url) == 0) {
        ESP_LOGE(TAG, "Webhook URL not configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (text == NULL || strlen(text) == 0) {
        return ESP_OK;
    }

    // 2000文字ごとに分割
    size_t text_len = strlen(text);
    size_t offset = 0;

    while (offset < text_len) {
        size_t chunk_len = text_len - offset;
        if (chunk_len > SEEDCLAW_DISCORD_MAX_MSG_LEN) {
            chunk_len = SEEDCLAW_DISCORD_MAX_MSG_LEN;
        }

        char *chunk = strndup(text + offset, chunk_len);
        if (chunk == NULL) {
            ESP_LOGE(TAG, "Failed to allocate chunk");
            return ESP_ERR_NO_MEM;
        }

        // JSONボディ作成
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "content", chunk);
        char *json_str = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        free(chunk);

        if (json_str == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON");
            return ESP_ERR_NO_MEM;
        }

        // HTTP POST (429リトライ付き)
        int retry_count = 0;
        esp_err_t send_err = ESP_FAIL;
        int send_status = 0;

        while (retry_count < 3) {
            esp_http_client_config_t config = {
                .url = s_webhook_url,
                .method = HTTP_METHOD_POST,
                .timeout_ms = SEEDCLAW_HTTP_TIMEOUT_MS,
                .crt_bundle_attach = esp_crt_bundle_attach,
            };

            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, json_str, strlen(json_str));

            send_err = esp_http_client_perform(client);
            send_status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);

            if (send_err != ESP_OK) {
                ESP_LOGE(TAG, "Webhook request failed: %s", esp_err_to_name(send_err));
                break;
            }

            if (send_status == 429) {
                retry_count++;
                int wait_ms = retry_count * 2000;
                ESP_LOGW(TAG, "Webhook rate limited (429), retry %d/3 after %dms",
                         retry_count, wait_ms);
                vTaskDelay(pdMS_TO_TICKS(wait_ms));
                continue;
            }
            break;
        }

        free(json_str);

        if (send_err != ESP_OK) {
            return send_err;
        }
        if (send_status < 200 || send_status >= 300) {
            ESP_LOGE(TAG, "Webhook HTTP error: %d", send_status);
            return ESP_FAIL;
        }

        offset += chunk_len;
    }

    ESP_LOGI(TAG, "Message sent via webhook");
    return ESP_OK;
}

esp_err_t discord_set_token(const char *token)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SEEDCLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs_handle, "bot_token", token);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
        strncpy(s_bot_token, token, sizeof(s_bot_token) - 1);
    }
    nvs_close(nvs_handle);
    return err;
}

esp_err_t discord_set_channel(const char *channel_id)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SEEDCLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs_handle, "channel_id", channel_id);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
        strncpy(s_channel_id, channel_id, sizeof(s_channel_id) - 1);
    }
    nvs_close(nvs_handle);
    return err;
}

esp_err_t discord_set_webhook(const char *url)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(SEEDCLAW_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs_handle, "webhook_url", url);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
        strncpy(s_webhook_url, url, sizeof(s_webhook_url) - 1);
    }
    nvs_close(nvs_handle);
    return err;
}
