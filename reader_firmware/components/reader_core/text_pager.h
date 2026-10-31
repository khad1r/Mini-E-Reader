#ifndef TEXT_PAGER_H
#define TEXT_PAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "GUI_Paint.h"
#include "fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEXT_PAGER_HISTORY_DEPTH  16
#define TEXT_PAGER_MAX_LINES      16
#define TEXT_PAGER_MAX_LINE_CHARS 40

typedef enum {
    TEXT_PAGER_FONT_16 = 0, // 8x16 px, ~22 chars per line, max 8 lines per page
    TEXT_PAGER_FONT_12 = 1, // 6x12 px, ~30 chars per line, max 11 lines per page
} text_pager_font_size_t;

typedef struct {
    char lines[TEXT_PAGER_MAX_LINES][TEXT_PAGER_MAX_LINE_CHARS * 4 + 1];
    size_t line_count;
    size_t page_start_offset;
    size_t page_end_offset;
    uint32_t progress_pct;
    bool is_eof;
    bool is_bof;
} text_pager_page_t;

typedef struct {
    char active_book_path[256];
    char book_title[128];
    size_t total_bytes;
    size_t current_offset;
    text_pager_font_size_t font_size;

    // Rolling history of byte offsets (16 pages deep) for instant reverse paging
    size_t history[TEXT_PAGER_HISTORY_DEPTH];
    size_t history_count;

    // Current page content
    text_pager_page_t current_page;
    bool book_open;
} text_pager_t;

/**
 * @brief Initialize the text pager module.
 * @return ESP_OK on success.
 */
esp_err_t text_pager_init(void);

/**
 * @brief Deinitialize text pager and release resources.
 */
void text_pager_deinit(void);

/**
 * @brief Open a book file and seek to byte_offset.
 * @param path Full file path (e.g. "/sdcard/books/example.txt").
 * @param byte_offset Starting byte offset (0 for start).
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t text_pager_open_book(const char *path, size_t byte_offset);

/**
 * @brief Close currently open book.
 */
void text_pager_close_book(void);

/**
 * @brief Set the font size (Font16 or Font12).
 * @param font_size TEXT_PAGER_FONT_16 or TEXT_PAGER_FONT_12.
 * @return ESP_OK on success.
 */
esp_err_t text_pager_set_font_size(text_pager_font_size_t font_size);

/**
 * @brief Get current font size.
 * @return Current text_pager_font_size_t.
 */
text_pager_font_size_t text_pager_get_font_size(void);

/**
 * @brief Seek to a specific byte offset in the current book.
 * @param byte_offset Target byte offset.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t text_pager_seek(size_t byte_offset);

/**
 * @brief Advance to the next page.
 *        Pushes current offset onto 16-deep rolling history and parses next page.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND at EOF.
 */
esp_err_t text_pager_next_page(void);

/**
 * @brief Go back to previous page.
 *        Pops from 16-deep rolling history for instant reverse paging.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND at beginning of file.
 */
esp_err_t text_pager_prev_page(void);

/**
 * @brief Get current page data structure.
 * @return Pointer to current text_pager_page_t, or NULL if no book open.
 */
const text_pager_page_t *text_pager_get_current_page(void);

/**
 * @brief Get current byte offset.
 * @return Current byte offset in active book.
 */
size_t text_pager_get_current_offset(void);

/**
 * @brief Get total bytes of current book.
 * @return Total file size in bytes.
 */
size_t text_pager_get_total_bytes(void);

/**
 * @brief Render current page into GUI_Paint canvas buffer.
 *        Draws Header (book title), Body (text lines), and Footer (progress %).
 * @return ESP_OK on success.
 */
esp_err_t text_pager_render_page(void);

/**
 * @brief Render current page into an explicit canvas buffer and select it in Paint.
 * @param canvas_buffer Pointer to display buffer (at least DISPLAY_BUFFER_SIZE bytes).
 * @return ESP_OK on success.
 */
esp_err_t text_pager_render_to_buffer(uint8_t *canvas_buffer);

#ifdef __cplusplus
}
#endif

#endif // TEXT_PAGER_H
