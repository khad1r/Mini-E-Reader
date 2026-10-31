#include "text_pager.h"
#include "book_library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "epaper_port.h"

static const char *TAG = "text_pager";

#ifndef DISPLAY_WIDTH
#define DISPLAY_WIDTH          200
#endif
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT         200
#endif
#ifndef UI_HEADER_HEIGHT
#define UI_HEADER_HEIGHT       19
#endif
#ifndef UI_FOOTER_Y
#define UI_FOOTER_Y            184
#endif
#ifndef UI_FOOTER_HEIGHT
#define UI_FOOTER_HEIGHT       16
#endif
#ifndef UI_BODY_Y_START
#define UI_BODY_Y_START        22
#endif
#ifndef UI_BODY_MAX_HEIGHT
#define UI_BODY_MAX_HEIGHT     160
#endif

static text_pager_t      s_pager = {0};
static FILE             *s_book_fp = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static bool              s_initialized = false;

static inline UWORD get_color_black(void) {
    return (Paint.Scale == 4) ? EPD_1IN54G_BLACK : BLACK;
}

static inline UWORD get_color_white(void) {
    return (Paint.Scale == 4) ? EPD_1IN54G_WHITE : WHITE;
}

static void draw_safe_char(UWORD x, UWORD y, char c, sFONT *font, UWORD fg, UWORD bg) {
    if (c < 32 || c > 126) {
        c = ' ';
    }
    Paint_DrawChar(x, y, c, font, fg, bg);
}

static void draw_safe_string(UWORD x, UWORD y, const char *str, sFONT *font, UWORD fg, UWORD bg) {
    if (!str || !font) return;
    UWORD cur_x = x;
    while (*str) {
        if (cur_x + font->Width > Paint.Width) {
            break;
        }
        draw_safe_char(cur_x, y, *str, font, fg, bg);
        cur_x += font->Width;
        str++;
    }
}

static void extract_clean_title(const char *path, char *out_title, size_t max_len) {
    if (!path || !out_title || max_len == 0) return;

    const char *slash = strrchr(path, '/');
    const char *name = slash ? (slash + 1) : path;

    char temp[128];
    strncpy(temp, name, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    char *dot = strrchr(temp, '.');
    if (dot && strcasecmp(dot, ".txt") == 0) {
        *dot = '\0';
    }

    bool cap_next = true;
    for (size_t i = 0; temp[i] != '\0'; i++) {
        if (temp[i] == '_' || temp[i] == '-') {
            temp[i] = ' ';
            cap_next = true;
        } else if (cap_next && isalpha((unsigned char)temp[i])) {
            temp[i] = (char)toupper((unsigned char)temp[i]);
            cap_next = false;
        } else {
            cap_next = false;
        }
    }

    if (strlen(temp) > 28 && max_len > 31) {
        temp[25] = '.';
        temp[26] = '.';
        temp[27] = '.';
        temp[28] = '\0';
    }

    strncpy(out_title, temp, max_len - 1);
    out_title[max_len - 1] = '\0';
}

static size_t align_utf8_forward(const uint8_t *buf, size_t cur, size_t len) {
    while (cur < len && (buf[cur] & 0xC0) == 0x80) {
        cur++;
    }
    return cur;
}

static inline uint32_t decode_utf8(const uint8_t *buf, size_t cur, size_t len, int *out_len) {
    uint8_t b0 = buf[cur];
    if ((b0 & 0x80) == 0) {
        *out_len = 1;
        return b0;
    } else if ((b0 & 0xE0) == 0xC0) {
        if (cur + 1 < len && (buf[cur + 1] & 0xC0) == 0x80) {
            *out_len = 2;
            return ((b0 & 0x1F) << 6) | (buf[cur + 1] & 0x3F);
        }
    } else if ((b0 & 0xF0) == 0xE0) {
        if (cur + 2 < len && (buf[cur + 1] & 0xC0) == 0x80 && (buf[cur + 2] & 0xC0) == 0x80) {
            *out_len = 3;
            return ((b0 & 0x0F) << 12) | ((buf[cur + 1] & 0x3F) << 6) | (buf[cur + 2] & 0x3F);
        }
    } else if ((b0 & 0xF8) == 0xF0) {
        if (cur + 3 < len && (buf[cur + 1] & 0xC0) == 0x80 && (buf[cur + 2] & 0xC0) == 0x80 && (buf[cur + 3] & 0xC0) == 0x80) {
            *out_len = 4;
            return ((b0 & 0x07) << 18) | ((buf[cur + 1] & 0x3F) << 12) | ((buf[cur + 2] & 0x3F) << 6) | (buf[cur + 3] & 0x3F);
        }
    }
    *out_len = 1;
    return '?';
}

static inline char map_unicode_to_ascii(uint32_t cp) {
    if (cp == 0x2018 || cp == 0x2019) return '\'';
    if (cp == 0x201C || cp == 0x201D) return '"';
    if (cp == 0x2013 || cp == 0x2014) return '-';
    if (cp == 0x2026) return '.';
    if (cp == 0x00A0) return ' ';
    if (cp >= 32 && cp <= 126) return (char)cp;
    return ' ';
}

static esp_err_t parse_page_at_offset(size_t offset, text_pager_page_t *page) {
    if (!s_book_fp || !page) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(page, 0, sizeof(*page));
    page->page_start_offset = offset;

    if (offset >= s_pager.total_bytes) {
        page->page_end_offset = s_pager.total_bytes;
        page->is_eof = true;
        page->is_bof = (offset == 0);
        return ESP_OK;
    }

    if (fseek(s_book_fp, (long)offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek failed to offset %u", (unsigned int)offset);
        return ESP_FAIL;
    }

    uint8_t read_buf[2048];
    size_t bytes_read = fread(read_buf, 1, sizeof(read_buf), s_book_fp);
    if (bytes_read == 0) {
        page->page_end_offset = s_pager.total_bytes;
        page->is_eof = true;
        page->is_bof = (offset == 0);
        return ESP_OK;
    }

    size_t cursor = 0;
    cursor = align_utf8_forward(read_buf, cursor, bytes_read);
    page->page_start_offset = offset + cursor;

    size_t max_lines = (s_pager.font_size == TEXT_PAGER_FONT_16) ? 8 : 11;
    size_t max_cols  = (s_pager.font_size == TEXT_PAGER_FONT_16) ? 22 : 30;

    page->line_count = 0;

    while (page->line_count < max_lines && cursor < bytes_read) {
        char *line = page->lines[page->line_count];
        size_t line_bytes = 0;
        size_t line_cols = 0;

        size_t last_space_cursor = 0;
        size_t last_space_bytes = 0;
        bool have_space = false;

        while (cursor < bytes_read) {
            uint8_t b0 = read_buf[cursor];

            if (b0 == '\r') {
                cursor++;
                if (cursor < bytes_read && read_buf[cursor] == '\n') {
                    cursor++;
                }
                break;
            } else if (b0 == '\n') {
                cursor++;
                break;
            }

            int cp_len = 1;
            uint32_t cp = decode_utf8(read_buf, cursor, bytes_read, &cp_len);
            char ch = map_unicode_to_ascii(cp);
            size_t col_width = 1;
            if (cp >= 0x4E00 && cp <= 0x9FFF) {
                col_width = 2;
            }

            if (line_cols + col_width > max_cols) {
                if (have_space && last_space_bytes > 0) {
                    cursor = last_space_cursor;
                    line_bytes = last_space_bytes;
                    while (cursor < bytes_read && (read_buf[cursor] == ' ' || read_buf[cursor] == '\t')) {
                        cursor++;
                    }
                }
                break;
            }

            if (ch == ' ' || cp == '\t') {
                ch = ' ';
                if (line_cols > 0) {
                    last_space_cursor = cursor + cp_len;
                    last_space_bytes = line_bytes;
                    have_space = true;
                }
            }

            if (line_bytes < sizeof(page->lines[0]) - 1) {
                line[line_bytes++] = ch;
            }
            line_cols += col_width;
            cursor += cp_len;
        }

        while (line_bytes > 0 && line[line_bytes - 1] == ' ') {
            line_bytes--;
        }
        line[line_bytes] = '\0';

        page->line_count++;
    }

    page->page_end_offset = offset + cursor;
    if (page->page_end_offset >= s_pager.total_bytes || (bytes_read < sizeof(read_buf) && cursor >= bytes_read)) {
        page->page_end_offset = s_pager.total_bytes;
        page->is_eof = true;
    } else {
        page->is_eof = false;
    }

    page->is_bof = (page->page_start_offset == 0);

    if (s_pager.total_bytes > 0) {
        if (page->is_eof) {
            page->progress_pct = 100;
        } else {
            page->progress_pct = (uint32_t)((uint64_t)page->page_start_offset * 100 / s_pager.total_bytes);
            if (page->progress_pct > 100) page->progress_pct = 100;
        }
    } else {
        page->progress_pct = 0;
    }

    return ESP_OK;
}

static size_t backtrack_prev_page_offset(size_t current_offset) {
    if (current_offset == 0) return 0;

    size_t max_lines = (s_pager.font_size == TEXT_PAGER_FONT_16) ? 8 : 11;
    size_t max_cols  = (s_pager.font_size == TEXT_PAGER_FONT_16) ? 22 : 30;
    size_t avg_page_bytes = max_lines * max_cols;

    size_t backtrack = avg_page_bytes * 2;
    size_t candidate = (current_offset > backtrack) ? (current_offset - backtrack) : 0;

    text_pager_page_t temp_page;
    size_t curr = candidate;
    size_t prev_start = candidate;

    while (curr < current_offset) {
        if (parse_page_at_offset(curr, &temp_page) != ESP_OK) {
            break;
        }
        if (temp_page.page_end_offset >= current_offset) {
            return temp_page.page_start_offset;
        }
        if (temp_page.page_end_offset <= curr) {
            break;
        }
        prev_start = temp_page.page_start_offset;
        curr = temp_page.page_end_offset;
    }

    return prev_start;
}

esp_err_t text_pager_init(void) {
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_pager, 0, sizeof(s_pager));
    s_pager.font_size = TEXT_PAGER_FONT_16;
    s_initialized = true;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Text pager initialized");
    return ESP_OK;
}

void text_pager_deinit(void) {
    text_pager_close_book();

    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memset(&s_pager, 0, sizeof(s_pager));
        s_initialized = false;
        xSemaphoreGive(s_mutex);

        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
}

esp_err_t text_pager_open_book(const char *path, size_t byte_offset) {
    if (!path || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_book_fp) {
        fclose(s_book_fp);
        s_book_fp = NULL;
    }

    s_book_fp = fopen(path, "rb");
    if (!s_book_fp) {
        ESP_LOGE(TAG, "Failed to open book file: %s", path);
        s_pager.book_open = false;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(s_book_fp, 0, SEEK_END);
    long fsize = ftell(s_book_fp);
    fseek(s_book_fp, 0, SEEK_SET);

    s_pager.total_bytes = (fsize > 0) ? (size_t)fsize : 0;
    strncpy(s_pager.active_book_path, path, sizeof(s_pager.active_book_path) - 1);
    s_pager.active_book_path[sizeof(s_pager.active_book_path) - 1] = '\0';

    extract_clean_title(path, s_pager.book_title, sizeof(s_pager.book_title));

    s_pager.current_offset = (byte_offset < s_pager.total_bytes) ? byte_offset : 0;
    s_pager.history_count = 0;
    s_pager.book_open = true;

    parse_page_at_offset(s_pager.current_offset, &s_pager.current_page);

    xSemaphoreGive(s_mutex);

    book_library_set_active_book(path, false);
    book_library_update_offset(s_pager.current_offset);

    ESP_LOGI(TAG, "Opened '%s' (title: '%s', size: %u bytes, offset: %u)",
             path, s_pager.book_title, (unsigned int)s_pager.total_bytes, (unsigned int)s_pager.current_offset);

    return text_pager_render_page();
}

void text_pager_close_book(void) {
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_book_fp) {
            fclose(s_book_fp);
            s_book_fp = NULL;
        }
        s_pager.book_open = false;
        s_pager.history_count = 0;
        s_pager.active_book_path[0] = '\0';
        s_pager.book_title[0] = '\0';
        memset(&s_pager.current_page, 0, sizeof(s_pager.current_page));
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t text_pager_set_font_size(text_pager_font_size_t font_size) {
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_pager.font_size != font_size) {
        s_pager.font_size = font_size;
        s_pager.history_count = 0; // Clear history on font size change

        if (s_pager.book_open) {
            parse_page_at_offset(s_pager.current_offset, &s_pager.current_page);
        }
    }
    xSemaphoreGive(s_mutex);

    if (s_pager.book_open) {
        text_pager_render_page();
    }
    return ESP_OK;
}

text_pager_font_size_t text_pager_get_font_size(void) {
    text_pager_font_size_t size = TEXT_PAGER_FONT_16;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        size = s_pager.font_size;
        xSemaphoreGive(s_mutex);
    }
    return size;
}

esp_err_t text_pager_seek(size_t byte_offset) {
    if (s_mutex == NULL || !s_pager.book_open) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (byte_offset >= s_pager.total_bytes) {
        byte_offset = (s_pager.total_bytes > 0) ? (s_pager.total_bytes - 1) : 0;
    }

    s_pager.current_offset = byte_offset;
    s_pager.history_count = 0;

    parse_page_at_offset(s_pager.current_offset, &s_pager.current_page);
    xSemaphoreGive(s_mutex);

    book_library_update_offset(s_pager.current_offset);
    return text_pager_render_page();
}

esp_err_t text_pager_next_page(void) {
    if (s_mutex == NULL || !s_pager.book_open) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_pager.current_page.is_eof) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    // Push into rolling history (16 pages deep)
    if (s_pager.history_count < TEXT_PAGER_HISTORY_DEPTH) {
        s_pager.history[s_pager.history_count++] = s_pager.current_offset;
    } else {
        memmove(&s_pager.history[0], &s_pager.history[1], sizeof(size_t) * (TEXT_PAGER_HISTORY_DEPTH - 1));
        s_pager.history[TEXT_PAGER_HISTORY_DEPTH - 1] = s_pager.current_offset;
    }

    s_pager.current_offset = s_pager.current_page.page_end_offset;
    parse_page_at_offset(s_pager.current_offset, &s_pager.current_page);

    xSemaphoreGive(s_mutex);

    book_library_update_offset(s_pager.current_offset);
    text_pager_render_page();

    return ESP_OK;
}

esp_err_t text_pager_prev_page(void) {
    if (s_mutex == NULL || !s_pager.book_open) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t target_offset = 0;

    if (s_pager.history_count > 0) {
        target_offset = s_pager.history[--s_pager.history_count];
    } else if (s_pager.current_offset > 0) {
        target_offset = backtrack_prev_page_offset(s_pager.current_offset);
    } else {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    s_pager.current_offset = target_offset;
    parse_page_at_offset(s_pager.current_offset, &s_pager.current_page);

    xSemaphoreGive(s_mutex);

    book_library_update_offset(s_pager.current_offset);
    text_pager_render_page();

    return ESP_OK;
}

const text_pager_page_t *text_pager_get_current_page(void) {
    if (!s_pager.book_open) {
        return NULL;
    }
    return &s_pager.current_page;
}

size_t text_pager_get_current_offset(void) {
    return s_pager.current_offset;
}

size_t text_pager_get_total_bytes(void) {
    return s_pager.total_bytes;
}

esp_err_t text_pager_render_page(void) {
    if (Paint.Image == NULL) {
        ESP_LOGW(TAG, "Paint.Image is NULL, canvas not selected");
        return ESP_ERR_INVALID_STATE;
    }

    UWORD fg_col = get_color_black();
    UWORD bg_col = get_color_white();

    // 1. Header (0 .. UI_HEADER_HEIGHT)
    Paint_ClearWindows(0, 0, Paint.Width - 1, UI_HEADER_HEIGHT - 1, bg_col);
    if (s_pager.book_title[0] != '\0') {
        draw_safe_string(6, 3, s_pager.book_title, &Font12, fg_col, bg_col);
    }
    Paint_DrawLine(0, UI_HEADER_HEIGHT, Paint.Width - 1, UI_HEADER_HEIGHT, fg_col, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    // 2. Body (UI_BODY_Y_START .. UI_FOOTER_Y - 1)
    Paint_ClearWindows(0, UI_HEADER_HEIGHT + 1, Paint.Width - 1, UI_FOOTER_Y - 1, bg_col);

    sFONT *body_font = (s_pager.font_size == TEXT_PAGER_FONT_16) ? &Font16 : &Font12;
    UWORD line_pitch = (s_pager.font_size == TEXT_PAGER_FONT_16) ? 20 : 14;
    UWORD margin_x   = (s_pager.font_size == TEXT_PAGER_FONT_16) ? 12 : 10;
    UWORD body_y     = UI_BODY_Y_START;

    if (!s_pager.book_open || s_pager.current_page.line_count == 0) {
        draw_safe_string(20, 80, "No book open.", &Font12, fg_col, bg_col);
        draw_safe_string(20, 100, "Press BOOT for menu", &Font12, fg_col, bg_col);
    } else {
        for (size_t i = 0; i < s_pager.current_page.line_count; i++) {
            UWORD y_pos = body_y + (UWORD)(i * line_pitch);
            if (y_pos + body_font->Height >= UI_FOOTER_Y) {
                break;
            }
            draw_safe_string(margin_x, y_pos, s_pager.current_page.lines[i], body_font, fg_col, bg_col);
        }
    }

    // 3. Footer (UI_FOOTER_Y .. Paint.Height - 1)
    Paint_DrawLine(0, UI_FOOTER_Y - 1, Paint.Width - 1, UI_FOOTER_Y - 1, fg_col, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_ClearWindows(0, UI_FOOTER_Y, Paint.Width - 1, Paint.Height - 1, bg_col);

    // Bookmark indicator on left
    if (s_pager.book_open && book_library_is_bookmarked(s_pager.current_page.page_start_offset)) {
        draw_safe_string(8, UI_FOOTER_Y + 2, "[BM]", &Font12, fg_col, bg_col);
    }

    // Progress percentage on right
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%u%%", (unsigned int)s_pager.current_page.progress_pct);
    size_t pct_len = strlen(pct_str);
    UWORD pct_w = (UWORD)(pct_len * Font12.Width);
    UWORD pct_x = (Paint.Width > pct_w + 8) ? (Paint.Width - pct_w - 8) : (Paint.Width - pct_w);
    draw_safe_string(pct_x, UI_FOOTER_Y + 2, pct_str, &Font12, fg_col, bg_col);

    return ESP_OK;
}

esp_err_t text_pager_render_to_buffer(uint8_t *canvas_buffer) {
    if (!canvas_buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    Paint_SelectImage(canvas_buffer);
    return text_pager_render_page();
}
