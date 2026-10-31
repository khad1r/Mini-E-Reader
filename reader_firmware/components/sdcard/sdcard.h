#ifndef SDCARD_H
#define SDCARD_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SDCARD_MOUNT_POINT "/sdcard"

esp_err_t sdcard_init(void);
void      sdcard_deinit(void);
bool      sdcard_is_mounted(void);
uint64_t  sdcard_get_total_bytes(void);
uint64_t  sdcard_get_free_bytes(void);

#ifdef __cplusplus
}
#endif

#endif // SDCARD_H
