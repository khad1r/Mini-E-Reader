#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define DISPLAY_WIDTH          200
#define DISPLAY_HEIGHT         200
#define DISPLAY_BUFFER_SIZE    (DISPLAY_WIDTH * DISPLAY_HEIGHT / 4) // 10,000 bytes

// UI Layout Coordinates
#define UI_HEADER_HEIGHT       19
#define UI_FOOTER_Y            184
#define UI_FOOTER_HEIGHT       16
#define UI_BODY_Y_START        22
#define UI_BODY_MAX_HEIGHT     160

// Timeouts
#define UI_INACTIVITY_SLEEP_SEC 60

#endif // APP_CONFIG_H
