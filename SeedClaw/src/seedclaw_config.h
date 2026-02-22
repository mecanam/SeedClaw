#pragma once

/* ── secrets.h から設定を読み込む ── */
#include "secrets.h"

/* ── ビルド時デフォルト（secrets.h で未定義の場合のフォールバック） ── */
#ifndef SEEDCLAW_DEFAULT_WIFI_SSID
#define SEEDCLAW_DEFAULT_WIFI_SSID      ""
#endif
#ifndef SEEDCLAW_DEFAULT_WIFI_PASS
#define SEEDCLAW_DEFAULT_WIFI_PASS      ""
#endif
#ifndef SEEDCLAW_DEFAULT_BOT_TOKEN
#define SEEDCLAW_DEFAULT_BOT_TOKEN      ""
#endif
#ifndef SEEDCLAW_DEFAULT_CHANNEL_ID
#define SEEDCLAW_DEFAULT_CHANNEL_ID     ""
#endif
#ifndef SEEDCLAW_DEFAULT_WEBHOOK_URL
#define SEEDCLAW_DEFAULT_WEBHOOK_URL    ""
#endif
#ifndef SEEDCLAW_DEFAULT_API_KEY
#define SEEDCLAW_DEFAULT_API_KEY        ""
#endif

/* ── Discord ── */
#define SEEDCLAW_POLL_INTERVAL_MS       3000    /* ポーリング間隔 (ms) */
#define SEEDCLAW_MAX_POLL_MSGS          3       /* 1回のポーリングで取得する最大メッセージ数 */
#define SEEDCLAW_DISCORD_MAX_MSG_LEN    2000    /* Discordメッセージ文字数制限 */
#define SEEDCLAW_HTTP_TIMEOUT_MS        10000   /* HTTP タイムアウト */
#define SEEDCLAW_HTTP_BUF_SIZE          8192    /* HTTPレスポンスバッファ (Discord用) */

/* ── LLM ── */
#define SEEDCLAW_LLM_DEFAULT_PROVIDER   "anthropic"
#define SEEDCLAW_LLM_DEFAULT_MODEL      "claude-haiku-4-5-20251001"
#define SEEDCLAW_LLM_MAX_TOKENS         1024
#define SEEDCLAW_LLM_RESP_BUF_SIZE      8192    /* LLMレスポンスバッファ */
#define SEEDCLAW_LLM_TIMEOUT_MS         30000   /* LLM API タイムアウト */

#define SEEDCLAW_ANTHROPIC_API_URL      "https://api.anthropic.com/v1/messages"
#define SEEDCLAW_ANTHROPIC_VERSION      "2023-06-01"
#define SEEDCLAW_OPENAI_API_URL         "https://api.openai.com/v1/chat/completions"

/* ── ReAct ── */
#define SEEDCLAW_MAX_TOOL_CALLS         5       /* 1メッセージあたりの最大ツール呼び出し回数 */
#define SEEDCLAW_MAX_HISTORY            3       /* 会話履歴の最大往復数 */

/* ── GPIO ── */
#define SEEDCLAW_GPIO_ALLOWED_MASK      ((1ULL<<2)|(1ULL<<3)|(1ULL<<4)|(1ULL<<5)|\
                                         (1ULL<<6)|(1ULL<<7)|(1ULL<<8)|(1ULL<<10)|\
                                         (1ULL<<20)|(1ULL<<21))
#define SEEDCLAW_ADC_ALLOWED_MASK       ((1ULL<<2)|(1ULL<<3)|(1ULL<<4))
#define SEEDCLAW_PWM_MAX_CHANNELS       6

/* ── 自律監視 ── */
#define SEEDCLAW_MAX_RULES              5
#define SEEDCLAW_MAX_RULE_LEN           256
#define SEEDCLAW_AUTO_CHECK_INTERVAL_DEFAULT  10 /* ポーリング回数ごと */

/* ── WiFi ── */
#define SEEDCLAW_WIFI_MAX_RETRY         10
#define SEEDCLAW_WIFI_CONNECT_TIMEOUT_MS 30000

/* ── CLI ── */
#define SEEDCLAW_CLI_STACK              4096
#define SEEDCLAW_CLI_PRIO               3

/* ── NVS ── */
#define SEEDCLAW_NVS_NAMESPACE          "seedclaw"

/* ── デフォルトシステムプロンプト ── */
#define SEEDCLAW_DEFAULT_SYSTEM_PROMPT \
"あなたはXIAO ESP32C3上のAIアシスタント「SeedClaw」です。GPIO制御とセンサー読み取りを行います。\n" \
"\n" \
"【最重要】ハードウェア操作は必ずツールを呼んで実行せよ。テキストだけで「しました」は禁止。\n" \
"\n" \
"ピン: D0/A0=GPIO2, D1/A1=GPIO3, D2/A2=GPIO4 (ADC可), D3=5, D4=6, D5=7, D6=21, D7=20, D8=8, D10=10\n" \
"禁止: GPIO9(BOOT), GPIO11-17(Flash)\n" \
"\n" \
"自律監視:\n" \
"- 「監視して」「定期チェック」→ rule_add + set_auto_interval\n" \
"- 「監視やめて」「止めて」→ rule_clear\n" \
"- interval=3で約9秒ごと、10で約30秒ごと\n" \
"\n" \
"簡潔に日本語で答えてください"
