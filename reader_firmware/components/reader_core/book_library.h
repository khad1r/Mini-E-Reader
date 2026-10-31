#ifndef BOOK_LIBRARY_H
#define BOOK_LIBRARY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOK_LIBRARY_BOOKS_DIR     "/sdcard/books"
#define BOOK_LIBRARY_STATE_DIR     "/sdcard/.reader"
#define BOOK_LIBRARY_STATE_FILE    "/sdcard/.reader/state.json"

#define BOOK_FILENAME_MAX_LEN      128
#define BOOK_PATH_MAX_LEN          256
#define BOOK_MAX_COUNT             64
#define BOOK_MAX_BOOKMARKS         32

typedef struct {
    char filename[BOOK_FILENAME_MAX_LEN];
    char path[BOOK_PATH_MAX_LEN];
    size_t size; // file size in bytes
} book_info_t;

typedef struct {
    char active_book_path[BOOK_PATH_MAX_LEN];
    size_t byte_offset;
    size_t total_bytes;
    size_t bookmarks[BOOK_MAX_BOOKMARKS];
    size_t bookmark_count;
} reading_state_t;

/**
 * @brief Initialize the book library and load reading state.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t book_library_init(void);

/**
 * @brief Deinitialize book library and release resources.
 */
void book_library_deinit(void);

/**
 * @brief Scan /sdcard/books/ for .txt files.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t book_library_scan(void);

/**
 * @brief Get the count of available books.
 * @return Number of books found.
 */
size_t book_library_get_count(void);

/**
 * @brief Get book info by index.
 * @param index Index in [0, book_count - 1].
 * @return Pointer to book_info_t, or NULL if index out of bounds.
 */
const book_info_t *book_library_get_book(size_t index);

/**
 * @brief Find book by filename or path.
 * @param filename_or_path Filename (e.g. "alice.txt") or full path (e.g. "/sdcard/books/alice.txt").
 * @return Pointer to book_info_t, or NULL if not found.
 */
const book_info_t *book_library_find_book(const char *filename_or_path);

/**
 * @brief Load reading progress state from /sdcard/.reader/state.json.
 * @param state Pointer to reading_state_t structure to fill.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t book_library_load_state(reading_state_t *state);

/**
 * @brief Save reading progress state to /sdcard/.reader/state.json.
 * @param state Pointer to reading_state_t structure to save.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t book_library_save_state(const reading_state_t *state);

/**
 * @brief Get copy of current in-memory reading state.
 * @param state Pointer to reading_state_t structure.
 * @return ESP_OK on success.
 */
esp_err_t book_library_get_current_state(reading_state_t *state);

/**
 * @brief Set current active book path and update total bytes.
 * @param book_path Full path to active book.
 * @param reset_offset If true, reset byte_offset to 0; if false, retain offset if same book.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t book_library_set_active_book(const char *book_path, bool reset_offset);

/**
 * @brief Update the byte offset of the active book and persist to state.json.
 * @param offset New byte offset.
 * @return ESP_OK on success.
 */
esp_err_t book_library_update_offset(size_t offset);

/**
 * @brief Add a bookmark at the specified offset.
 * @param offset Byte offset to bookmark.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if max reached, ESP_OK if already present.
 */
esp_err_t book_library_add_bookmark(size_t offset);

/**
 * @brief Remove a bookmark at the specified offset.
 * @param offset Byte offset to remove.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not present.
 */
esp_err_t book_library_remove_bookmark(size_t offset);

/**
 * @brief Check if an offset is bookmarked.
 * @param offset Byte offset to check.
 * @return true if bookmarked, false otherwise.
 */
bool book_library_is_bookmarked(size_t offset);

#ifdef __cplusplus
}
#endif

#endif // BOOK_LIBRARY_H
