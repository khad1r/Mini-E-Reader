#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_ID_BOOT = 0,
    BTN_ID_PWR  = 1,
} btn_id_t;

typedef enum {
    BTN_CLICK = 0,
    BTN_DOUBLE_CLICK,
    BTN_LONG_PRESS,
} btn_press_type_t;

typedef struct {
    btn_id_t         button;
    btn_press_type_t type;
} button_event_t;

// Initializes GPIO 0 (BOOT) and GPIO 18 (PWR) with polling timer & event queue
esp_err_t button_init(QueueHandle_t *out_queue);

// Deinitializes timer and button resources
void button_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // BUTTON_H
