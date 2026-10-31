#ifndef POWER_MGR_H
#define POWER_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_sleep.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAKEUP_REASON_COLD_BOOT = 0,
    WAKEUP_REASON_BOOT_BUTTON,
    WAKEUP_REASON_PWR_BUTTON,
    WAKEUP_REASON_TIMER,
    WAKEUP_REASON_OTHER,
} wakeup_reason_t;

// Locks power rail (GPIO 17 HIGH) and disables audio/peripherals
void power_mgr_early_init(void);

// Detects cause of wake up
wakeup_reason_t power_mgr_get_wakeup_cause(void);

// Battery ADC readings
uint32_t power_mgr_get_battery_voltage_mv(void);
uint8_t  power_mgr_get_battery_percentage(void);

// Enters low-power deep sleep (sleep_seconds = 0 for indefinite until button)
void power_mgr_enter_deep_sleep(uint32_t sleep_seconds);

// Hard power off (drops GPIO 17 latch)
void power_mgr_power_off(void);

#ifdef __cplusplus
}
#endif

#endif // POWER_MGR_H
