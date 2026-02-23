#include "gpio_ctrl.h"
#include "seedclaw_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "gpio_ctrl";

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

static pin_state_t s_pin_state[22];
static int s_next_pwm_channel = 0;

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali_handle = NULL;

static bool is_pin_allowed(int pin)
{
    if (pin < 0 || pin >= 22) {
        return false;
    }
    return (SEEDCLAW_GPIO_ALLOWED_MASK & (1ULL << pin)) != 0;
}

static bool is_adc_allowed(int pin)
{
    if (pin < 0 || pin >= 22) {
        return false;
    }
    return (SEEDCLAW_ADC_ALLOWED_MASK & (1ULL << pin)) != 0;
}

bool gpio_is_pin_allowed(int pin)
{
    return is_pin_allowed(pin);
}

bool gpio_is_adc_allowed(int pin)
{
    return is_adc_allowed(pin);
}

esp_err_t gpio_ctrl_init(void)
{
    memset(s_pin_state, 0, sizeof(s_pin_state));
    for (int i = 0; i < 22; i++) {
        s_pin_state[i].mode = PIN_MODE_UNUSED;
        s_pin_state[i].pwm_channel = -1;
    }

    // LEDC タイマー設定
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_10_BIT,
        .freq_hz          = 1000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    // ADC oneshot初期化
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    err = adc_oneshot_new_unit(&init_config, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC oneshot init failed: %s", esp_err_to_name(err));
        return err;
    }

    // ADC calibration (ESP32-C3 uses curve fitting)
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc_cali_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration init failed: %s (continuing without calibration)", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "GPIO control initialized");
    return ESP_OK;
}

int gpio_ctrl_read(int pin)
{
    if (!is_pin_allowed(pin)) {
        ESP_LOGE(TAG, "Pin %d is not allowed", pin);
        return -1;
    }

    // OUTPUT/PWMモードの場合は状態を壊さずそのまま読む
    if (s_pin_state[pin].mode == PIN_MODE_OUTPUT || s_pin_state[pin].mode == PIN_MODE_PWM) {
        int level = gpio_get_level(pin);
        s_pin_state[pin].value = level;
        ESP_LOGI(TAG, "GPIO%d read (output mode): %d", pin, level);
        return level;
    }

    // 未使用またはINPUT以外→INPUTに設定
    if (s_pin_state[pin].mode != PIN_MODE_INPUT) {
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        s_pin_state[pin].mode = PIN_MODE_INPUT;
    }

    int level = gpio_get_level(pin);
    s_pin_state[pin].value = level;
    ESP_LOGI(TAG, "GPIO%d read: %d", pin, level);
    return level;
}

esp_err_t gpio_ctrl_write(int pin, int value)
{
    if (!is_pin_allowed(pin)) {
        ESP_LOGE(TAG, "Pin %d is not allowed", pin);
        return ESP_ERR_INVALID_ARG;
    }

    if (value != 0 && value != 1) {
        ESP_LOGE(TAG, "Invalid value %d (must be 0 or 1)", value);
        return ESP_ERR_INVALID_ARG;
    }

    // PWMモードなら停止
    if (s_pin_state[pin].mode == PIN_MODE_PWM && s_pin_state[pin].pwm_channel >= 0) {
        ledc_stop(LEDC_LOW_SPEED_MODE, s_pin_state[pin].pwm_channel, 0);
    }

    // OUTPUT以外からの切り替え時のみリセット
    if (s_pin_state[pin].mode != PIN_MODE_OUTPUT) {
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);  // 入出力両方有効（読み戻し可能）
    }
    gpio_set_level(pin, value);

    s_pin_state[pin].mode = PIN_MODE_OUTPUT;
    s_pin_state[pin].value = value;

    ESP_LOGI(TAG, "GPIO%d write: %d", pin, value);
    return ESP_OK;
}

esp_err_t gpio_ctrl_adc_read(int pin, adc_result_t *result)
{
    if (!is_adc_allowed(pin)) {
        ESP_LOGE(TAG, "Pin %d is not an ADC pin (only GPIO 2, 3, 4 allowed)", pin);
        return ESP_ERR_INVALID_ARG;
    }

    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // ピン番号をADCチャンネルに変換
    adc_channel_t channel;
    if (pin == 2) {
        channel = ADC_CHANNEL_0;
    } else if (pin == 3) {
        channel = ADC_CHANNEL_1;
    } else if (pin == 4) {
        channel = ADC_CHANNEL_2;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    // チャンネル設定
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    esp_err_t err = adc_oneshot_config_channel(s_adc_handle, channel, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    // ADC読み取り
    int raw;
    err = adc_oneshot_read(s_adc_handle, channel, &raw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(err));
        return err;
    }

    result->raw = raw;

    // 電圧変換
    if (s_adc_cali_handle != NULL) {
        int voltage;
        err = adc_cali_raw_to_voltage(s_adc_cali_handle, raw, &voltage);
        if (err == ESP_OK) {
            result->voltage_mv = voltage;
        } else {
            result->voltage_mv = 0;
        }
    } else {
        result->voltage_mv = 0;
    }

    // パーセンテージ計算 (0-4095 → 0-100%)
    result->percentage = (raw * 100) / 4095;

    s_pin_state[pin].mode = PIN_MODE_ADC;
    s_pin_state[pin].value = raw;

    ESP_LOGI(TAG, "ADC GPIO%d read: raw=%d, voltage=%dmV, %%=%d",
             pin, result->raw, result->voltage_mv, result->percentage);

    return ESP_OK;
}

esp_err_t gpio_ctrl_pwm_set(int pin, int duty_percent, int freq_hz)
{
    if (!is_pin_allowed(pin)) {
        ESP_LOGE(TAG, "Pin %d is not allowed", pin);
        return ESP_ERR_INVALID_ARG;
    }

    // デューティ比をクランプ
    if (duty_percent < 0) duty_percent = 0;
    if (duty_percent > 100) duty_percent = 100;

    // 周波数デフォルト
    if (freq_hz <= 0) freq_hz = 1000;

    // チャンネル割り当て
    if (s_pin_state[pin].pwm_channel < 0) {
        if (s_next_pwm_channel >= SEEDCLAW_PWM_MAX_CHANNELS) {
            ESP_LOGE(TAG, "No more PWM channels available");
            return ESP_ERR_NO_MEM;
        }

        ledc_channel_config_t ledc_channel = {
            .speed_mode     = LEDC_LOW_SPEED_MODE,
            .channel        = s_next_pwm_channel,
            .timer_sel      = LEDC_TIMER_0,
            .intr_type      = LEDC_INTR_DISABLE,
            .gpio_num       = pin,
            .duty           = 0,
            .hpoint         = 0
        };
        esp_err_t err = ledc_channel_config(&ledc_channel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(err));
            return err;
        }

        s_pin_state[pin].pwm_channel = s_next_pwm_channel;
        s_next_pwm_channel++;
    }

    int channel = s_pin_state[pin].pwm_channel;

    // 周波数設定
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz);

    // デューティ設定 (10bit = 0-1023)
    uint32_t duty = (duty_percent * 1023) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);

    s_pin_state[pin].mode = PIN_MODE_PWM;
    s_pin_state[pin].pwm_duty = duty_percent;
    s_pin_state[pin].pwm_freq = freq_hz;

    ESP_LOGI(TAG, "PWM GPIO%d: duty=%d%%, freq=%dHz", pin, duty_percent, freq_hz);
    return ESP_OK;
}

char *gpio_ctrl_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *pins = cJSON_CreateArray();

    for (int i = 0; i < 22; i++) {
        if (s_pin_state[i].mode == PIN_MODE_UNUSED) {
            continue;
        }

        cJSON *pin_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(pin_obj, "pin", i);

        // ピンラベル
        const char *label = "";
        switch (i) {
            case 2:  label = "D0/A0"; break;
            case 3:  label = "D1/A1"; break;
            case 4:  label = "D2/A2"; break;
            case 5:  label = "D3"; break;
            case 6:  label = "D4"; break;
            case 7:  label = "D5"; break;
            case 8:  label = "D8"; break;
            case 10: label = "D10"; break;
            case 20: label = "D7"; break;
            case 21: label = "D6"; break;
        }
        cJSON_AddStringToObject(pin_obj, "label", label);

        // モード
        const char *mode_str = "";
        switch (s_pin_state[i].mode) {
            case PIN_MODE_INPUT:  mode_str = "INPUT"; break;
            case PIN_MODE_OUTPUT: mode_str = "OUTPUT"; break;
            case PIN_MODE_ADC:    mode_str = "ADC"; break;
            case PIN_MODE_PWM:    mode_str = "PWM"; break;
            default: mode_str = "UNUSED"; break;
        }
        cJSON_AddStringToObject(pin_obj, "mode", mode_str);

        // 値
        if (s_pin_state[i].mode == PIN_MODE_INPUT || s_pin_state[i].mode == PIN_MODE_OUTPUT) {
            cJSON_AddNumberToObject(pin_obj, "value", s_pin_state[i].value);
        } else if (s_pin_state[i].mode == PIN_MODE_ADC) {
            cJSON_AddNumberToObject(pin_obj, "raw", s_pin_state[i].value);
        } else if (s_pin_state[i].mode == PIN_MODE_PWM) {
            cJSON_AddNumberToObject(pin_obj, "duty", s_pin_state[i].pwm_duty);
            cJSON_AddNumberToObject(pin_obj, "freq", s_pin_state[i].pwm_freq);
        }

        cJSON_AddItemToArray(pins, pin_obj);
    }

    cJSON_AddItemToObject(root, "pins", pins);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}
