#include "book_library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_err.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdcard.h"

static const char *TAG = "book_library";

static book_info_t     s_books[BOOK_MAX_COUNT];
static size_t          s_book_count = 0;
static reading_state_t s_reading_state = {0};
static SemaphoreHandle_t s_mutex = NULL;
static bool            s_initialized = false;

static int compare_books(const void *a, const void *b) {
    const book_info_t *book_a = (const book_info_t *)a;
    const book_info_t *book_b = (const book_info_t *)b;
    return strcasecmp(book_a->filename, book_b->filename);
}

static void ensure_dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0777);
    }
}

esp_err_t book_library_init(void) {
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    ensure_dir_exists(BOOK_LIBRARY_BOOKS_DIR);
    ensure_dir_exists(BOOK_LIBRARY_STATE_DIR);

    // Initial scan and load
    book_library_scan();
    book_library_load_state(&s_reading_state);

    s_initialized = true;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Book library initialized with %u books", (unsigned int)s_book_count);
    return ESP_OK;
}

void book_library_deinit(void) {
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_book_count = 0;
        memset(&s_reading_state, 0, sizeof(s_reading_state));
        s_initialized = false;
        xSemaphoreGive(s_mutex);

        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
}

esp_err_t book_library_scan(void) {
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    ensure_dir_exists(BOOK_LIBRARY_BOOKS_DIR);

    DIR *dir = opendir(BOOK_LIBRARY_BOOKS_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open books directory: %s", BOOK_LIBRARY_BOOKS_DIR);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    s_book_count = 0;
    struct dirent *ent = NULL;

    while ((ent = readdir(dir)) != NULL) {
        if (s_book_count >= BOOK_MAX_COUNT) {
            ESP_LOGW(TAG, "Max book limit (%d) reached", BOOK_MAX_COUNT);
            break;
        }

        // Skip hidden files
        if (ent->d_name[0] == '.') {
            continue;
        }

        size_t name_len = strlen(ent->d_name);
        if (name_len < 4) {
            continue;
        }

        // Must end in .txt
        if (strcasecmp(ent->d_name + name_len - 4, ".txt") != 0) {
            continue;
        }

        book_info_t *book = &s_books[s_book_count];
        strncpy(book->filename, ent->d_name, sizeof(book->filename) - 1);
        book->filename[sizeof(book->filename) - 1] = '\0';

        snprintf(book->path, sizeof(book->path), "%s/%s", BOOK_LIBRARY_BOOKS_DIR, ent->d_name);

        struct stat st;
        if (stat(book->path, &st) == 0) {
            book->size = (size_t)st.st_size;
        } else {
            book->size = 0;
        }

        s_book_count++;
    }

    closedir(dir);

    if (s_book_count > 1) {
        qsort(s_books, s_book_count, sizeof(book_info_t), compare_books);
    }

    ESP_LOGI(TAG, "Scanned %u books in %s", (unsigned int)s_book_count, BOOK_LIBRARY_BOOKS_DIR);

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

size_t book_library_get_count(void) {
    size_t count = 0;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        count = s_book_count;
        xSemaphoreGive(s_mutex);
    }
    return count;
}

const book_info_t *book_library_get_book(size_t index) {
    const book_info_t *res = NULL;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (index < s_book_count) {
            res = &s_books[index];
        }
        xSemaphoreGive(s_mutex);
    }
    return res;
}

const book_info_t *book_library_find_book(const char *filename_or_path) {
    if (!filename_or_path || !s_mutex) {
        return NULL;
    }

    const book_info_t *found = NULL;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_book_count; i++) {
        if (strcasecmp(s_books[i].filename, filename_or_path) == 0 ||
            strcmp(s_books[i].path, filename_or_path) == 0) {
            found = &s_books[i];
            break;
        }
    }

    xSemaphoreGive(s_mutex);
    return found;
}

esp_err_t book_library_load_state(reading_state_t *state) {
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    FILE *fp = fopen(BOOK_LIBRARY_STATE_FILE, "r");
    if (!fp) {
        ESP_LOGI(TAG, "State file not found (%s), initializing defaults", BOOK_LIBRARY_STATE_FILE);
        memset(&s_reading_state, 0, sizeof(s_reading_state));
        if (s_book_count > 0) {
            strncpy(s_reading_state.active_book_path, s_books[0].path, sizeof(s_reading_state.active_book_path) - 1);
            s_reading_state.total_bytes = s_books[0].size;
        }
        if (state) {
            *state = s_reading_state;
        }
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 16384) {
        fclose(fp);
        ESP_LOGW(TAG, "State file empty or abnormally large (%ld bytes)", fsize);
        if (state) {
            *state = s_reading_state;
        }
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    char *json_buf = (char *)malloc(fsize + 1);
    if (!json_buf) {
        fclose(fp);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(json_buf, 1, fsize, fp);
    fclose(fp);
    json_buf[read_bytes] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse state JSON");
        if (state) {
            *state = s_reading_state;
        }
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    memset(&s_reading_state, 0, sizeof(s_reading_state));

    cJSON *item = cJSON_GetObjectItem(root, "active_book_path");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(s_reading_state.active_book_path, item->valuestring, sizeof(s_reading_state.active_book_path) - 1);
    }

    item = cJSON_GetObjectItem(root, "byte_offset");
    if (cJSON_IsNumber(item)) {
        s_reading_state.byte_offset = (size_t)item->valuedouble;
    }

    item = cJSON_GetObjectItem(root, "total_bytes");
    if (cJSON_IsNumber(item)) {
        s_reading_state.total_bytes = (size_t)item->valuedouble;
    }

    cJSON *bms = cJSON_GetObjectItem(root, "bookmarks");
    if (cJSON_IsArray(bms)) {
        int bcount = cJSON_GetArraySize(bms);
        for (int i = 0; i < bcount && i < BOOK_MAX_BOOKMARKS; i++) {
            cJSON *b = cJSON_GetArrayItem(bms, i);
            if (cJSON_IsNumber(b)) {
                s_reading_state.bookmarks[s_reading_state.bookmark_count++] = (size_t)b->valuedouble;
            }
        }
    }

    cJSON_Delete(root);

    // If total_bytes is 0, attempt to look up size from book file
    if (s_reading_state.total_bytes == 0 && s_reading_state.active_book_path[0] != '\0') {
        struct stat st;
        if (stat(s_reading_state.active_book_path, &st) == 0) {
            s_reading_state.total_bytes = (size_t)st.st_size;
        }
    }

    if (state) {
        *state = s_reading_state;
    }

    ESP_LOGI(TAG, "Loaded state: active=%s, offset=%u, total=%u, bookmarks=%u",
             s_reading_state.active_book_path,
             (unsigned int)s_reading_state.byte_offset,
             (unsigned int)s_reading_state.total_bytes,
             (unsigned int)s_reading_state.bookmark_count);

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t book_library_save_state(const reading_state_t *state) {
    if (!state || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    ensure_dir_exists(BOOK_LIBRARY_STATE_DIR);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "active_book_path", state->active_book_path);
    cJSON_AddNumberToObject(root, "byte_offset", (double)state->byte_offset);
    cJSON_AddNumberToObject(root, "total_bytes", (double)state->total_bytes);

    cJSON *bm_arr = cJSON_CreateArray();
    if (bm_arr) {
        for (size_t i = 0; i < state->bookmark_count; i++) {
            cJSON_AddItemToArray(bm_arr, cJSON_CreateNumber((double)state->bookmarks[i]));
        }
        cJSON_AddItemToObject(root, "bookmarks", bm_arr);
    }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    FILE *fp = fopen(BOOK_LIBRARY_STATE_FILE, "w");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open %s for writing: errno %d", BOOK_LIBRARY_STATE_FILE, errno);
        free(json_str);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, fp);
    fflush(fp);
    fclose(fp);
    free(json_str);

    if (written != len) {
        ESP_LOGE(TAG, "Partial write to %s (%u / %u bytes)", BOOK_LIBRARY_STATE_FILE, (unsigned int)written, (unsigned int)len);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    s_reading_state = *state;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t book_library_get_current_state(reading_state_t *state) {
    if (!state || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *state = s_reading_state;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t book_library_set_active_book(const char *book_path, bool reset_offset) {
    if (!book_path || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool same_book = (strcmp(s_reading_state.active_book_path, book_path) == 0);

    strncpy(s_reading_state.active_book_path, book_path, sizeof(s_reading_state.active_book_path) - 1);
    s_reading_state.active_book_path[sizeof(s_reading_state.active_book_path) - 1] = '\0';

    if (reset_offset || !same_book) {
        s_reading_state.byte_offset = 0;
        s_reading_state.bookmark_count = 0;
    }

    struct stat st;
    if (stat(book_path, &st) == 0) {
        s_reading_state.total_bytes = (size_t)st.st_size;
    } else {
        s_reading_state.total_bytes = 0;
    }

    xSemaphoreGive(s_mutex);

    return book_library_save_state(&s_reading_state);
}

esp_err_t book_library_update_offset(size_t offset) {
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_reading_state.byte_offset = offset;
    reading_state_t copy = s_reading_state;
    xSemaphoreGive(s_mutex);

    return book_library_save_state(&copy);
}

esp_err_t book_library_add_bookmark(size_t offset) {
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Check if already exists
    for (size_t i = 0; i < s_reading_state.bookmark_count; i++) {
        if (s_reading_state.bookmarks[i] == offset) {
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }

    if (s_reading_state.bookmark_count >= BOOK_MAX_BOOKMARKS) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    // Insert sorted
    size_t insert_idx = s_reading_state.bookmark_count;
    for (size_t i = 0; i < s_reading_state.bookmark_count; i++) {
        if (offset < s_reading_state.bookmarks[i]) {
            insert_idx = i;
            break;
        }
    }

    for (size_t i = s_reading_state.bookmark_count; i > insert_idx; i--) {
        s_reading_state.bookmarks[i] = s_reading_state.bookmarks[i - 1];
    }
    s_reading_state.bookmarks[insert_idx] = offset;
    s_reading_state.bookmark_count++;

    reading_state_t copy = s_reading_state;
    xSemaphoreGive(s_mutex);

    return book_library_save_state(&copy);
}

esp_err_t book_library_remove_bookmark(size_t offset) {
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int found_idx = -1;
    for (size_t i = 0; i < s_reading_state.bookmark_count; i++) {
        if (s_reading_state.bookmarks[i] == offset) {
            found_idx = (int)i;
            break;
        }
    }

    if (found_idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = (size_t)found_idx; i + 1 < s_reading_state.bookmark_count; i++) {
        s_reading_state.bookmarks[i] = s_reading_state.bookmarks[i + 1];
    }
    s_reading_state.bookmark_count--;

    reading_state_t copy = s_reading_state;
    xSemaphoreGive(s_mutex);

    return book_library_save_state(&copy);
}

bool book_library_is_bookmarked(size_t offset) {
    if (s_mutex == NULL) {
        return false;
    }

    bool found = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_reading_state.bookmark_count; i++) {
        if (s_reading_state.bookmarks[i] == offset) {
            found = true;
            break;
        }
    }

    xSemaphoreGive(s_mutex);
    return found;
}
