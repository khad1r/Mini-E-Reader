#include "power_mgr.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "POWER_MGR"

#define VBAT_PWR_HOLD_PIN  GPIO_NUM_17
#define BOOT_BUTTON_PIN    GPIO_NUM_0
#define PWR_BUTTON_PIN     GPIO_NUM_18
#define AUDIO_PWR_PIN      GPIO_NUM_42
#define AUDIO_PA_PIN       GPIO_NUM_46
#define LED_GREEN_PIN      GPIO_NUM_3

#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0 // GPIO 1

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static bool                      s_cali_enabled = false;

void power_mgr_early_init(void) {
    // 1. Release any hold from prior deep sleep
    gpio_hold_dis(VBAT_PWR_HOLD_PIN);

    // 2. Configure and assert GPIO 17 HIGH immediately to latch system power
    gpio_config_t pwr_conf = {
        .pin_bit_mask = (1ULL << VBAT_PWR_HOLD_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_conf);
    gpio_set_level(VBAT_PWR_HOLD_PIN, 1);

    // 3. Disable audio amplifier rails to prevent power waste
    gpio_config_t audio_conf = {
        .pin_bit_mask = (1ULL << AUDIO_PWR_PIN) | (1ULL << AUDIO_PA_PIN) | (1ULL << LED_GREEN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&audio_conf);
    gpio_set_level(AUDIO_PWR_PIN, 0);
    gpio_set_level(AUDIO_PA_PIN, 0);
    gpio_set_level(LED_GREEN_PIN, 1); // 1 = OFF for active low LED

    // 4. Initialize ADC for battery monitoring
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    if (adc_oneshot_new_unit(&init_config, &s_adc_handle) == ESP_OK) {
        adc_oneshot_chan_cfg_t chan_config = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = ADC_ATTEN_DB_12,
        };
        adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_config);

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .chan = BATTERY_ADC_CHANNEL,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali_handle) == ESP_OK) {
            s_cali_enabled = true;
        }
#endif
    }

    ESP_LOGI(TAG, "Power manager initialized. Board power latched.");
}

wakeup_reason_t power_mgr_get_wakeup_cause(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        uint64_t status = esp_sleep_get_ext1_wakeup_status();
        if (status & (1ULL << BOOT_BUTTON_PIN)) {
            return WAKEUP_REASON_BOOT_BUTTON;
        } else if (status & (1ULL << PWR_BUTTON_PIN)) {
            return WAKEUP_REASON_PWR_BUTTON;
        }
        return WAKEUP_REASON_OTHER;
    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        return WAKEUP_REASON_TIMER;
    }
    return WAKEUP_REASON_COLD_BOOT;
}

uint32_t power_mgr_get_battery_voltage_mv(void) {
    if (!s_adc_handle) return 3700;
    int raw = 0;
    int mv = 0;
    if (adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw) == ESP_OK) {
        if (s_cali_enabled && s_cali_handle) {
            adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
        } else {
            mv = (raw * 3300) / 4095;
        }
        // Hardware voltage divider: 2x multiplier
        return (uint32_t)(mv * 2);
    }
    return 3700;
}

uint8_t power_mgr_get_battery_percentage(void) {
    uint32_t mv = power_mgr_get_battery_voltage_mv();
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;
    return (uint8_t)((mv - 3300) * 100 / (4200 - 3300));
}

void power_mgr_enter_deep_sleep(uint32_t sleep_seconds) {
    ESP_LOGI(TAG, "Preparing deep sleep (sleep_seconds=%lu)...", sleep_seconds);

    // Disable all wakeup sources first
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // Setup Ext1 wakeup on BOOT (GPIO 0) and PWR (GPIO 18) pins
    const uint64_t ext1_pins = (1ULL << BOOT_BUTTON_PIN) | (1ULL << PWR_BUTTON_PIN);
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(ext1_pins, ESP_EXT1_WAKEUP_ANY_LOW));

    // Configure RTC pull-ups to avoid floating pins
    rtc_gpio_pulldown_dis(BOOT_BUTTON_PIN);
    rtc_gpio_pullup_en(BOOT_BUTTON_PIN);
    rtc_gpio_pulldown_dis(PWR_BUTTON_PIN);
    rtc_gpio_pullup_en(PWR_BUTTON_PIN);

    if (sleep_seconds > 0) {
        esp_sleep_enable_timer_wakeup(((uint64_t)sleep_seconds) * 1000000ULL);
    }

    // Keep GPIO 17 latched high during sleep so battery circuit stays alive
    gpio_hold_en(VBAT_PWR_HOLD_PIN);
    gpio_deep_sleep_hold_en();

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

void power_mgr_power_off(void) {
    ESP_LOGI(TAG, "Hard power off requested. Dropping GPIO 17...");
    gpio_hold_dis(VBAT_PWR_HOLD_PIN);
    gpio_set_level(VBAT_PWR_HOLD_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
}
