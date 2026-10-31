/**
 * @file ble_sync.c
 * @brief NimBLE GATT Server Implementation for Mini-E-Reader Book Synchronization
 */

#include "ble_sync.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#if __has_include("esp_nimble_hci.h") && (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0))
#include "esp_nimble_hci.h"
#endif

#include "power_mgr.h"
#include "sdcard.h"

static const char *TAG = "ble_sync";

#define BOOKS_DIR_PATH   "/sdcard/books"
#define RINGBUF_SIZE     (64 * 1024)
#define CMD_QUEUE_LEN    16
#define MAX_LIST_ENTRIES 256

/* 128-bit Service and Characteristic UUIDs (stored little-endian in NimBLE) */
/* Service: 0000fe60-0000-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_svc_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
               0x00, 0x10, 0x00, 0x00, 0x60, 0xfe, 0x00, 0x00 }
};

/* Control Characteristic: 0000fe61-0000-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_chr_control_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
               0x00, 0x10, 0x00, 0x00, 0x61, 0xfe, 0x00, 0x00 }
};

/* Data Characteristic: 0000fe62-0000-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_chr_data_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
               0x00, 0x10, 0x00, 0x00, 0x62, 0xfe, 0x00, 0x00 }
};

/* Status Characteristic: 0000fe63-0000-1000-8000-00805f9b34fb */
static const ble_uuid128_t s_chr_status_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
               0x00, 0x10, 0x00, 0x00, 0x63, 0xfe, 0x00, 0x00 }
};

/* Characteristic Value Handles */
static uint16_t s_control_val_handle;
static uint16_t s_data_val_handle;
static uint16_t s_status_val_handle;

/* BLE Connection State */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool     s_is_connected = false;
static bool     s_sync_running = false;
static uint16_t s_current_mtu = 23;

/* File Upload State */
static FILE    *s_upload_fp = NULL;
static bool     s_is_uploading = false;
static bool     s_upload_error = false;
static uint32_t s_bytes_received = 0;
static uint32_t s_expected_file_size = 0;
static uint16_t s_expected_seq = 0;
static char     s_upload_filename[128] = { 0 };

/* Ringbuffer & Worker Task */
static RingbufHandle_t s_data_ringbuf = NULL;
static QueueHandle_t   s_cmd_queue = NULL;
static TaskHandle_t    s_worker_task_hdl = NULL;

typedef enum {
    CMD_LIST_BOOKS,
    CMD_START_UPLOAD,
    CMD_FINISH_UPLOAD,
    CMD_DELETE_BOOK,
    CMD_ABORT_UPLOAD,
} ble_sync_cmd_type_t;

typedef struct {
    ble_sync_cmd_type_t type;
    char filename[128];
    uint32_t file_size;
} ble_sync_cmd_msg_t;

typedef struct {
    char name[128];
    uint32_t size;
} book_entry_t;

/* Optional Event Callback */
static ble_sync_event_cb_t s_event_cb = NULL;
static void               *s_event_cb_arg = NULL;

/* Forward Declarations */
static void ble_sync_advertise(void);
static int ble_sync_gap_event_cb(struct ble_gap_event *event, void *arg);
static int ble_sync_control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_sync_data_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_sync_status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg);
static esp_err_t ble_sync_notify(uint16_t val_handle, const void *data, uint16_t len);

/* GATT Service Definitions */
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* Control Characteristic (0xFE61): Write, Notify */
                .uuid = &s_chr_control_uuid.u,
                .access_cb = ble_sync_control_access_cb,
                .val_handle = &s_control_val_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                /* Data Characteristic (0xFE62): Write, Write Without Response */
                .uuid = &s_chr_data_uuid.u,
                .access_cb = ble_sync_data_access_cb,
                .val_handle = &s_data_val_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* Status Characteristic (0xFE63): Read, Notify */
                .uuid = &s_chr_status_uuid.u,
                .access_cb = ble_sync_status_access_cb,
                .val_handle = &s_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                0, /* End of characteristics */
            }
        },
    },
    {
        0, /* End of services */
    },
};

/* --- Helpers --- */

static void sanitize_filename(const char *in, char *out, size_t max_len)
{
    if (!in || !out || max_len == 0) return;

    // Strip leading path components
    const char *p = strrchr(in, '/');
    if (p) in = p + 1;
    p = strrchr(in, '\\');
    if (p) in = p + 1;

    // Strip leading dots to prevent hidden or traversal files
    while (*in == '.') in++;

    size_t i = 0;
    while (*in && i < max_len - 1) {
        char c = *in++;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) {
            c = '_';
        }
        out[i++] = c;
    }
    out[i] = '\0';

    if (i == 0) {
        strncpy(out, "unnamed.txt", max_len - 1);
        out[max_len - 1] = '\0';
    }
}

static void ensure_books_dir(void)
{
    struct stat st;
    if (stat(BOOKS_DIR_PATH, &st) != 0) {
        mkdir(BOOKS_DIR_PATH, 0755);
    }
}

static void ble_sync_get_status_payload(ble_sync_status_payload_t *payload)
{
    memset(payload, 0, sizeof(*payload));
    payload->battery_pct = power_mgr_get_battery_percentage();
    uint32_t mv = power_mgr_get_battery_voltage_mv();
    payload->battery_mv = (uint16_t)(mv > 0xFFFF ? 0xFFFF : mv);
    payload->is_charging = (mv >= 4250) ? 1 : 0;

    uint64_t free_bytes = sdcard_get_free_bytes();
    uint64_t total_bytes = sdcard_get_total_bytes();
    payload->free_kb = (uint32_t)(free_bytes / 1024ULL);
    payload->total_kb = (uint32_t)(total_bytes / 1024ULL);
}

static esp_err_t ble_sync_notify(uint16_t val_handle, const void *data, uint16_t len)
{
    if (!s_is_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        ESP_LOGE(TAG, "Failed to allocate os_mbuf for notification");
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, val_handle, om);
    if (rc != 0) {
        if (rc == BLE_HS_EDISABLED) {
            ESP_LOGD(TAG, "Notifications disabled by client for handle %u", val_handle);
        } else {
            ESP_LOGW(TAG, "ble_gatts_notify_custom failed: rc=%d", rc);
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

static int book_entry_cmp(const void *a, const void *b)
{
    const book_entry_t *ea = (const book_entry_t *)a;
    const book_entry_t *eb = (const book_entry_t *)b;
    return strcasecmp(ea->name, eb->name);
}

/* --- Worker Command Handlers --- */

static void handle_cmd_list_books(void)
{
    ESP_LOGI(TAG, "Listing books in %s", BOOKS_DIR_PATH);
    ensure_books_dir();

    DIR *dir = opendir(BOOKS_DIR_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory %s", BOOKS_DIR_PATH);
        uint8_t err_resp[8] = { BLE_SYNC_OP_LIST_BOOKS, BLE_SYNC_STATUS_ERROR, 1, 0, 0, 0, 0, 0 };
        ble_sync_notify(s_control_val_handle, err_resp, sizeof(err_resp));
        return;
    }

    book_entry_t *entries = (book_entry_t *)calloc(MAX_LIST_ENTRIES, sizeof(book_entry_t));
    size_t count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue; // skip hidden files

        size_t nlen = strlen(entry->d_name);
        if (nlen < 4) continue;
        if (strcasecmp(entry->d_name + nlen - 4, ".txt") != 0) continue;

        char fpath[256];
        snprintf(fpath, sizeof(fpath), "%s/%s", BOOKS_DIR_PATH, entry->d_name);

        struct stat st;
        if (stat(fpath, &st) == 0 && S_ISREG(st.st_mode)) {
            if (entries && count < MAX_LIST_ENTRIES) {
                strncpy(entries[count].name, entry->d_name, sizeof(entries[count].name) - 1);
                entries[count].size = (uint32_t)st.st_size;
                count++;
            }
        }
    }
    closedir(dir);

    if (count == 0) {
        // Empty list notification
        uint8_t empty_resp[8] = { BLE_SYNC_OP_LIST_BOOKS, BLE_SYNC_STATUS_OK, 1, 0, 0, 0, 0, 0 };
        ble_sync_notify(s_control_val_handle, empty_resp, sizeof(empty_resp));
    } else {
        if (entries) {
            qsort(entries, count, sizeof(book_entry_t), book_entry_cmp);
            for (size_t i = 0; i < count; i++) {
                uint8_t is_last = (i == count - 1) ? 1 : 0;
                uint8_t resp[256];
                resp[0] = BLE_SYNC_OP_LIST_BOOKS;
                resp[1] = BLE_SYNC_STATUS_OK;
                resp[2] = is_last;
                uint32_t fsize = entries[i].size;
                resp[3] = (uint8_t)(fsize & 0xFF);
                resp[4] = (uint8_t)((fsize >> 8) & 0xFF);
                resp[5] = (uint8_t)((fsize >> 16) & 0xFF);
                resp[6] = (uint8_t)((fsize >> 24) & 0xFF);
                size_t name_len = strlen(entries[i].name);
                if (name_len > sizeof(resp) - 8) {
                    name_len = sizeof(resp) - 8;
                }
                memcpy(&resp[7], entries[i].name, name_len);
                resp[7 + name_len] = '\0';

                ble_sync_notify(s_control_val_handle, resp, 7 + name_len + 1);
                vTaskDelay(pdMS_TO_TICKS(15));
            }
        }
    }

    if (entries) {
        free(entries);
    }
}

static void handle_cmd_start_upload(const ble_sync_cmd_msg_t *cmd)
{
    ESP_LOGI(TAG, "Start upload: name='%s', size=%u", cmd->filename, (unsigned)cmd->file_size);

    if (s_upload_fp != NULL) {
        fclose(s_upload_fp);
        s_upload_fp = NULL;
    }

    // Flush leftover data in ringbuffer if any
    if (s_data_ringbuf != NULL) {
        size_t dummy_sz = 0;
        void *dummy = NULL;
        while ((dummy = xRingbufferReceiveUpTo(s_data_ringbuf, &dummy_sz, 0, 4096)) != NULL) {
            vRingbufferReturnItem(s_data_ringbuf, dummy);
        }
    }

    sanitize_filename(cmd->filename, s_upload_filename, sizeof(s_upload_filename));
    s_expected_file_size = cmd->file_size;
    s_bytes_received = 0;
    s_expected_seq = 0;
    s_upload_error = false;

    ensure_books_dir();

    char fpath[256];
    snprintf(fpath, sizeof(fpath), "%s/%s", BOOKS_DIR_PATH, s_upload_filename);

    s_upload_fp = fopen(fpath, "wb");
    uint8_t status = BLE_SYNC_STATUS_OK;
    if (!s_upload_fp) {
        ESP_LOGE(TAG, "Failed to open '%s' for write", fpath);
        status = BLE_SYNC_STATUS_ERROR;
        s_is_uploading = false;
    } else {
        s_is_uploading = true;
        ESP_LOGI(TAG, "Upload target file opened: %s", fpath);
    }

    uint8_t resp[2] = { BLE_SYNC_OP_START_UPLOAD, status };
    ble_sync_notify(s_control_val_handle, resp, sizeof(resp));

    if (status == BLE_SYNC_STATUS_OK && s_event_cb) {
        s_event_cb(BLE_SYNC_EVT_UPLOAD_START, s_upload_filename, s_event_cb_arg);
    }
}

static void handle_cmd_finish_upload(void)
{
    ESP_LOGI(TAG, "Finish upload requested");

    // Drain all remaining data from the ringbuffer into the file
    if (s_data_ringbuf != NULL && s_upload_fp != NULL) {
        size_t item_sz = 0;
        uint8_t *item = NULL;
        while ((item = (uint8_t *)xRingbufferReceiveUpTo(s_data_ringbuf, &item_sz, 0, 4096)) != NULL) {
            if (fwrite(item, 1, item_sz, s_upload_fp) != item_sz) {
                ESP_LOGE(TAG, "fwrite failed while draining ringbuffer");
                s_upload_error = true;
            }
            vRingbufferReturnItem(s_data_ringbuf, item);
        }
    }

    if (s_upload_fp != NULL) {
        fflush(s_upload_fp);
        fsync(fileno(s_upload_fp));
        fclose(s_upload_fp);
        s_upload_fp = NULL;
    }

    s_is_uploading = false;

    if (s_expected_file_size > 0 && s_bytes_received != s_expected_file_size) {
        ESP_LOGE(TAG, "Upload size mismatch: got %u, expected %u",
                 (unsigned)s_bytes_received, (unsigned)s_expected_file_size);
        s_upload_error = true;
    }

    uint8_t status = s_upload_error ? BLE_SYNC_STATUS_ERROR : BLE_SYNC_STATUS_OK;
    if (s_upload_error && s_upload_filename[0] != '\0') {
        char fpath[256];
        snprintf(fpath, sizeof(fpath), "%s/%s", BOOKS_DIR_PATH, s_upload_filename);
        unlink(fpath);
        ESP_LOGW(TAG, "Deleted corrupted/incomplete file on error: %s", fpath);
    }

    uint8_t resp[2] = { BLE_SYNC_OP_FINISH_UPLOAD, status };

    ESP_LOGI(TAG, "Upload finished: %u bytes received, status=%d",
             (unsigned)s_bytes_received, status);

    if (s_event_cb) {
        s_event_cb(BLE_SYNC_EVT_UPLOAD_COMPLETE, s_upload_filename, s_event_cb_arg);
    }
}

static void handle_cmd_delete_book(const ble_sync_cmd_msg_t *cmd)
{
    ESP_LOGI(TAG, "Delete book requested: '%s'", cmd->filename);

    char safe_name[128];
    sanitize_filename(cmd->filename, safe_name, sizeof(safe_name));

    if (s_upload_fp && strcmp(s_upload_filename, safe_name) == 0) {
        fclose(s_upload_fp);
        s_upload_fp = NULL;
        s_is_uploading = false;
    }

    char fpath[256];
    snprintf(fpath, sizeof(fpath), "%s/%s", BOOKS_DIR_PATH, safe_name);

    int rc = unlink(fpath);
    uint8_t status = (rc == 0) ? BLE_SYNC_STATUS_OK : BLE_SYNC_STATUS_ERROR;

    if (rc == 0) {
        ESP_LOGI(TAG, "Deleted file: %s", fpath);
    } else {
        ESP_LOGW(TAG, "Failed to delete file: %s", fpath);
    }

    uint8_t resp[2] = { BLE_SYNC_OP_DELETE_BOOK, status };
    ble_sync_notify(s_control_val_handle, resp, sizeof(resp));

    if (rc == 0 && s_event_cb) {
        s_event_cb(BLE_SYNC_EVT_FILE_DELETED, safe_name, s_event_cb_arg);
    }
}

static void handle_cmd_abort_upload(void)
{
    ESP_LOGW(TAG, "Aborting file upload");

    if (s_upload_fp != NULL) {
        fclose(s_upload_fp);
        s_upload_fp = NULL;
    }
    s_is_uploading = false;

    if (s_upload_filename[0] != '\0') {
        char fpath[256];
        snprintf(fpath, sizeof(fpath), "%s/%s", BOOKS_DIR_PATH, s_upload_filename);
        unlink(fpath);
        ESP_LOGW(TAG, "Deleted partial file on abort: %s", fpath);
        s_upload_filename[0] = '\0';
    }

    if (s_data_ringbuf != NULL) {
        size_t dummy_sz = 0;
        void *dummy = NULL;
        while ((dummy = xRingbufferReceiveUpTo(s_data_ringbuf, &dummy_sz, 0, 4096)) != NULL) {
            vRingbufferReturnItem(s_data_ringbuf, dummy);
        }
    }

    s_bytes_received = 0;
    s_expected_file_size = 0;
    s_expected_seq = 0;
    s_upload_error = false;

    uint8_t resp[2] = { BLE_SYNC_OP_ABORT_UPLOAD, BLE_SYNC_STATUS_OK };
    ble_sync_notify(s_control_val_handle, resp, sizeof(resp));

    if (s_event_cb) {
        s_event_cb(BLE_SYNC_EVT_UPLOAD_ABORTED, NULL, s_event_cb_arg);
    }
}

static void ble_sync_worker_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Worker task started");
    ble_sync_cmd_msg_t cmd;

    while (s_sync_running) {
        if (!s_is_uploading) {
            // Idle state: wait for command
            if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
                switch (cmd.type) {
                case CMD_LIST_BOOKS:
                    handle_cmd_list_books();
                    break;
                case CMD_START_UPLOAD:
                    handle_cmd_start_upload(&cmd);
                    break;
                case CMD_DELETE_BOOK:
                    handle_cmd_delete_book(&cmd);
                    break;
                case CMD_FINISH_UPLOAD:
                    handle_cmd_finish_upload();
                    break;
                case CMD_ABORT_UPLOAD:
                    handle_cmd_abort_upload();
                    break;
                default:
                    break;
                }
            }
        } else {
            // Uploading state: check for commands first
            if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
                switch (cmd.type) {
                case CMD_FINISH_UPLOAD:
                    handle_cmd_finish_upload();
                    continue;
                case CMD_ABORT_UPLOAD:
                    handle_cmd_abort_upload();
                    continue;
                case CMD_START_UPLOAD:
                    handle_cmd_start_upload(&cmd);
                    continue;
                default:
                    break;
                }
            }

            // Stream data from ringbuffer into open file
            if (s_data_ringbuf != NULL && s_upload_fp != NULL) {
                size_t item_sz = 0;
                uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(s_data_ringbuf, &item_sz,
                                                                 pdMS_TO_TICKS(20), 4096);
                if (item != NULL) {
                    size_t written = fwrite(item, 1, item_sz, s_upload_fp);
                    if (written != item_sz) {
                        ESP_LOGE(TAG, "fwrite failed: written %u != %u",
                                 (unsigned)written, (unsigned)item_sz);
                        s_upload_error = true;
                    }
                    vRingbufferReturnItem(s_data_ringbuf, item);

                    if (s_event_cb) {
                        s_event_cb(BLE_SYNC_EVT_UPLOAD_PROGRESS,
                                   (void *)(uintptr_t)s_bytes_received,
                                   s_event_cb_arg);
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    ESP_LOGI(TAG, "Worker task terminating");
    if (s_upload_fp != NULL) {
        fclose(s_upload_fp);
        s_upload_fp = NULL;
    }
    s_worker_task_hdl = NULL;
    vTaskDelete(NULL);
}

/* --- GATT Callbacks --- */

static int ble_sync_control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t buf[256];
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &copied);
    if (rc != 0 || copied == 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    buf[copied] = '\0';

    uint8_t opcode = buf[0];
    ESP_LOGI(TAG, "Control write: opcode=0x%02x, len=%u", opcode, copied);

    ble_sync_cmd_msg_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    switch (opcode) {
    case BLE_SYNC_OP_LIST_BOOKS:
        cmd.type = CMD_LIST_BOOKS;
        xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50));
        break;

    case BLE_SYNC_OP_START_UPLOAD:
        if (copied < 6) {
            ESP_LOGE(TAG, "START_UPLOAD payload too short: %u bytes", copied);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        cmd.type = CMD_START_UPLOAD;
        cmd.file_size = (uint32_t)buf[1] |
                        ((uint32_t)buf[2] << 8) |
                        ((uint32_t)buf[3] << 16) |
                        ((uint32_t)buf[4] << 24);
        strncpy(cmd.filename, (char *)&buf[5], sizeof(cmd.filename) - 1);
        cmd.filename[sizeof(cmd.filename) - 1] = '\0';
        xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50));
        break;

    case BLE_SYNC_OP_FINISH_UPLOAD:
        cmd.type = CMD_FINISH_UPLOAD;
        xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50));
        break;

    case BLE_SYNC_OP_DELETE_BOOK:
        if (copied < 2) {
            ESP_LOGE(TAG, "DELETE_BOOK payload too short: %u bytes", copied);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        cmd.type = CMD_DELETE_BOOK;
        strncpy(cmd.filename, (char *)&buf[1], sizeof(cmd.filename) - 1);
        cmd.filename[sizeof(cmd.filename) - 1] = '\0';
        xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50));
        break;
    case BLE_SYNC_OP_ABORT_UPLOAD:
        cmd.type = CMD_ABORT_UPLOAD;
        xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50));
        break;


    default:
        ESP_LOGW(TAG, "Unknown control opcode: 0x%02x", opcode);
        return BLE_ATT_ERR_UNLIKELY;
    }

    return 0;
}

static int ble_sync_data_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t total_len = OS_MBUF_PKTLEN(ctxt->om);
    if (total_len < 2) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (!s_is_uploading || s_data_ringbuf == NULL) {
        ESP_LOGW(TAG, "Data packet received while not uploading");
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t chunk_buf[512];
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, chunk_buf, sizeof(chunk_buf), &copied);
    if (rc != 0 || copied < 2) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t seq = (uint16_t)chunk_buf[0] | ((uint16_t)chunk_buf[1] << 8);
    const uint8_t *payload = &chunk_buf[2];
    size_t payload_len = copied - 2;

    if (seq != s_expected_seq) {
        ESP_LOGE(TAG, "Seq mismatch: expected %u, got %u", s_expected_seq, seq);
        s_upload_error = true;
    }
    s_expected_seq = seq + 1;

    if (payload_len > 0) {
        BaseType_t sent = xRingbufferSend(s_data_ringbuf, payload, payload_len, pdMS_TO_TICKS(100));
        if (sent != pdTRUE) {
            ESP_LOGE(TAG, "Ringbuffer full, dropped %u bytes", (unsigned)payload_len);
            s_upload_error = true;
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        s_bytes_received += payload_len;
    }

    return 0;
}

static int ble_sync_status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        ble_sync_status_payload_t status;
        ble_sync_get_status_payload(&status);
        int rc = os_mbuf_append(ctxt->om, &status, sizeof(status));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* --- GAP Events & Advertising --- */

static int ble_sync_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);

        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_is_connected = true;

            // Set preferred MTU to 512 and initiate MTU exchange
            ble_att_set_preferred_mtu(512);
            int rc_mtu = ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
            if (rc_mtu != 0) {
                ESP_LOGW(TAG, "ble_gattc_exchange_mtu failed: %d", rc_mtu);
            }
            // Request optimal connection interval (15ms - 30ms) for high throughput
            struct ble_gap_upd_params params = {
                .itvl_min = 12,  // 15 ms (12 * 1.25)
                .itvl_max = 24,  // 30 ms (24 * 1.25)
                .latency  = 0,
                .supervision_timeout = 400, // 4000 ms
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            ble_gap_update_params(s_conn_handle, &params);

            if (s_event_cb) {
                s_event_cb(BLE_SYNC_EVT_CONNECTED, NULL, s_event_cb_arg);
            }
        } else {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_is_connected = false;
            if (s_sync_running) {
                ble_sync_advertise();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected; reason=%d", event->disconnect.reason);
        s_is_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

        if (s_is_uploading) {
            ble_sync_cmd_msg_t cmd = { .type = CMD_ABORT_UPLOAD };
            xQueueSend(s_cmd_queue, &cmd, 0);
        }

        if (s_event_cb) {
            s_event_cb(BLE_SYNC_EVT_DISCONNECTED, NULL, s_event_cb_arg);
        }

        if (s_sync_running) {
            ble_sync_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU updated: conn_handle=%d, mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        s_current_mtu = event->mtu.value;
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "BLE subscribe event: attr_handle=%d, cur_notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == s_status_val_handle && event->subscribe.cur_notify) {
            ble_sync_notify_status();
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising complete; reason=%d", event->adv_complete.reason);
        if (s_sync_running && !s_is_connected) {
            ble_sync_advertise();
        }
        return 0;

    default:
        return 0;
    }
}

static void ble_sync_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    int rc;

    // Primary advertisement data
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set adv fields: rc=%d", rc);
        return;
    }

    // Scan response data (complete device name)
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    const char *dev_name = ble_svc_gap_device_name();
    if (!dev_name) {
        dev_name = BLE_SYNC_DEVICE_NAME;
    }
    rsp_fields.name = (uint8_t *)dev_name;
    rsp_fields.name_len = strlen(dev_name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set scan rsp fields: rc=%d", rc);
        return;
    }

    // Start advertising
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to determine own address type: rc=%d", rc);
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_sync_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising: rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE advertising started as '%s'", dev_name);
    if (s_event_cb) {
        s_event_cb(BLE_SYNC_EVT_ADVERTISING_STARTED, NULL, s_event_cb_arg);
    }
}

static void ble_sync_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced");
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
    }
    ble_sync_advertise();
}

static void ble_sync_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset; reason=%d", reason);
}

static void ble_sync_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];
    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGD(TAG, "Registered service %s with handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                 ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGD(TAG, "Registered char %s def_handle=%d val_handle=%d",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                 ctxt->chr.def_handle,
                 ctxt->chr.val_handle);
        break;
    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGD(TAG, "Registered desc %s handle=%d",
                 ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                 ctxt->dsc.handle);
        break;
    default:
        break;
    }
}

static void ble_sync_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task running");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* --- Public API --- */

esp_err_t ble_sync_start(void)
{
    if (s_sync_running) {
        ESP_LOGI(TAG, "BLE sync already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting BLE sync component");

    // Initialize NVS flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %d", ret);
        return ret;
    }

    // Create command queue if needed
    if (s_cmd_queue == NULL) {
        s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(ble_sync_cmd_msg_t));
        if (s_cmd_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create command queue");
            return ESP_ERR_NO_MEM;
        }
    }

    // Create data ringbuffer if needed
    if (s_data_ringbuf == NULL) {
        s_data_ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
        if (s_data_ringbuf == NULL) {
            ESP_LOGE(TAG, "Failed to create ringbuffer");
            return ESP_ERR_NO_MEM;
        }
    }

    s_sync_running = true;

    // Start worker task if needed
    if (s_worker_task_hdl == NULL) {
        BaseType_t task_ret = xTaskCreate(ble_sync_worker_task,
                                          "ble_sync_wrk",
                                          4096,
                                          NULL,
                                          5,
                                          &s_worker_task_hdl);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create worker task");
            s_sync_running = false;
            return ESP_ERR_NO_MEM;
        }
    }

    // Initialize NimBLE host stack
    static bool s_nimble_inited = false;
    if (!s_nimble_inited) {
#if __has_include("esp_nimble_hci.h") && (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0))
        ret = esp_nimble_hci_and_controller_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_nimble_hci_and_controller_init failed: %d", ret);
            return ret;
        }
#endif
        ret = nimble_port_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
            return ret;
        }

        ble_hs_cfg.reset_cb = ble_sync_on_reset;
        ble_hs_cfg.sync_cb = ble_sync_on_sync;
        ble_hs_cfg.gatts_register_cb = ble_sync_gatts_register_cb;
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

        ble_svc_gap_init();
        ble_svc_gatt_init();

        int rc = ble_gatts_count_cfg(s_gatt_svcs);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
            return ESP_FAIL;
        }

        rc = ble_gatts_add_svcs(s_gatt_svcs);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
            return ESP_FAIL;
        }

        rc = ble_svc_gap_device_name_set(BLE_SYNC_DEVICE_NAME);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
            return ESP_FAIL;
        }

        nimble_port_freertos_init(ble_sync_host_task);
        s_nimble_inited = true;
    } else {
        ble_sync_advertise();
    }

    return ESP_OK;
}

void ble_sync_stop(void)
{
    if (!s_sync_running) {
        return;
    }
    ESP_LOGI(TAG, "Stopping BLE sync");
    s_sync_running = false;

    ble_gap_adv_stop();

    if (s_is_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_is_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    if (s_is_uploading || s_upload_fp != NULL) {
        if (s_upload_fp != NULL) {
            fclose(s_upload_fp);
            s_upload_fp = NULL;
        }
        if (s_upload_filename[0] != '\0') {
            char fpath[256];
            snprintf(fpath, sizeof(fpath), "%s/%s", BOOKS_DIR_PATH, s_upload_filename);
            unlink(fpath);
            ESP_LOGW(TAG, "Deleted partial file on stop: %s", fpath);
            s_upload_filename[0] = '\0';
        }
    }

    s_is_uploading = false;
    s_upload_error = false;
    s_bytes_received = 0;
    s_expected_file_size = 0;
    s_expected_seq = 0;
    s_current_mtu = 23;

    if (s_data_ringbuf != NULL) {
        size_t dummy_sz = 0;
        void *dummy = NULL;
        while ((dummy = xRingbufferReceiveUpTo(s_data_ringbuf, &dummy_sz, 0, 4096)) != NULL) {
            vRingbufferReturnItem(s_data_ringbuf, dummy);
        }
    }
}

bool ble_sync_is_connected(void)
{
    return s_is_connected;
}

uint32_t ble_sync_get_bytes_received(void)
{
    return s_bytes_received;
}

esp_err_t ble_sync_notify_status(void)
{
    ble_sync_status_payload_t status;
    ble_sync_get_status_payload(&status);
    return ble_sync_notify(s_status_val_handle, &status, sizeof(status));
}

void ble_sync_register_callback(ble_sync_event_cb_t cb, void *user_data)
{
    s_event_cb = cb;
    s_event_cb_arg = user_data;
}
