<p align="center">
  <picture>
    <img src="./SeedClaw.png" alt="SeedClaw" width="800">
  </picture>
</p>

<p align="center">
  <strong>$4 マイコンで動く Discord AI ボット</strong><br>
  自然言語で GPIO 制御 &amp; 自律センサー監視 — Pure C / ESP32-C3
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-ESP32--C3-green?style=flat-square" alt="ESP32-C3">
  <img src="https://img.shields.io/badge/Language-C_(ESP--IDF)-blue?style=flat-square" alt="C">
  <img src="https://img.shields.io/badge/LLM-Claude_Haiku_4.5-orange?style=flat-square" alt="Claude">
  <img src="https://img.shields.io/badge/Interface-Discord-5865F2?style=flat-square" alt="Discord">
  <img src="https://img.shields.io/badge/License-MIT-yellow?style=flat-square" alt="MIT">
</p>

---

## SeedClaw とは？

**SeedClaw** は [Seeed Studio XIAO ESP32C3](https://www.seeedstudio.com/Seeed-XIAO-ESP32C3-p-5431.html) 上で動作する超軽量 Discord AI ボットです。LLM（Claude Haiku 4.5）が Discord のメッセージを理解し、GPIO 操作（センサー読み取り、LED 制御、モーター駆動など）を自律的に実行します。

## 特徴

- **自然言語で GPIO 制御** — Discord で「GPIO5 の LED を点灯して」と送るだけで操作完了
- **ReAct ツール呼び出し** — LLM が自律的にツールを選択・実行（デジタル I/O、ADC、PWM、Web 取得など）
- **複数 GPIO 同時操作** — 「D3 と D4 の LED を両方点灯させて」のような指示も 1 回のレスポンスで処理
- **自律監視** — 自然言語でルール設定：「GPIO3 のセンサーを 10 秒ごとに監視して、2000 を超えたら教えて」
- **Web データ取得** — LLM が外部 API（天気、株価など）を取得してデータに基づいた制御が可能
- **Discord レート制限対応** — 429 レスポンス時の自動リトライ・バックオフ
- **シリアル CLI** — USB 経由で WiFi・API キー設定、GPIO テストが可能
- **Pure C / ESP-IDF** — Arduino 不使用、MicroPython 不使用。400KB SRAM で動作

## ハードウェア

| 項目 | スペック |
|------|----------|
| ボード | [Seeed Studio XIAO ESP32C3](https://www.seeedstudio.com/Seeed-XIAO-ESP32C3-p-5431.html) |
| CPU | ESP32-C3 RISC-V 160MHz |
| SRAM | 400KB（静的使用量 約 47KB） |
| Flash | 4MB |
| WiFi | 802.11 b/g/n |
| 価格 | 約 $4 USD |

## クイックスタート

### 1. クローン & シークレット設定

```bash
git clone https://github.com/<your-username>/SeedClaw.git
cd SeedClaw
```

`src/secrets.h` を作成し、認証情報を記入：

```c
#pragma once

#define SEEDCLAW_DEFAULT_WIFI_SSID    "WiFiのSSID"
#define SEEDCLAW_DEFAULT_WIFI_PASS    "WiFiのパスワード"
#define SEEDCLAW_DEFAULT_BOT_TOKEN    "Discord-Botトークン"
#define SEEDCLAW_DEFAULT_CHANNEL_ID   "チャンネルID"
#define SEEDCLAW_DEFAULT_WEBHOOK_URL  "https://discord.com/api/webhooks/..."
#define SEEDCLAW_DEFAULT_API_KEY      "sk-ant-..."
```

> `src/secrets.h` は `.gitignore` に登録済みのため、コミットされません。シリアル CLI から実行時に設定することも可能です。

### 2. ビルド & フラッシュ（PlatformIO）

```bash
# PlatformIO CLI のインストール（未インストールの場合）
pip install platformio

# ビルド
pio run

# ESP32C3 にフラッシュ書き込み
pio run -t upload

# シリアルモニタを起動
pio device monitor
```

### 3. Discord Bot の作成

1. [Discord Developer Portal](https://discord.com/developers/applications) にアクセス
2. 「New Application」で新規作成（例: "SeedClaw"）
3. 左メニュー「Bot」>「Reset Token」> **トークンをコピー**
4. **「MESSAGE CONTENT INTENT」を ON にする**
5. 左メニュー「OAuth2」>「URL Generator」
   - Scopes: `bot` にチェック
   - Bot Permissions: `Send Messages`, `Read Message History`, `View Channels`
6. 生成された URL でボットをサーバーに招待

### 4. Webhook の作成

1. Discord でボットを追加したチャンネルを開く
2. チャンネル設定 > 「連携サービス」>「ウェブフックを作成」
3. **ウェブフック URL をコピー**

### 5. ESP32 への設定（シリアル CLI）

`secrets.h` を作成しなかった場合、シリアル CLI から設定：

```
seed> wifi <SSID> <PASSWORD>
seed> discord_token <BOT_TOKEN>
seed> discord_channel <CHANNEL_ID>
seed> webhook <WEBHOOK_URL>
seed> api_key <API_KEY>
seed> restart
```

## 使い方

### Discord からの GPIO 制御

Discord チャンネルで自然言語メッセージを送信：

```
GPIO5 に LED をつないだ。点灯して
```

```
D3 と D4 の LED を両方点灯させて
```

```
GPIO3 の ADC 値を読み取って
```

```
GPIO5 の PWM を 50% にして
```

SeedClaw が意図を理解し、適切な GPIO ツールを呼び出して結果を返答します。

### Discord からの自律監視設定

自然言語で監視ルールを設定できます：

```
GPIO3 のセンサーを 10 秒ごとに監視して、値が 2000 を超えたら教えて
```

```
監視やめて
```

LLM が裏で `rule_add` + `set_auto_interval` ツールを自動的に呼び出します。ルールは RAM に保存され、電源を切るとリセットされます。

### Web データ取得

LLM が外部データを取得して判断に使用できます：

```
東京の天気を調べて、雨なら GPIO5 の LED を点灯して
```

## アーキテクチャ

```
Discord ←── REST API ──→ ESP32-C3 (XIAO)
  ↑                          │
  │ Webhook                  ├─ LLM API (Claude Haiku 4.5)
  │                          │   └─ ReAct ツールループ
  └──────────────────────────├─ GPIO 制御
                             │   ├─ デジタル I/O
                             │   ├─ ADC (12-bit)
                             │   └─ PWM
                             ├─ Web Fetch (HTTPS)
                             ├─ 自律監視エンジン
                             │   ├─ ルール管理（最大 5 件）
                             │   └─ 定期チェック
                             └─ シリアル CLI
```

### LLM ツール一覧

| ツール | 説明 |
|--------|------|
| `gpio_read` | GPIO ピンのデジタル値を読み取り（HIGH/LOW） |
| `gpio_write` | GPIO ピンにデジタル値を出力 |
| `adc_read` | アナログ値を読み取り（0-4095、12-bit） |
| `pwm_set` | PWM 出力を設定（デューティ 0-100%、周波数設定可） |
| `gpio_status` | 設定済み全 GPIO ピンの状態を取得 |
| `web_fetch` | URL からデータを取得（HTTP/HTTPS） |
| `rule_add` | 自律監視ルールを追加 |
| `rule_remove` | 監視ルールを番号指定で削除 |
| `rule_clear` | 全監視ルールを削除 |
| `set_auto_interval` | 監視チェック間隔を設定 |
| `get_rules` | 現在の監視ルール一覧を取得 |

### メインループ

1. **Discord ポーリング** — 3 秒ごとに新しいメッセージを取得
2. **ReAct ループ** — LLM がメッセージを処理し、必要に応じてツールを呼び出し（1 メッセージあたり最大 5 回）
3. **Webhook 返信** — 結果を Discord に送信
4. **自律チェック** — 設定された間隔で監視ルールを実行し、変化があれば報告

## GPIO ピンマップ（XIAO ESP32C3）

| ラベル | GPIO | 用途 | 安全度 |
|--------|------|------|--------|
| D0/A0 | 2 | アナログ入力 | :warning: Strapping Pin |
| D1/A1 | 3 | アナログ入力 | :white_check_mark: 安全 |
| D2/A2 | 4 | アナログ入力 | :white_check_mark: 安全 |
| D3 | 5 | **デジタル出力（推奨）** | :white_check_mark: 最も安全 |
| D4 | 6 | デジタル入出力 | :white_check_mark: 安全 |
| D5 | 7 | デジタル入出力 | :white_check_mark: 安全 |
| D6 | 21 | デジタル出力 | :warning: 起動時 UART |
| D7 | 20 | デジタル入力 | :warning: UART RX 兼用 |
| D8 | 8 | デジタル入出力 | :warning: Strapping |
| D10 | 10 | **デジタル出力（推奨）** | :white_check_mark: 安全 |
| D9 | 9 | **使用禁止** | :x: BOOT ボタン |

## CLI コマンド一覧

| コマンド | 説明 |
|----------|------|
| `wifi <ssid> <password>` | WiFi 認証情報を設定 |
| `wifi_status` | WiFi 接続状態を表示 |
| `discord_token <token>` | Discord Bot Token を設定 |
| `discord_channel <id>` | Discord チャンネル ID を設定 |
| `webhook <url>` | Webhook URL を設定 |
| `api_key <key>` | LLM API Key を設定 |
| `provider <anthropic\|openai>` | LLM プロバイダーを設定 |
| `model <model_name>` | LLM モデル名を設定 |
| `gpio_read <pin>` | GPIO ピンを読み取り |
| `gpio_write <pin> <0\|1>` | GPIO ピンに出力 |
| `adc_read <pin>` | ADC 値を読み取り |
| `pwm_set <pin> <duty> [freq]` | PWM 出力を設定 |
| `gpio_status` | 全 GPIO 状態を表示 |
| `rule_add <text>` | 監視ルールを追加 |
| `rule_list` | 監視ルール一覧を表示 |
| `rule_clear` | 全ルールを削除 |
| `auto_interval <count>` | 自律チェック間隔を設定（ポーリング回数） |
| `auto_off` | 自律監視を無効化 |
| `prompt <text>` | システムプロンプトを変更 |
| `status` | システム状態を表示 |
| `restart` | ESP32 を再起動 |

## プロジェクト構造

```
SeedClaw/
├── src/
│   ├── seedclaw.c          # メインエントリ & メインループ
│   ├── seedclaw_config.h   # 全設定定数
│   ├── secrets.h           # 認証情報（gitignore 対象）
│   ├── wifi.c / wifi.h     # WiFi 接続管理
│   ├── discord.c / discord.h # Discord REST API & Webhook
│   ├── llm.c / llm.h       # LLM API クライアント（Anthropic）
│   ├── tools.c / tools.h   # ReAct ツールループ & 自律監視
│   ├── gpio_ctrl.c / gpio_ctrl.h # GPIO/ADC/PWM ドライバー
│   └── cli.c / cli.h       # シリアル CLI（USB）
├── platformio.ini           # PlatformIO ビルド設定
├── partitions.csv           # カスタムパーティションテーブル
├── sdkconfig.defaults       # ESP-IDF デフォルト設定
└── CMakeLists.txt           # トップレベル CMake
```

## トラブルシューティング

### WiFi に接続できない

```
seed> wifi_status
seed> wifi <正しいSSID> <正しいパスワード>
seed> restart
```

### Discord からメッセージが取得できない

- Bot Token が正しいか確認
- チャンネル ID が正しいか確認
- Bot に **MESSAGE CONTENT INTENT** 権限があるか確認
- Bot がチャンネルを閲覧できる権限があるか確認

### LLM API が動作しない

- API Key が正しいか確認
- `wifi_status` で WiFi 接続を確認
- `status` コマンドでヒープメモリ残量を確認

### 複数 GPIO を同時に操作できない

- 最新ファームウェアに更新してください
- LLM が 1 回のレスポンスで複数ツールを呼び出し、SeedClaw が全て実行します

## ライセンス

MIT License

## 謝辞

本プロジェクトは [**MimiClaw**](https://github.com/memovai/mimiclaw) に大きな影響を受けています。MimiClaw は ESP32-S3 + Telegram で動作する AI エージェントで、「$5 のマイコン上で LLM エージェントを動かす」というコンセプトの先駆けです。SeedClaw はそのアイデアを ESP32-C3 + Discord + GPIO 制御に展開したものです。

## クレジット

- 謝辞: [MimiClaw](https://github.com/memovai/mimiclaw) by memovai
- ハードウェア: [Seeed Studio XIAO ESP32C3](https://www.seeedstudio.com/Seeed-XIAO-ESP32C3-p-5431.html)
- フレームワーク: [ESP-IDF](https://github.com/espressif/esp-idf) v5.5.x（PlatformIO 経由）
- LLM: [Anthropic Claude](https://www.anthropic.com/) Haiku 4.5
