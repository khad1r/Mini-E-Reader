/**
 * @file ble_sync.h
 * @brief NimBLE GATT Server for Mini-E-Reader Book Synchronization
 */

#ifndef BLE_SYNC_H
#define BLE_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BLE UUID Definitions */
#define BLE_SYNC_SERVICE_UUID_STR        "0000fe60-0000-1000-8000-00805f9b34fb"
#define BLE_SYNC_CHR_CONTROL_UUID_STR    "0000fe61-0000-1000-8000-00805f9b34fb"
#define BLE_SYNC_CHR_DATA_UUID_STR       "0000fe62-0000-1000-8000-00805f9b34fb"
#define BLE_SYNC_CHR_STATUS_UUID_STR     "0000fe63-0000-1000-8000-00805f9b34fb"

#define BLE_SYNC_DEVICE_NAME             "Mini-E-Reader"

/* Control Characteristic OpCodes */
#define BLE_SYNC_OP_LIST_BOOKS           0x01
#define BLE_SYNC_OP_START_UPLOAD         0x02
#define BLE_SYNC_OP_FINISH_UPLOAD        0x03
#define BLE_SYNC_OP_DELETE_BOOK          0x04
#define BLE_SYNC_OP_ABORT_UPLOAD         0x05
/* Control Response Status Codes */
#define BLE_SYNC_STATUS_OK               0x00
#define BLE_SYNC_STATUS_ERROR            0x01

/**
 * @brief Status Characteristic 12-byte packed binary payload layout
 */
typedef struct __attribute__((packed)) {
    uint8_t  battery_pct;   /*!< Battery percentage (0-100) */
    uint8_t  is_charging;   /*!< 1 if charging, 0 otherwise */
    uint16_t battery_mv;    /*!< Battery voltage in millivolts (little-endian) */
    uint32_t free_kb;       /*!< MicroSD free space in kilobytes (little-endian) */
    uint32_t total_kb;      /*!< MicroSD total space in kilobytes (little-endian) */
} ble_sync_status_payload_t;

/**
 * @brief Event types emitted during BLE synchronization
 */
typedef enum {
    BLE_SYNC_EVT_ADVERTISING_STARTED,
    BLE_SYNC_EVT_CONNECTED,
    BLE_SYNC_EVT_DISCONNECTED,
    BLE_SYNC_EVT_UPLOAD_START,
    BLE_SYNC_EVT_UPLOAD_PROGRESS,
    BLE_SYNC_EVT_UPLOAD_COMPLETE,
    BLE_SYNC_EVT_UPLOAD_ABORTED,
    BLE_SYNC_EVT_FILE_DELETED,
} ble_sync_event_t;

typedef void (*ble_sync_event_cb_t)(ble_sync_event_t event, const void *param, void *user_data);

/**
 * @brief Initialize and start the NimBLE GATT server and advertise book sync service.
 *
 * Configures NimBLE stack, registers service 0000fe60-..., sets preferred MTU to 512,
 * and advertises as "Mini-E-Reader".
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t ble_sync_start(void);

/**
 * @brief Stop BLE synchronization.
 *
 * Stops advertising and terminates any active connection, aborting pending transfers.
 */
void ble_sync_stop(void);

/**
 * @brief Check if a BLE client is currently connected.
 *
 * @return true if connected, false otherwise.
 */
bool ble_sync_is_connected(void);

/**
 * @brief Get total bytes received during the current/latest file upload session.
 *
 * @return Number of bytes received.
 */
uint32_t ble_sync_get_bytes_received(void);

/**
 * @brief Manually send a Status Characteristic notification to the connected client.
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t ble_sync_notify_status(void);

/**
 * @brief Register an optional callback for BLE synchronization events.
 *
 * @param cb Callback function.
 * @param user_data User context pointer passed to callback.
 */
void ble_sync_register_callback(ble_sync_event_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif // BLE_SYNC_H
