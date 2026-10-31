#ifndef READER_UI_H
#define READER_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VIEW_READING = 0,
    VIEW_MENU,
    VIEW_BOOK_LIST,
    VIEW_BLE_SYNC,
} reader_view_t;

typedef enum {
    MENU_ACTION_NONE = 0,
    MENU_ACTION_RESUME,
    MENU_ACTION_BOOK_LIST,
    MENU_ACTION_TOGGLE_FONT,
    MENU_ACTION_BLE_SYNC,
    MENU_ACTION_SLEEP,
} menu_action_t;

esp_err_t reader_ui_init(void);
void      reader_ui_render_reading(void);
void      reader_ui_render_menu(int selected_index);
void      reader_ui_render_book_list(int selected_index);
void      reader_ui_render_ble_sync(bool is_connected, uint32_t bytes_received);
void      reader_ui_render_splash(const char *title, const char *subtitle);

#ifdef __cplusplus
}
#endif

#endif // READER_UI_H
