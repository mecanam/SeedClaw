# NanoClaw 完全実装仕様書

## このドキュメントについて

NanoClawはSeeed Studio XIAO ESP32C3上で動作する超軽量Discord AIボットである。
LLM（Claude / GPT）を通じて自然言語でGPIOを制御し、
AIによる自律的なセンサー監視・アクチュエータ制御を実現する。

本ドキュメントは、NanoClawの実装に必要な全ての仕様を網羅している。
開発環境はESP-IDF v5.5.x、言語はCを使用する。

---

## 目次

1. [ハードウェア仕様](#1-ハードウェア仕様)
2. [アーキテクチャ概要](#2-アーキテクチャ概要)
3. [ファイル構成](#3-ファイル構成)
4. [メインループ詳細設計](#4-メインループ詳細設計)
5. [WiFi接続 (wifi.c)](#5-wifi接続)
6. [Discord通信 (discord.c)](#6-discord通信)
7. [LLM API呼び出し (llm.c)](#7-llm-api呼び出し)
8. [GPIO制御 (gpio_ctrl.c)](#8-gpio制御)
9. [ReActツールループ](#9-reactツールループ)
10. [自律監視モード](#10-自律監視モード)
11. [シリアルCLI (cli.c)](#11-シリアルcli)
12. [NVS設定管理](#12-nvs設定管理)
13. [メモリバジェット](#13-メモリバジェット)
14. [エラーハンドリング](#14-エラーハンドリング)
15. [ビルド設定](#15-ビルド設定)
16. [定数一覧](#16-定数一覧)
17. [ユーザーセットアップ手順](#17-ユーザーセットアップ手順)

---

## 1. ハードウェア仕様

### 1.1 ターゲットボード

**Seeed Studio XIAO ESP32C3**

| 項目 | スペック |
|------|---------|
| SoC | ESP32-C3 (RISC-V シングルコア 160MHz) |
| SRAM | 400 KB（PSRAMなし） |
| Flash | 4 MB |
| WiFi | 802.11 b/g/n |
| BLE | Bluetooth 5 (LE)（本プロジェクトでは不使用） |
| USB | USB-C (USB Serial/JTAG) |
| GPIO | 11本（外部ピン） |
| ADC | 12bit, ADC1のみ安定動作（GPIO2, 3, 4） |
| PWM | LEDC、最大6チャンネル |
| 動作電圧 | 3.3V ロジック |
| 電源 | USB-C 5V 入力 |

### 1.2 XIAO ESP32C3 ピンマップ

以下は物理ピンラベル（D0〜D10）とESP32-C3内部GPIO番号の対応、
およびNanoClawでの推奨用途をまとめたものである。

```
                  XIAO ESP32C3 (上面図)
                 ┌──────────────────┐
          D0/A0 ─┤ GPIO2        5V  ├─ 5V
          D1/A1 ─┤ GPIO3       GND  ├─ GND
          D2/A2 ─┤ GPIO4      3V3   ├─ 3.3V
            D3  ─┤ GPIO5      D10   ├─ GPIO10
            D4  ─┤ GPIO6       D9   ├─ GPIO9 (BOOT)
            D5  ─┤ GPIO7       D8   ├─ GPIO8
            D6  ─┤ GPIO21      D7   ├─ GPIO20
                 └──────────────────┘
                      USB-C
```

| ラベル | GPIO | ADC | PWM | NanoClaw推奨用途 | 安全度 | 注記 |
|--------|------|-----|-----|-----------------|--------|------|
| D0/A0 | 2 | ADC1_CH0 | ○ | アナログ入力 | ⚠️ | Strapping Pin |
| D1/A1 | 3 | ADC1_CH1 | ○ | アナログ入力 | ✅ | 安全 |
| D2/A2 | 4 | ADC1_CH2 | ○ | アナログ入力 | ✅ | 安全 |
| D3 | 5 | - | ○ | **デジタル出力 (LED/リレー)** | ✅ | 最も安全 |
| D4 | 6 | - | ○ | デジタル出力 / I2C SCL | ✅ | 安全 |
| D5 | 7 | - | ○ | デジタル入出力 / I2C SDA | ✅ | 安全 |
| D6 | 21 | - | ○ | デジタル出力のみ推奨 | ⚠️ | 起動時UART出力あり |
| D7 | 20 | - | ○ | デジタル入力推奨 | ⚠️ | UART RX兼用 |
| D8 | 8 | - | ○ | デジタル入出力 | ⚠️ | Strapping、起動時HIGH必要 |
| D9 | 9 | - | ○ | 入力 (BOOTボタン接続) | ❌ | BOOTボタン共用、使用非推奨 |
| D10 | 10 | - | ○ | **デジタル出力 (LED/リレー)** | ✅ | 安全 |

### 1.3 使用可能ピン定義（ファームウェアでのガード）

```c
// AIが操作を許可されるピン（ビットマスク）
// GPIO: 2,3,4,5,6,7,8,10,20,21
// GPIO9はBOOTボタン、GPIO11-17はFlash、GPIO18-19はUSBのため除外
#define NANOCLAW_GPIO_ALLOWED_MASK  ( \
    (1ULL << 2)  | (1ULL << 3)  | (1ULL << 4)  | (1ULL << 5)  | \
    (1ULL << 6)  | (1ULL << 7)  | (1ULL << 8)  | (1ULL << 10) | \
    (1ULL << 20) | (1ULL << 21) )

// ADCとして使用可能なピン（GPIO 2, 3, 4 のみ）
#define NANOCLAW_ADC_ALLOWED_MASK   ( \
    (1ULL << 2) | (1ULL << 3) | (1ULL << 4) )
```

---

## 2. アーキテクチャ概要

### 2.1 設計思想

NanoClawは**シングルコア・シングルTLS接続・シーケンシャル処理**を前提とする。

- TLS接続は**同時に1本のみ**。1つのHTTPリクエストを完了し、接続を閉じてから次を開始する。
  これによりTLSバッファのメモリ消費を約45KBに抑える。
- FreeRTOSのタスクは**メインループ1つ + CLIタスク1つ**のみ。
- WebSocket Gateway は**使用しない**。Discord通信は全てHTTPS REST API。
- メッセージ送信にはDiscord Webhookを使用する（Bot REST APIの「事前Gateway接続必須」要件を回避）。

### 2.2 処理フロー概要

```
app_main()
  ├── NVS初期化
  ├── WiFi接続（ブロッキング、リトライ付き）
  ├── GPIO初期化
  ├── CLIタスク起動
  └── main_loop() ← 永久ループ
        │
        ├── [STEP 1] discord_poll()
        │   GET /channels/{id}/messages?after={last_id}&limit=5
        │   → 新メッセージの配列を取得
        │   → ボット自身のメッセージはスキップ
        │   → last_message_id をNVSに保存
        │
        ├── [STEP 2] 新メッセージあり？
        │   ├─ YES → 各メッセージに対してreact_loop()実行
        │   │         → LLMにメッセージ＋ツール定義を送信
        │   │         → tool_use応答 → GPIOツール実行 → 結果をLLMに返送
        │   │         → 最大3回のツール呼び出しループ
        │   │         → 最終テキスト応答 → discord_send_webhook()
        │   └─ NO  → スキップ
        │
        ├── [STEP 3] auto_check_counter++
        │   カウンター >= NANOCLAW_AUTO_CHECK_INTERVAL ?
        │   ├─ YES かつ 監視ルールあり
        │   │   → autonomous_check()
        │   │   → AIに自律プロンプト＋ツールを送信
        │   │   → AIがセンサー読み取り→判断→GPIO操作
        │   │   → 結果に変化あり → discord_send_webhook()で報告
        │   └─ NO → スキップ
        │
        └── vTaskDelay(NANOCLAW_POLL_INTERVAL_MS)
            → STEP 1 に戻る
```

---

## 3. ファイル構成

```
nanoclaw/
├── CMakeLists.txt              # トップレベル（ESP-IDFプロジェクト定義）
├── partitions.csv              # パーティションテーブル（NVS拡張）
├── sdkconfig.defaults          # ビルド時のデフォルト設定
├── main/
│   ├── CMakeLists.txt          # ソースファイル登録
│   ├── idf_component.yml       # 外部コンポーネント依存なし
│   ├── nanoclaw.c              # app_main(), main_loop()
│   ├── nanoclaw_config.h       # 全定数・設定マクロ
│   ├── wifi.c                  # WiFi接続
│   ├── wifi.h
│   ├── discord.c               # Discord REST polling + Webhook送信
│   ├── discord.h
│   ├── llm.c                   # LLM API呼び出し（Anthropic / OpenAI）
│   ├── llm.h
│   ├── gpio_ctrl.c             # GPIO制御（デジタル/ADC/PWM）
│   ├── gpio_ctrl.h
│   ├── tools.c                 # ReActツールループ + 自律チェック
│   ├── tools.h
│   └── cli.c                   # シリアルCLI
│       cli.h
```

### 3.1 ソースファイル登録 (main/CMakeLists.txt)

```cmake
idf_component_register(
    SRCS
        "nanoclaw.c"
        "wifi.c"
        "discord.c"
        "llm.c"
        "gpio_ctrl.c"
        "tools.c"
        "cli.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client
        esp_event json esp-tls driver esp_timer esp_adc console
)
```

### 3.2 トップレベル CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nanoclaw)
```

### 3.3 idf_component.yml

```yaml
dependencies:
  idf:
    version: ">=5.5.0,<5.6.0"
```

外部コンポーネント（esp_websocket_client等）は不要。ESP-IDF標準コンポーネントのみ使用。

---

## 4. メインループ詳細設計

### 4.1 nanoclaw.c

```
#include 構成:
  nanoclaw_config.h, wifi.h, discord.h, llm.h, gpio_ctrl.h, tools.h, cli.h
  esp_log.h, nvs_flash.h, freertos/FreeRTOS.h, freertos/task.h

app_main():
  1. nvs_flash_init() (NVS_NO_FREE_PAGES時はeraseしてretry)
  2. esp_event_loop_create_default()
  3. wifi_init() → wifi_connect() (最大30秒ブロッキング待機)
  4. gpio_ctrl_init()
  5. cli_init() → CLI用FreeRTOSタスクを起動
  6. main_loop() を直接呼び出し（タスク化不要、app_mainのスタックで実行）

main_loop():
  static int auto_counter = 0;

  while (1) {
      // STEP 1: Discordポーリング
      discord_message_t msgs[NANOCLAW_MAX_POLL_MSGS];
      int msg_count = discord_poll(msgs, NANOCLAW_MAX_POLL_MSGS);

      // STEP 2: 各メッセージを処理
      for (int i = 0; i < msg_count; i++) {
          char *reply = react_loop(msgs[i].content);
          if (reply) {
              discord_send_webhook(reply);
              free(reply);
          }
      }

      // STEP 3: 自律チェック
      auto_counter++;
      if (auto_counter >= nanoclaw_auto_interval && rules_count() > 0) {
          char *report = autonomous_check();
          if (report) {
              discord_send_webhook(report);
              free(report);
          }
          auto_counter = 0;
      }

      vTaskDelay(pdMS_TO_TICKS(NANOCLAW_POLL_INTERVAL_MS));
  }
```

### 4.2 メッセージ構造体

```c
// discord.h
typedef struct {
    char id[24];           // Discordメッセージ ID (snowflake文字列)
    char content[512];     // メッセージ本文 (512バイトに制限して読み取り)
    char author_id[24];    // 著者のユーザーID
    bool author_is_bot;    // ボットかどうか
} discord_message_t;
```

---

## 5. WiFi接続

### 5.1 wifi.c の仕様

WiFiはstation modeで接続する。
SSID/パスワードはNVSから読み込み、ビルド時のデフォルト値をフォールバックとする。

```
wifi_init():
  1. esp_netif_init()
  2. esp_netif_create_default_wifi_sta()
  3. esp_wifi_init() (WIFI_INIT_CONFIG_DEFAULT)
  4. イベントハンドラ登録 (WIFI_EVENT / IP_EVENT)
  5. NVSからSSID/PASSを読み込み（なければビルド時デフォルト）
  6. wifi_config_t に設定

wifi_connect():
  1. esp_wifi_start()
  2. EventGroupでIP取得を最大30秒待機
  3. 失敗時はESP_LOGE出力、リターン（CLIで設定変更可能）

wifi_reconnect():
  切断イベント時に自動再接続（指数バックオフ: 1s, 2s, 4s, ... 最大60s）
```

### 5.2 公開API

```c
// wifi.h
esp_err_t wifi_init(void);
esp_err_t wifi_connect(void);   // ブロッキング、最大30秒
bool wifi_is_connected(void);
const char *wifi_get_ip(void);
esp_err_t wifi_set_credentials(const char *ssid, const char *pass); // NVS保存
```

---

## 6. Discord通信

### 6.1 設計方針

- **メッセージ読み取り**: Bot Token + REST API (`GET /channels/{id}/messages`)
- **メッセージ送信**: Webhook URL (`POST /api/webhooks/{id}/{token}`)
- Webhookを使う理由: Discord Bot APIの `POST /channels/{id}/messages` は
  事前にGateway(WebSocket)へ1回接続する必要があるが、Webhookは不要。
  WebSocket接続のメモリオーバーヘッド（~50KB）を完全に回避できる。
- TLS接続は1リクエストごとに開いて閉じる（同時接続を避ける）。

### 6.2 discord.c の仕様

#### discord_init()

```
NVSから以下を読み込み:
  - bot_token (必須: メッセージ読み取りに使用)
  - channel_id (必須: 監視対象チャンネル)
  - webhook_url (必須: メッセージ送信に使用)
  - last_msg_id (起動時の既読位置)
```

#### discord_poll()

```
関数シグネチャ:
  int discord_poll(discord_message_t *out_msgs, int max_msgs);
  戻り値: 取得したメッセージ数 (0〜max_msgs)

処理:
  1. HTTPリクエスト構築:
     GET https://discord.com/api/v10/channels/{channel_id}/messages?after={last_msg_id}&limit=5
     ヘッダー:
       Authorization: Bot {bot_token}

  2. esp_http_client でリクエスト実行
     - timeout_ms: 10000
     - buffer_size: 2048
     - crt_bundle_attach: esp_crt_bundle_attach

  3. レスポンス（JSON配列）をcJSONでパース:
     [
       {
         "id": "...",
         "content": "...",
         "author": { "id": "...", "bot": false }
       },
       ...
     ]
     注意: 配列は新しいメッセージが先頭（降順）。処理は逆順（古い方から）で行う。

  4. 各メッセージについて:
     - author.bot == true → スキップ（自分自身や他のボットを無視）
     - content が空文字列 → スキップ（embed/attachment-onlyメッセージ）
     - discord_message_t に格納

  5. 最も新しいメッセージのIDで last_msg_id を更新
     → NVSに保存（毎回ではなく、変更時のみ）

  6. esp_http_client_cleanup() で接続を閉じる

エラー処理:
  - HTTP 401: "Bot token invalid" をログ出力
  - HTTP 403: "Missing permissions" をログ出力
  - HTTP 429: レスポンスJSONの retry_after を読み、その秒数だけ待機
  - ネットワークエラー: -1 を返す（呼び出し元でバックオフ）
```

#### discord_send_webhook()

```
関数シグネチャ:
  esp_err_t discord_send_webhook(const char *text);

処理:
  1. テキストが2000文字を超える場合、2000文字ごとに分割して複数回送信

  2. 各チャンクについて:
     POST {webhook_url}
     Content-Type: application/json
     ボディ: {"content": "テキスト"}

     注意: Webhookは Authorization ヘッダー不要（URLにトークンが含まれる）

  3. esp_http_client_cleanup() で接続を閉じる

  4. HTTP 429 の場合は retry_after に従って待機してリトライ

HTTPバッファ:
  レスポンスバッファは小さくてよい（Webhookレスポンスは短い）。
  受信バッファ 1024バイトで十分。
```

### 6.3 公開API

```c
// discord.h
typedef struct {
    char id[24];
    char content[512];
    char author_id[24];
    bool author_is_bot;
} discord_message_t;

esp_err_t discord_init(void);
int  discord_poll(discord_message_t *out_msgs, int max_msgs);
esp_err_t discord_send_webhook(const char *text);
esp_err_t discord_set_token(const char *token);      // NVS保存
esp_err_t discord_set_channel(const char *channel_id); // NVS保存
esp_err_t discord_set_webhook(const char *url);       // NVS保存
```

---

## 7. LLM API呼び出し

### 7.1 設計方針

- Anthropic Claude API と OpenAI API の両方に対応する。
- プロバイダーの切替はNVS設定 `provider` で行う（"anthropic" or "openai"）。
- ストリーミングモード（SSE）で受信し、メモリを節約する。
- レスポンス全体をバッファに溜めるのではなく、テキストチャンクを逐次結合する。
- ツール呼び出し（tool_use）にも対応する（ReActパターンに必要）。

### 7.2 llm.c の仕様

#### llm_init()

```
NVSから以下を読み込み:
  - api_key (必須)
  - provider ("anthropic" or "openai", デフォルト: "anthropic")
  - model (デフォルト: "claude-sonnet-4-20250514")
  - system_prompt (デフォルト: 後述の定数)
```

#### llm_chat()

```
関数シグネチャ:
  esp_err_t llm_chat(
      const char *messages_json,   // JSON配列文字列: [{"role":"user","content":"..."},...]
      const char *tools_json,      // ツール定義JSON配列文字列（NULLならツールなし）
      char *out_buf,               // 出力バッファ（テキスト応答 or tool_use JSON）
      size_t out_buf_size,         // 出力バッファサイズ
      llm_response_type_t *out_type // 応答タイプ: LLM_RESP_TEXT or LLM_RESP_TOOL_USE
  );

  typedef enum {
      LLM_RESP_TEXT,       // テキスト応答（最終的な返答）
      LLM_RESP_TOOL_USE,   // ツール呼び出し要求
      LLM_RESP_ERROR,      // エラー
  } llm_response_type_t;
```

#### Anthropic Claude API

```
リクエスト:
  POST https://api.anthropic.com/v1/messages
  ヘッダー:
    x-api-key: {api_key}
    anthropic-version: 2023-06-01
    content-type: application/json

  ボディ:
  {
    "model": "{model}",
    "max_tokens": 1024,
    "stream": true,
    "system": "{system_prompt}",
    "messages": {messages_json},
    "tools": {tools_json}       ← tools_jsonがNULLならこのフィールドは省略
  }

ストリーミング応答のパース (SSE形式):
  各行は "event: xxx" または "data: {json}" の形式。

  (A) テキスト応答の場合:
    event: content_block_delta
    data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"こん"}}
    → delta.text を out_buf に追記

    event: message_stop
    → 終了、out_type = LLM_RESP_TEXT

  (B) ツール呼び出しの場合:
    event: content_block_start
    data: {"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_xxx","name":"gpio_write","input":{}}}
    → ツール名を記録

    event: content_block_delta
    data: {"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"pin\":"}}
    → partial_json を結合してツール入力JSONを構築

    event: content_block_stop
    → 終了。out_buf にJSON形式で格納:
      {"tool_use_id":"toolu_xxx","name":"gpio_write","input":{"pin":5,"value":1}}
      out_type = LLM_RESP_TOOL_USE

SSEパースの実装方針:
  esp_http_client の on_data イベントで受信チャンクを処理。
  改行区切りで行をバッファリングし、"data: " で始まる行からJSONを抽出。
  一時バッファは 1024 バイトで十分（各SSEチャンクは小さい）。
```

#### OpenAI API

```
リクエスト:
  POST https://api.openai.com/v1/chat/completions
  ヘッダー:
    Authorization: Bearer {api_key}
    content-type: application/json

  ボディ:
  {
    "model": "{model}",
    "max_tokens": 1024,
    "stream": true,
    "messages": [
      {"role": "system", "content": "{system_prompt}"},
      ...{messages_json の中身}
    ],
    "tools": [                    ← tools_jsonがNULLならこのフィールドは省略
      {
        "type": "function",
        "function": {tools_json の各ツール}
      }
    ]
  }

  OpenAI形式ではツール定義が Anthropic と異なるラッピングが必要。
    Anthropic: {"name":"gpio_write","description":"...","input_schema":{...}}
    OpenAI:    {"type":"function","function":{"name":"gpio_write","description":"...","parameters":{...}}}

ストリーミング応答のパース (SSE形式):
  data: {"choices":[{"delta":{"content":"こん"}}]}
  → delta.content を out_buf に追記

  data: {"choices":[{"delta":{"tool_calls":[{"function":{"name":"gpio_write","arguments":"{\"pin\":"}}]}}]}
  → ツール名とargumentsを結合

  data: [DONE]
  → 終了
```

#### 非ストリーミング（Phase 1 簡易実装）

Phase 1 ではストリーミングを省略し、非ストリーミングで実装しても良い。

```
"stream": false にして、レスポンスを一括で受信:

Anthropic レスポンス:
{
  "content": [
    {"type": "text", "text": "応答テキスト"},
    {"type": "tool_use", "id": "toolu_xxx", "name": "gpio_write", "input": {"pin": 5, "value": 1}}
  ],
  "stop_reason": "end_turn" or "tool_use"
}

OpenAI レスポンス:
{
  "choices": [{
    "message": {
      "content": "応答テキスト",
      "tool_calls": [{"id":"call_xxx","function":{"name":"gpio_write","arguments":"{\"pin\":5,\"value\":1}"}}]
    },
    "finish_reason": "stop" or "tool_calls"
  }]
}

レスポンスバッファ: 8192 バイト
```

### 7.3 公開API

```c
// llm.h
typedef enum {
    LLM_RESP_TEXT,
    LLM_RESP_TOOL_USE,
    LLM_RESP_ERROR,
} llm_response_type_t;

esp_err_t llm_init(void);
esp_err_t llm_chat(
    const char *messages_json,
    const char *tools_json,
    char *out_buf,
    size_t out_buf_size,
    llm_response_type_t *out_type
);
esp_err_t llm_set_api_key(const char *key);
esp_err_t llm_set_model(const char *model);
esp_err_t llm_set_provider(const char *provider);
esp_err_t llm_set_system_prompt(const char *prompt);
```

---

## 8. GPIO制御

### 8.1 gpio_ctrl.c の仕様

GPIO制御はツールからの呼び出しとCLIからの直接呼び出しの両方に対応する。

#### 内部データ構造

```c
typedef enum {
    PIN_MODE_UNUSED,
    PIN_MODE_INPUT,
    PIN_MODE_OUTPUT,
    PIN_MODE_ADC,
    PIN_MODE_PWM,
} pin_mode_t;

typedef struct {
    pin_mode_t mode;
    int value;           // デジタル値 or ADC raw値
    int pwm_duty;        // 0-100 (PWM時のみ)
    int pwm_freq;        // Hz (PWM時のみ)
    int pwm_channel;     // LEDCチャンネル番号 (-1なら未割当)
} pin_state_t;

// 最大GPIO番号 + 1 = 22
static pin_state_t s_pin_state[22];
static int s_next_pwm_channel = 0;  // 次に割り当てるLEDCチャンネル (最大6)
```

#### gpio_ctrl_init()

全ピンを PIN_MODE_UNUSED に初期化。LEDCタイマーを1つ設定。

#### gpio_ctrl_read()

```
int gpio_ctrl_read(int pin);

処理:
  1. is_pin_allowed(pin) でチェック → 失敗なら -1
  2. ピンがまだ INPUT 設定でなければ gpio_set_direction(pin, GPIO_MODE_INPUT)
  3. s_pin_state[pin].mode = PIN_MODE_INPUT
  4. gpio_get_level(pin) で値を取得
  5. s_pin_state[pin].value = 取得値
  6. 値を返す
```

#### gpio_ctrl_write()

```
esp_err_t gpio_ctrl_write(int pin, int value);

処理:
  1. is_pin_allowed(pin) でチェック
  2. value != 0 && value != 1 → エラー
  3. ピンがPWMモードなら、まずPWMを停止
  4. gpio_set_direction(pin, GPIO_MODE_OUTPUT)
  5. gpio_set_level(pin, value)
  6. s_pin_state[pin] = { .mode = PIN_MODE_OUTPUT, .value = value }
```

#### gpio_ctrl_adc_read()

```
typedef struct {
    int raw;          // 0-4095
    int voltage_mv;   // ミリボルト
    int percentage;   // 0-100%
} adc_result_t;

esp_err_t gpio_ctrl_adc_read(int pin, adc_result_t *result);

処理:
  1. is_adc_allowed(pin) でチェック（GPIO 2,3,4 のみ）
  2. ADC oneshot で読み取り:
     adc_oneshot_unit_handle_t を使用
     adc_oneshot_config_channel() → adc_oneshot_read()
  3. adc_cali_raw_to_voltage() で電圧変換
  4. result に格納
  5. s_pin_state[pin] = { .mode = PIN_MODE_ADC, .value = raw }

ESP-IDF v5.x の新しいADC API (adc_oneshot) を使用すること。
旧APIの adc1_get_raw() は非推奨。

チャンネルマッピング:
  GPIO2 → ADC1_CHANNEL_0
  GPIO3 → ADC1_CHANNEL_1
  GPIO4 → ADC1_CHANNEL_2
```

#### gpio_ctrl_pwm_set()

```
esp_err_t gpio_ctrl_pwm_set(int pin, int duty_percent, int freq_hz);

処理:
  1. is_pin_allowed(pin) でチェック
  2. duty_percent を 0-100 にクランプ
  3. freq_hz が 0 以下ならデフォルト 1000Hz
  4. pin に LEDCチャンネルがまだ割り当てられていなければ:
     - s_next_pwm_channel >= 6 → エラー（最大チャンネル超過）
     - ledc_channel_config() で新しいチャンネルを割当
  5. ledc_set_freq() (必要なら)
  6. ledc_set_duty() → ledc_update_duty()
  7. s_pin_state[pin] = { .mode = PIN_MODE_PWM, .pwm_duty = duty, .pwm_freq = freq }

LEDCタイマー設定:
  - タイマー: LEDC_TIMER_0
  - 速度モード: LEDC_LOW_SPEED_MODE (ESP32-C3はlow-speedのみ)
  - duty分解能: LEDC_TIMER_10_BIT (0-1023)
  - duty計算: duty_raw = (duty_percent * 1023) / 100
```

#### gpio_ctrl_status()

```
const char *gpio_ctrl_status_json(void);

処理:
  cJSON配列を構築:
  {
    "pins": [
      {"pin": 5, "label": "D3", "mode": "OUTPUT", "value": 1},
      {"pin": 3, "label": "D1/A1", "mode": "ADC", "raw": 2048, "voltage_mv": 1650},
      ...
    ]
  }
  PIN_MODE_UNUSED のピンは省略。
  戻り値は cJSON_PrintUnformatted() の結果（呼び出し元がfreeする）。
```

### 8.2 公開API

```c
// gpio_ctrl.h
typedef struct {
    int raw;
    int voltage_mv;
    int percentage;
} adc_result_t;

esp_err_t gpio_ctrl_init(void);
int  gpio_ctrl_read(int pin);
esp_err_t gpio_ctrl_write(int pin, int value);
esp_err_t gpio_ctrl_adc_read(int pin, adc_result_t *result);
esp_err_t gpio_ctrl_pwm_set(int pin, int duty_percent, int freq_hz);
char *gpio_ctrl_status_json(void);   // 呼び出し元がfree()する
bool gpio_is_pin_allowed(int pin);
bool gpio_is_adc_allowed(int pin);
```

---

## 9. ReActツールループ

### 9.1 tools.c の仕様

LLMにツールを渡し、tool_use応答が返ってきたらツールを実行し、
結果をLLMに返すReActループを実装する。

#### ツール定義（LLMに渡すJSON）

以下のツール定義JSONを**コンパイル時に静的文字列**として保持する。

```c
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
  "}"
"]";
```

#### react_loop()

```
関数シグネチャ:
  char *react_loop(const char *user_message);
  戻り値: Discord に送信するテキスト（呼び出し元が free()）。NULLならエラー。

処理:
  1. 会話履歴に user_message を追加
  2. ツール呼び出しカウンター = 0

  3. ループ（最大 NANOCLAW_MAX_TOOL_CALLS = 3 回）:
     a. messages_json を構築（会話履歴全体）
     b. llm_chat(messages_json, TOOLS_JSON, out_buf, sizeof(out_buf), &resp_type)

     c. resp_type == LLM_RESP_TEXT の場合:
        → 会話履歴に assistant メッセージを追加
        → out_buf の内容を strdup() して返す

     d. resp_type == LLM_RESP_TOOL_USE の場合:
        → out_buf をパース: {"tool_use_id":"...","name":"...","input":{...}}
        → execute_tool(name, input) を呼び出し（後述）
        → ツール結果のJSON文字列を取得
        → 会話履歴に assistant の tool_use と user の tool_result を追加
        → ツール呼び出しカウンター++
        → ループ先頭に戻る

     e. resp_type == LLM_RESP_ERROR の場合:
        → "エラーが発生しました" を返す

  4. ツール呼び出し回数上限到達:
     → "操作が複雑すぎます。もう少し簡単にお願いします。" を返す
```

#### execute_tool()

```
char *execute_tool(const char *name, const char *input_json);
戻り値: ツール結果JSON文字列（呼び出し元がfree()）

処理:
  name に応じて分岐:

  "gpio_read":
    pin = input_json.pin
    value = gpio_ctrl_read(pin)
    → {"pin": 5, "value": 1, "state": "HIGH"}

  "gpio_write":
    pin = input_json.pin, value = input_json.value
    err = gpio_ctrl_write(pin, value)
    → {"pin": 5, "value": 1, "state": "HIGH", "ok": true}
    → エラー時: {"error": "Pin 9 is not allowed (BOOT button)"}

  "adc_read":
    pin = input_json.pin
    err = gpio_ctrl_adc_read(pin, &result)
    → {"pin": 3, "raw": 2048, "voltage_mv": 1650, "percentage": 50}
    → エラー時: {"error": "Pin 5 is not an ADC pin. Use GPIO 2, 3, or 4."}

  "pwm_set":
    pin = input_json.pin, duty = input_json.duty, freq = input_json.freq (省略時1000)
    err = gpio_ctrl_pwm_set(pin, duty, freq)
    → {"pin": 5, "duty": 75, "freq": 1000, "ok": true}

  "gpio_status":
    json_str = gpio_ctrl_status_json()
    → そのまま返す

  不明なツール名:
    → {"error": "Unknown tool: xxx"}
```

#### 会話履歴の管理

```c
#define NANOCLAW_MAX_HISTORY  5  // 最大5往復 = 10メッセージ

typedef struct {
    char role[16];         // "user", "assistant", "tool_result"
    char content[768];     // メッセージ内容 or ツール結果JSON
    // tool_use の場合のみ使用:
    char tool_use_id[32];
    char tool_name[20];
} history_entry_t;

static history_entry_t s_history[NANOCLAW_MAX_HISTORY * 2 + 6]; // ツール分の余裕
static int s_history_count = 0;

// リングバッファではなく、上限に達したら先頭から2エントリ（1往復）を削除してシフト
```

messages_json の構築では、Anthropic形式のtool_use/tool_resultのフォーマットに注意:

```json
[
  {"role": "user", "content": "LEDつけて"},
  {"role": "assistant", "content": [
    {"type": "tool_use", "id": "toolu_xxx", "name": "gpio_write", "input": {"pin": 5, "value": 1}}
  ]},
  {"role": "user", "content": [
    {"type": "tool_result", "tool_use_id": "toolu_xxx", "content": "{\"pin\":5,\"value\":1,\"ok\":true}"}
  ]},
  {"role": "assistant", "content": "GPIO5のLEDを点灯しました！"}
]
```

---

## 10. 自律監視モード

### 10.1 ルール管理

ルールは自然言語の文字列としてNVSに保存する。
AIがDiscordでの会話中にルールの追加・削除を行う場合は、
ルール管理用のツールを追加する（オプション）。
Phase 1 では CLIからのルール設定のみでもよい。

```c
#define NANOCLAW_MAX_RULES  5
#define NANOCLAW_MAX_RULE_LEN 256

static char s_rules[NANOCLAW_MAX_RULES][NANOCLAW_MAX_RULE_LEN];
static int s_rules_count = 0;

// NVSキー: "rule_count", "rule_0", "rule_1", ...
```

### 10.2 autonomous_check()

```
関数シグネチャ:
  char *autonomous_check(void);
  戻り値: Discord に報告するテキスト（NULL なら報告不要）

処理:
  1. ルールが0件なら即 NULL を返す

  2. 自律チェック用プロンプトを構築:
     "あなたはESP32-C3上のAIアシスタントです。
      以下の監視ルールに従って、センサーを確認し、
      必要に応じてアクションを実行してください。

      現在の監視ルール:
      1. {rule_0}
      2. {rule_1}
      ...

      ツールを使ってセンサーを読み取り、ルールに基づいて判断してください。
      アクションを実行した場合のみ報告してください。
      変化がなければ「変化なし」とだけ答えてください。"

  3. このプロンプトで react_loop() と同じツール呼び出しループを実行
     （ただし会話履歴には追加しない、一時的なやり取り）

  4. 応答テキストが "変化なし" を含む場合 → NULL を返す
  5. それ以外 → テキストを返す（Discordに報告される）
```

### 10.3 チェック間隔

```c
// デフォルト: 10回のポーリングごと = 約30秒
// CLIで変更可能: auto_interval コマンド
#define NANOCLAW_AUTO_CHECK_INTERVAL_DEFAULT  10
static int nanoclaw_auto_interval = NANOCLAW_AUTO_CHECK_INTERVAL_DEFAULT;
```

---

## 11. シリアルCLI

### 11.1 cli.c の仕様

ESP-IDFの `esp_console` コンポーネントを使用する。
USBシリアル (USB Serial/JTAG) 経由でコマンドを受け付ける。

#### CLIタスク

```
cli_init():
  1. esp_console_init()
  2. コマンド登録（後述）
  3. xTaskCreate(cli_task, "cli", 4096, NULL, 3, NULL)

cli_task():
  esp_console_repl を使用してREPLループを開始
  プロンプト: "nano> "
```

#### 登録コマンド一覧

| コマンド | 引数 | 説明 |
|---------|------|------|
| `wifi` | `<ssid> <password>` | WiFi認証情報をNVSに保存 |
| `wifi_status` | (なし) | WiFi接続状態とIPを表示 |
| `discord_token` | `<token>` | Discord Bot Token をNVSに保存 |
| `discord_channel` | `<channel_id>` | 監視チャンネルIDをNVSに保存 |
| `webhook` | `<url>` | Webhook URLをNVSに保存 |
| `api_key` | `<key>` | LLM APIキーをNVSに保存 |
| `provider` | `<anthropic\|openai>` | LLMプロバイダーをNVSに保存 |
| `model` | `<model_name>` | LLMモデル名をNVSに保存 |
| `prompt` | `<text>` | システムプロンプトをNVSに保存 |
| `gpio_read` | `<pin>` | GPIO値を読み取って表示 |
| `gpio_write` | `<pin> <0\|1>` | GPIO出力を設定 |
| `adc_read` | `<pin>` | ADC値を読み取って表示 |
| `pwm_set` | `<pin> <duty> [freq]` | PWM出力を設定 |
| `gpio_status` | (なし) | 全ピン状態を表示 |
| `rule_add` | `<rule_text>` | 監視ルールを追加 |
| `rule_list` | (なし) | 監視ルール一覧を表示 |
| `rule_clear` | (なし) | 全監視ルールを削除 |
| `auto_interval` | `<count>` | 自律チェック間隔を設定 |
| `auto_off` | (なし) | 自律チェックを停止 (interval=0) |
| `status` | (なし) | システム全体の状態を表示（ヒープ、WiFi、設定等） |
| `restart` | (なし) | ESP32を再起動 |
| `help` | (なし) | コマンド一覧を表示 |

---

## 12. NVS設定管理

### 12.1 NVS Namespace: "nanoclaw"

全ての設定を1つのnamespace `"nanoclaw"` に格納する。

| NVSキー | 型 | 最大長 | デフォルト | 説明 |
|---------|-----|--------|-----------|------|
| `wifi_ssid` | str | 32 | "" | WiFi SSID |
| `wifi_pass` | str | 64 | "" | WiFi パスワード |
| `bot_token` | str | 128 | "" | Discord Bot Token |
| `channel_id` | str | 24 | "" | 監視チャンネルID |
| `webhook_url` | str | 192 | "" | Webhook URL |
| `api_key` | str | 128 | "" | LLM API Key |
| `provider` | str | 16 | "anthropic" | "anthropic" or "openai" |
| `model` | str | 64 | "claude-sonnet-4-20250514" | モデル名 |
| `sys_prompt` | str | 512 | (後述) | カスタムシステムプロンプト |
| `last_msg_id` | str | 24 | "0" | 最後に読んだDiscordメッセージID |
| `rule_count` | i32 | - | 0 | 監視ルール数 |
| `rule_0`〜`rule_4` | str | 256 | "" | 監視ルール文字列 |
| `auto_interval` | i32 | - | 10 | 自律チェック間隔（ポーリング回数） |

### 12.2 デフォルトシステムプロンプト

```
あなたはSeeed Studio XIAO ESP32C3上で動作するAIアシスタント「NanoClaw」です。
ユーザーの指示に従ってGPIOピンを制御し、センサーの読み取りやLED・リレーの操作を行います。

利用可能なピン (XIAOラベル → ESP32C3 GPIO):
- D0/A0=GPIO2, D1/A1=GPIO3, D2/A2=GPIO4 (アナログ入力可能)
- D3=GPIO5, D4=GPIO6, D5=GPIO7, D10=GPIO10 (デジタル入出力)
- D6=GPIO21 (出力推奨), D7=GPIO20 (入力推奨), D8=GPIO8 (注意して使用)

注意事項:
- GPIO9(D9)はBOOTボタン用のため操作禁止
- GPIO11-17はFlash用のため操作禁止
- 出力ピンを変更する前に現在の状態を確認してください
- 不明確な指示の場合はユーザーに確認してください
- 簡潔に日本語で答えてください
```

---

## 13. メモリバジェット

### 13.1 見積もり

| 項目 | 使用量 | 備考 |
|------|--------|------|
| FreeRTOS + システム | ~80 KB | カーネル、ドライバ、タスク管理 |
| WiFi + lwIP | ~70 KB | WiFiスタック |
| TLS (mbedtls) × 1本 | ~45 KB | 同時1接続のみ |
| LLMレスポンスバッファ | 8 KB | 非ストリーミング時 |
| HTTPレスポンスバッファ | 4 KB | Discord API用 |
| ツール定義JSON（静的） | ~1.5 KB | コンパイル時定数 |
| 会話履歴 | ~12 KB | 16エントリ × ~768バイト |
| 監視ルール | ~1.3 KB | 5 × 256バイト |
| CLIタスクスタック | 4 KB | |
| app_mainスタック | 8 KB | sdkconfigで設定 |
| GPIO状態テーブル | ~0.3 KB | 22ピン × 14バイト |
| cJSON一時使用 | ~4 KB | パース時の一時領域 |
| その他 | ~5 KB | NVS, ログバッファ等 |
| **合計** | **~243 KB** | |
| **ESP32-C3 SRAM** | **400 KB** | |
| **残りヒープ** | **~157 KB** | 十分な安全マージン |

### 13.2 メモリ節約ルール

- malloc() の結果は**必ずチェック**する。NULLなら処理をスキップ。
- cJSON オブジェクトは使用後**即座に cJSON_Delete()**。
- HTTPクライアントは**リクエスト完了後即座に esp_http_client_cleanup()**。
- 大きなバッファ（LLMレスポンス等）はスタック変数ではなく**heap上に確保**し、使用後free()。
- 文字列コピーは**strncpy**を使い、必ずnull-terminateを保証。

---

## 14. エラーハンドリング

| エラー状況 | 対処 |
|-----------|------|
| WiFi切断 | イベントハンドラで自動再接続（指数バックオフ 1s〜60s） |
| Discord 401 Unauthorized | ESP_LOGE "Invalid bot token" → 次のポーリングまでスキップ |
| Discord 403 Forbidden | ESP_LOGE "Missing permissions" → 次のポーリングまでスキップ |
| Discord 429 Rate Limit | レスポンスJSONの `retry_after` 秒待機してリトライ |
| Discord ネットワークエラー | 3秒→6秒→12秒... 最大60秒のバックオフ |
| LLM API エラー (HTTP 4xx/5xx) | エラーメッセージをDiscordに返信 |
| LLM APIタイムアウト | "応答がタイムアウトしました" をDiscordに返信 |
| malloc 失敗 | ESP_LOGE → 会話履歴をクリアして再試行 |
| JSONパース失敗 | ESP_LOGW → スキップして次のイテレーションへ |
| 不正なGPIOピン指定 | ツール結果でエラーJSON返却 `{"error": "..."}` |
| ADCピン以外でadc_read | ツール結果でエラーJSON返却 |
| PWMチャンネル枯渇 | ツール結果でエラーJSON返却 |

---

## 15. ビルド設定

### 15.1 sdkconfig.defaults

```ini
# ターゲット
CONFIG_IDF_TARGET="esp32c3"

# Flash
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y

# パーティション
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# app_main タスクスタック
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192

# WiFi
CONFIG_ESP_WIFI_SSID=""
CONFIG_ESP_WIFI_PASSWORD=""

# TLS (mbedtls)
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y

# HTTP client
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y

# ログレベル
CONFIG_LOG_DEFAULT_LEVEL_INFO=y

# USB Serial JTAG Console
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y

# NVS
CONFIG_NVS_ENCRYPTION=n
```

### 15.2 partitions.csv

```csv
# Name,   Type, SubType, Offset,  Size,   Flags
nvs,      data, nvs,     0x9000,  0x8000,
phy_init, data, phy,     0x11000, 0x1000,
factory,  app,  factory, 0x20000, 0x1E0000,
```

NVSを32KBに拡大（デフォルトの24KBから）。ルールや設定が増えても余裕を持たせる。
SPIFFSは不使用。

---

## 16. 定数一覧

### nanoclaw_config.h

```c
#pragma once

/* ── ビルド時デフォルト（mimi_secrets.h 相当） ── */
#ifndef NANOCLAW_DEFAULT_WIFI_SSID
#define NANOCLAW_DEFAULT_WIFI_SSID      ""
#endif
#ifndef NANOCLAW_DEFAULT_WIFI_PASS
#define NANOCLAW_DEFAULT_WIFI_PASS      ""
#endif
#ifndef NANOCLAW_DEFAULT_BOT_TOKEN
#define NANOCLAW_DEFAULT_BOT_TOKEN      ""
#endif
#ifndef NANOCLAW_DEFAULT_CHANNEL_ID
#define NANOCLAW_DEFAULT_CHANNEL_ID     ""
#endif
#ifndef NANOCLAW_DEFAULT_WEBHOOK_URL
#define NANOCLAW_DEFAULT_WEBHOOK_URL    ""
#endif
#ifndef NANOCLAW_DEFAULT_API_KEY
#define NANOCLAW_DEFAULT_API_KEY        ""
#endif

/* ── Discord ── */
#define NANOCLAW_POLL_INTERVAL_MS       3000    /* ポーリング間隔 (ms) */
#define NANOCLAW_MAX_POLL_MSGS          5       /* 1回のポーリングで取得する最大メッセージ数 */
#define NANOCLAW_DISCORD_MAX_MSG_LEN    2000    /* Discordメッセージ文字数制限 */
#define NANOCLAW_HTTP_TIMEOUT_MS        10000   /* HTTP タイムアウト */
#define NANOCLAW_HTTP_BUF_SIZE          2048    /* HTTPレスポンスバッファ (Discord用) */

/* ── LLM ── */
#define NANOCLAW_LLM_DEFAULT_PROVIDER   "anthropic"
#define NANOCLAW_LLM_DEFAULT_MODEL      "claude-sonnet-4-20250514"
#define NANOCLAW_LLM_MAX_TOKENS         1024
#define NANOCLAW_LLM_RESP_BUF_SIZE      8192    /* LLMレスポンスバッファ */
#define NANOCLAW_LLM_TIMEOUT_MS         30000   /* LLM API タイムアウト */

#define NANOCLAW_ANTHROPIC_API_URL      "https://api.anthropic.com/v1/messages"
#define NANOCLAW_ANTHROPIC_VERSION      "2023-06-01"
#define NANOCLAW_OPENAI_API_URL         "https://api.openai.com/v1/chat/completions"

/* ── ReAct ── */
#define NANOCLAW_MAX_TOOL_CALLS         3       /* 1メッセージあたりの最大ツール呼び出し回数 */
#define NANOCLAW_MAX_HISTORY            5       /* 会話履歴の最大往復数 */

/* ── GPIO ── */
#define NANOCLAW_GPIO_ALLOWED_MASK      ((1ULL<<2)|(1ULL<<3)|(1ULL<<4)|(1ULL<<5)|\
                                         (1ULL<<6)|(1ULL<<7)|(1ULL<<8)|(1ULL<<10)|\
                                         (1ULL<<20)|(1ULL<<21))
#define NANOCLAW_ADC_ALLOWED_MASK       ((1ULL<<2)|(1ULL<<3)|(1ULL<<4))
#define NANOCLAW_PWM_MAX_CHANNELS       6

/* ── 自律監視 ── */
#define NANOCLAW_MAX_RULES              5
#define NANOCLAW_MAX_RULE_LEN           256
#define NANOCLAW_AUTO_CHECK_INTERVAL_DEFAULT  10 /* ポーリング回数ごと */

/* ── WiFi ── */
#define NANOCLAW_WIFI_MAX_RETRY         10
#define NANOCLAW_WIFI_CONNECT_TIMEOUT_MS 30000

/* ── CLI ── */
#define NANOCLAW_CLI_STACK              4096
#define NANOCLAW_CLI_PRIO               3

/* ── NVS ── */
#define NANOCLAW_NVS_NAMESPACE          "nanoclaw"
```

---

## 17. ユーザーセットアップ手順

（CLIの `help` コマンドでも表示する）

### 17.1 Discord Bot の作成

1. https://discord.com/developers/applications にアクセス
2. 「New Application」→ 名前を入力（例: "NanoClaw"）
3. 左メニュー「Bot」→「Reset Token」→ **トークンをコピー**（一度だけ表示）
4. **Privileged Gateway Intents** で **「MESSAGE CONTENT INTENT」を ON にする**
5. 左メニュー「OAuth2」→「URL Generator」
   - Scopes: `bot` にチェック
   - Bot Permissions: `Send Messages`, `Read Message History`, `View Channels`
6. 生成されたURLをブラウザで開き、ボットをサーバーに追加

### 17.2 Webhook の作成

1. Discordでボットを追加したサーバーの対象チャンネルを開く
2. チャンネル名の横の ⚙️ → 「連携サービス」→「ウェブフックを作成」
3. 名前を「NanoClaw」に変更（任意）
4. **「ウェブフックURLをコピー」**

### 17.3 チャンネルID の取得

1. Discordアプリの設定 → 「詳細設定」→ **「開発者モード」を ON**
2. 対象チャンネルを右クリック → **「IDをコピー」**

### 17.4 ESP32への設定

```
nano> wifi <SSID> <PASSWORD>
nano> discord_token <BOT_TOKEN>
nano> discord_channel <CHANNEL_ID>
nano> webhook <WEBHOOK_URL>
nano> api_key <LLM_API_KEY>
nano> restart
```

### 17.5 動作確認

1. シリアルモニタで「WiFi connected」「Discord polling started」を確認
2. Discordチャンネルで「こんにちは」と送信
3. ボットが数秒後に返答すれば成功
4. 「GPIO5にLEDをつないだ。点灯して」で GPIO制御を確認

---

## 実装優先度

### Phase 1 (MVP) — まずこれだけ動かす

1. `nanoclaw_config.h` — 定数定義
2. `wifi.c` — WiFi接続
3. `discord.c` — REST polling + Webhook送信
4. `llm.c` — LLM API呼び出し（**非ストリーミング**でOK）
5. `gpio_ctrl.c` — GPIO制御（全5操作）
6. `tools.c` — ReActループ（ツール呼び出し1回分でもOK）
7. `cli.c` — 最低限の設定コマンド
8. `nanoclaw.c` — メインループ

### Phase 2 — 安定化

- LLMストリーミング受信
- 完全なReActループ（最大3回）
- 自律監視モード
- エラーハンドリング強化
- Rate Limit対応

### Phase 3 — 拡張

- OpenAI対応
- OTA
- 時刻取得ツール（自律判断の「夜中は消して」に必要）
- Discord上でのルール管理（add_rule / remove_rule ツール）
