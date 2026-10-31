#include "button.h"
#include "multi_button.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#define TAG "BUTTON"

#define BOOT_BUTTON_GPIO  GPIO_NUM_0
#define PWR_BUTTON_GPIO   GPIO_NUM_18

static Button s_btn_boot;
static Button s_btn_pwr;
static esp_timer_handle_t s_timer = NULL;
static QueueHandle_t s_event_queue = NULL;

static uint8_t read_button_gpio(uint8_t button_id) {
    if (button_id == BTN_ID_BOOT) {
        return gpio_get_level(BOOT_BUTTON_GPIO);
    } else if (button_id == BTN_ID_PWR) {
        return gpio_get_level(PWR_BUTTON_GPIO);
    }
    return 1;
}

static void send_event(btn_id_t btn, btn_press_type_t type) {
    if (!s_event_queue) return;
    button_event_t evt = { .button = btn, .type = type };
    xQueueSend(s_event_queue, &evt, 0);
}

// BOOT callbacks
static void on_boot_click(Button *b) { send_event(BTN_ID_BOOT, BTN_CLICK); }
static void on_boot_double_click(Button *b) { send_event(BTN_ID_BOOT, BTN_DOUBLE_CLICK); }
static void on_boot_long_press(Button *b) { send_event(BTN_ID_BOOT, BTN_LONG_PRESS); }

// PWR callbacks
static void on_pwr_click(Button *b) { send_event(BTN_ID_PWR, BTN_CLICK); }
static void on_pwr_double_click(Button *b) { send_event(BTN_ID_PWR, BTN_DOUBLE_CLICK); }
static void on_pwr_long_press(Button *b) { send_event(BTN_ID_PWR, BTN_LONG_PRESS); }

static void button_timer_cb(void *arg) {
    button_ticks();
}

esp_err_t button_init(QueueHandle_t *out_queue) {
    s_event_queue = xQueueCreate(16, sizeof(button_event_t));
    if (!s_event_queue) return ESP_ERR_NO_MEM;
    if (out_queue) *out_queue = s_event_queue;

    gpio_config_t conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO) | (1ULL << PWR_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&conf));

    button_init_struct(&s_btn_boot, read_button_gpio, 0, BTN_ID_BOOT);
    button_attach(&s_btn_boot, SINGLE_CLICK, on_boot_click);
    button_attach(&s_btn_boot, DOUBLE_CLICK, on_boot_double_click);
    button_attach(&s_btn_boot, LONG_PRESS_START, on_boot_long_press);
    button_start(&s_btn_boot);

    button_init_struct(&s_btn_pwr, read_button_gpio, 0, BTN_ID_PWR);
    button_attach(&s_btn_pwr, SINGLE_CLICK, on_pwr_click);
    button_attach(&s_btn_pwr, DOUBLE_CLICK, on_pwr_double_click);
    button_attach(&s_btn_pwr, LONG_PRESS_START, on_pwr_long_press);
    button_start(&s_btn_pwr);

    esp_timer_create_args_t timer_args = {
        .callback = button_timer_cb,
        .name = "btn_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, 20 * 1000)); // 20ms

    ESP_LOGI(TAG, "Buttons initialized (BOOT: GPIO 0, PWR: GPIO 18)");
    return ESP_OK;
}

void button_deinit(void) {
    if (s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_delete(s_timer);
        s_timer = NULL;
    }
    button_stop(&s_btn_boot);
    button_stop(&s_btn_pwr);
    if (s_event_queue) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
}
