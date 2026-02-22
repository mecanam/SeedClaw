#include "cli.h"
#include "seedclaw_config.h"
#include "wifi.h"
#include "discord.h"
#include "llm.h"
#include "gpio_ctrl.h"
#include "tools.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cli";

// ===== コマンド実装 =====

static int cmd_wifi(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: wifi <ssid> <password>\n");
        return 1;
    }

    esp_err_t err = wifi_set_credentials(argv[1], argv[2]);
    if (err == ESP_OK) {
        printf("WiFi credentials saved. Restart to apply.\n");
    } else {
        printf("Failed to save WiFi credentials: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_wifi_status(int argc, char **argv)
{
    printf("WiFi connected: %s\n", wifi_is_connected() ? "YES" : "NO");
    if (wifi_is_connected()) {
        printf("IP address: %s\n", wifi_get_ip());
    }
    return 0;
}

static int cmd_discord_token(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: discord_token <token>\n");
        return 1;
    }

    esp_err_t err = discord_set_token(argv[1]);
    if (err == ESP_OK) {
        printf("Discord bot token saved.\n");
    } else {
        printf("Failed to save token: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_discord_channel(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: discord_channel <channel_id>\n");
        return 1;
    }

    esp_err_t err = discord_set_channel(argv[1]);
    if (err == ESP_OK) {
        printf("Discord channel ID saved.\n");
    } else {
        printf("Failed to save channel ID: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_webhook(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: webhook <url>\n");
        return 1;
    }

    esp_err_t err = discord_set_webhook(argv[1]);
    if (err == ESP_OK) {
        printf("Webhook URL saved.\n");
    } else {
        printf("Failed to save webhook URL: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_api_key(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: api_key <key>\n");
        return 1;
    }

    esp_err_t err = llm_set_api_key(argv[1]);
    if (err == ESP_OK) {
        printf("LLM API key saved.\n");
    } else {
        printf("Failed to save API key: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_provider(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: provider <anthropic|openai>\n");
        return 1;
    }

    esp_err_t err = llm_set_provider(argv[1]);
    if (err == ESP_OK) {
        printf("LLM provider set to: %s\n", argv[1]);
    } else {
        printf("Failed to set provider: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_model(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: model <model_name>\n");
        return 1;
    }

    esp_err_t err = llm_set_model(argv[1]);
    if (err == ESP_OK) {
        printf("LLM model set to: %s\n", argv[1]);
    } else {
        printf("Failed to set model: %s\n", esp_err_to_name(err));
    }
    return 0;
}

static int cmd_gpio_read(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: gpio_read <pin>\n");
        return 1;
    }

    int pin = atoi(argv[1]);
    int value = gpio_ctrl_read(pin);
    if (value < 0) {
        printf("Failed to read GPIO%d\n", pin);
    } else {
        printf("GPIO%d = %d (%s)\n", pin, value, value ? "HIGH" : "LOW");
    }
    return 0;
}

static int cmd_gpio_write(int argc, char **argv)
{
    if (argc != 3) {
        printf("Usage: gpio_write <pin> <0|1>\n");
        return 1;
    }

    int pin = atoi(argv[1]);
    int value = atoi(argv[2]);
    esp_err_t err = gpio_ctrl_write(pin, value);
    if (err == ESP_OK) {
        printf("GPIO%d set to %d (%s)\n", pin, value, value ? "HIGH" : "LOW");
    } else {
        printf("Failed to write GPIO%d: %s\n", pin, esp_err_to_name(err));
    }
    return 0;
}

static int cmd_adc_read(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: adc_read <pin>\n");
        return 1;
    }

    int pin = atoi(argv[1]);
    adc_result_t result;
    esp_err_t err = gpio_ctrl_adc_read(pin, &result);
    if (err == ESP_OK) {
        printf("ADC GPIO%d: raw=%d, voltage=%dmV, %%=%d\n",
               pin, result.raw, result.voltage_mv, result.percentage);
    } else {
        printf("Failed to read ADC on GPIO%d: %s\n", pin, esp_err_to_name(err));
    }
    return 0;
}

static int cmd_pwm_set(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        printf("Usage: pwm_set <pin> <duty> [freq]\n");
        return 1;
    }

    int pin = atoi(argv[1]);
    int duty = atoi(argv[2]);
    int freq = (argc == 4) ? atoi(argv[3]) : 1000;

    esp_err_t err = gpio_ctrl_pwm_set(pin, duty, freq);
    if (err == ESP_OK) {
        printf("PWM GPIO%d: duty=%d%%, freq=%dHz\n", pin, duty, freq);
    } else {
        printf("Failed to set PWM on GPIO%d: %s\n", pin, esp_err_to_name(err));
    }
    return 0;
}

static int cmd_gpio_status(int argc, char **argv)
{
    char *status = gpio_ctrl_status_json();
    if (status != NULL) {
        printf("%s\n", status);
        free(status);
    } else {
        printf("Failed to get GPIO status\n");
    }
    return 0;
}

static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    printf("=== SeedClaw System Status ===\n");
    printf("Free heap: %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("WiFi: %s\n", wifi_is_connected() ? "Connected" : "Disconnected");
    if (wifi_is_connected()) {
        printf("  IP: %s\n", wifi_get_ip());
    }
    printf("Monitoring rules: %d/%d\n", rules_count(), SEEDCLAW_MAX_RULES);
    int interval = auto_interval_get();
    if (interval > 0) {
        printf("Auto check: every %d polls (~%ds)\n",
               interval, (interval * SEEDCLAW_POLL_INTERVAL_MS) / 1000);
    } else {
        printf("Auto check: disabled\n");
    }
    return 0;
}

static int cmd_rule_add(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: rule_add <rule text>\n");
        return 1;
    }
    char rule[SEEDCLAW_MAX_RULE_LEN];
    rule[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(rule, " ", sizeof(rule) - strlen(rule) - 1);
        strncat(rule, argv[i], sizeof(rule) - strlen(rule) - 1);
    }
    esp_err_t err = rules_add(rule);
    if (err == ESP_OK) {
        printf("Rule added (%d/%d): %s\n", rules_count(), SEEDCLAW_MAX_RULES, rule);
    } else {
        printf("Cannot add rule (max %d reached).\n", SEEDCLAW_MAX_RULES);
    }
    return 0;
}

static int cmd_rule_list(int argc, char **argv)
{
    printf("=== Monitoring Rules (%d/%d) ===\n", rules_count(), SEEDCLAW_MAX_RULES);
    rules_list();
    printf("Auto interval: %d polls (0=disabled)\n", auto_interval_get());
    return 0;
}

static int cmd_rule_clear(int argc, char **argv)
{
    rules_clear();
    printf("All monitoring rules cleared.\n");
    return 0;
}

static int cmd_auto_interval(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: auto_interval <count>\n");
        printf("Current: %d polls (%dms per poll)\n",
               auto_interval_get(), SEEDCLAW_POLL_INTERVAL_MS);
        return 1;
    }
    int interval = atoi(argv[1]);
    if (interval < 0) interval = 0;
    auto_interval_set(interval);
    if (interval == 0) {
        printf("Autonomous monitoring disabled.\n");
    } else {
        printf("Auto check interval set to %d polls (~%ds).\n",
               interval, (interval * SEEDCLAW_POLL_INTERVAL_MS) / 1000);
    }
    return 0;
}

static int cmd_auto_off(int argc, char **argv)
{
    auto_interval_set(0);
    printf("Autonomous monitoring disabled.\n");
    return 0;
}

static int cmd_prompt(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: prompt <system prompt text>\n");
        return 1;
    }
    char prompt[1024];
    prompt[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(prompt, " ", sizeof(prompt) - strlen(prompt) - 1);
        strncat(prompt, argv[i], sizeof(prompt) - strlen(prompt) - 1);
    }
    esp_err_t err = llm_set_system_prompt(prompt);
    if (err == ESP_OK) {
        printf("System prompt updated (%d bytes).\n", (int)strlen(prompt));
    } else {
        printf("Failed to save prompt: %s\n", esp_err_to_name(err));
    }
    return 0;
}

// ===== コマンド登録ヘルパー =====

static void register_cmd(const char *command, esp_console_cmd_func_t func,
                          const char *help, const char *hint)
{
    const esp_console_cmd_t cmd = {
        .command = command,
        .help = help,
        .hint = hint,
        .func = func,
        .argtable = NULL,
    };
    esp_console_cmd_register(&cmd);
}

// ===== CLI初期化 =====

static void cli_task(void *arg)
{
    // USB Serial JTAG ドライバー初期化
    usb_serial_jtag_driver_config_t usb_serial_config = {
        .rx_buffer_size = 1024,
        .tx_buffer_size = 1024,
    };
    usb_serial_jtag_driver_install(&usb_serial_config);

    usb_serial_jtag_vfs_use_driver();

    // Console初期化
    esp_console_config_t console_config = {
        .max_cmdline_length = 256,
        .max_cmdline_args = 8,
    };
    esp_console_init(&console_config);

    // コマンド登録
    register_cmd("wifi", cmd_wifi, "Set WiFi credentials", "wifi <ssid> <password>");
    register_cmd("wifi_status", cmd_wifi_status, "Show WiFi status", NULL);
    register_cmd("discord_token", cmd_discord_token, "Set Discord bot token", "discord_token <token>");
    register_cmd("discord_channel", cmd_discord_channel, "Set Discord channel ID", "discord_channel <id>");
    register_cmd("webhook", cmd_webhook, "Set webhook URL", "webhook <url>");
    register_cmd("api_key", cmd_api_key, "Set LLM API key", "api_key <key>");
    register_cmd("provider", cmd_provider, "Set LLM provider", "provider <anthropic|openai>");
    register_cmd("model", cmd_model, "Set LLM model", "model <model_name>");
    register_cmd("gpio_read", cmd_gpio_read, "Read GPIO pin", "gpio_read <pin>");
    register_cmd("gpio_write", cmd_gpio_write, "Write GPIO pin", "gpio_write <pin> <0|1>");
    register_cmd("adc_read", cmd_adc_read, "Read ADC value", "adc_read <pin>");
    register_cmd("pwm_set", cmd_pwm_set, "Set PWM output", "pwm_set <pin> <duty> [freq]");
    register_cmd("gpio_status", cmd_gpio_status, "Show GPIO status", NULL);
    register_cmd("status", cmd_status, "Show system status", NULL);
    register_cmd("restart", cmd_restart, "Restart ESP32", NULL);
    register_cmd("rule_add", cmd_rule_add, "Add monitoring rule", "rule_add <text>");
    register_cmd("rule_list", cmd_rule_list, "List monitoring rules", NULL);
    register_cmd("rule_clear", cmd_rule_clear, "Clear all rules", NULL);
    register_cmd("auto_interval", cmd_auto_interval, "Set auto-check interval", "auto_interval <count>");
    register_cmd("auto_off", cmd_auto_off, "Disable auto monitoring", NULL);
    register_cmd("prompt", cmd_prompt, "Set system prompt", "prompt <text>");

    // help コマンドは esp_console が自動登録
    esp_console_register_help_command();

    ESP_LOGI(TAG, "CLI started. Type 'help' for available commands.");
    printf("\n");
    printf("===================================\n");
    printf("  SeedClaw - AI GPIO Controller\n");
    printf("===================================\n");
    printf("Type 'help' for available commands\n\n");

    // REPLループ
    static char line[256];
    while (true) {
        printf("seed> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) != NULL) {
            // 改行を削除
            line[strcspn(line, "\r\n")] = 0;

            if (strlen(line) > 0) {
                int ret;
                esp_err_t err = esp_console_run(line, &ret);
                if (err == ESP_ERR_NOT_FOUND) {
                    printf("Unknown command. Type 'help' for available commands.\n");
                } else if (err != ESP_OK) {
                    printf("Command failed: %s\n", esp_err_to_name(err));
                }
            }
        }
    }

    vTaskDelete(NULL);
}

esp_err_t cli_init(void)
{
    xTaskCreate(cli_task, "cli", SEEDCLAW_CLI_STACK, NULL, SEEDCLAW_CLI_PRIO, NULL);
    ESP_LOGI(TAG, "CLI task started");
    return ESP_OK;
}
