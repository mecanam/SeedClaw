#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    int raw;          // 0-4095
    int voltage_mv;   // ミリボルト
    int percentage;   // 0-100%
} adc_result_t;

/**
 * @brief GPIO制御モジュールを初期化
 */
esp_err_t gpio_ctrl_init(void);

/**
 * @brief GPIOピンの値を読み取る
 * @return ピン値 (0 or 1)、エラー時は -1
 */
int gpio_ctrl_read(int pin);

/**
 * @brief GPIOピンに値を書き込む
 * @param pin ピン番号
 * @param value 0 (LOW) or 1 (HIGH)
 */
esp_err_t gpio_ctrl_write(int pin, int value);

/**
 * @brief ADC値を読み取る
 * @param pin ピン番号 (GPIO 2, 3, 4 のみ)
 * @param result 結果を格納する構造体
 */
esp_err_t gpio_ctrl_adc_read(int pin, adc_result_t *result);

/**
 * @brief PWM出力を設定
 * @param pin ピン番号
 * @param duty_percent デューティ比 0-100 (%)
 * @param freq_hz 周波数 (Hz)、0以下ならデフォルト1000Hz
 */
esp_err_t gpio_ctrl_pwm_set(int pin, int duty_percent, int freq_hz);

/**
 * @brief 全ピン状態をJSON形式で取得
 * @return JSON文字列 (呼び出し元がfree()する)
 */
char *gpio_ctrl_status_json(void);

/**
 * @brief ピンが操作許可されているか確認
 */
bool gpio_is_pin_allowed(int pin);

/**
 * @brief ピンがADC対応か確認
 */
bool gpio_is_adc_allowed(int pin);
