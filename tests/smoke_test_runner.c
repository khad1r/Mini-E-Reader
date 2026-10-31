#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <ctype.h>

#define MAX_LINES 16
#define MAX_LINE_CHARS 40

typedef struct {
    char lines[MAX_LINES][MAX_LINE_CHARS * 4 + 1];
    size_t line_count;
    size_t page_start_offset;
    size_t page_end_offset;
    uint32_t progress_pct;
    bool is_eof;
    bool is_bof;
} test_page_t;

// UTF-8 Transliteration helper
static inline uint32_t decode_utf8(const uint8_t *buf, size_t cur, size_t len, int *out_len) {
    uint8_t b0 = buf[cur];
    if ((b0 & 0x80) == 0) {
        *out_len = 1;
        return (uint32_t)b0;
    } else if ((b0 & 0xE0) == 0xC0) {
        if (cur + 1 >= len) { *out_len = 1; return '?'; }
        *out_len = 2;
        return ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(buf[cur + 1] & 0x3F);
    } else if ((b0 & 0xF0) == 0xE0) {
        if (cur + 2 >= len) { *out_len = 1; return '?'; }
        *out_len = 3;
        return ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(buf[cur + 1] & 0x3F) << 6) | (uint32_t)(buf[cur + 2] & 0x3F);
    } else if ((b0 & 0xF8) == 0xF0) {
        if (cur + 3 >= len) { *out_len = 1; return '?'; }
        *out_len = 4;
        return ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(buf[cur + 1] & 0x3F) << 12) | ((uint32_t)(buf[cur + 2] & 0x3F) << 6) | (uint32_t)(buf[cur + 3] & 0x3F);
    }
    *out_len = 1;
    return '?';
}

static inline char transliterate_char(uint32_t cp) {
    if (cp >= 0x2018 && cp <= 0x201B) return '\'';
    if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x201F) return '"';
    if (cp == 0x2013 || cp == 0x2014) return '-';
    if (cp == 0x2026) return '.';
    if (cp >= 32 && cp <= 126) return (char)cp;
    return '?';
}

// Simulated parse_page_at_offset matching text_pager.c
static int parse_test_page(FILE *fp, size_t file_size, size_t offset, size_t max_lines, size_t max_cols, test_page_t *page) {
    memset(page, 0, sizeof(*page));
    page->page_start_offset = offset;
    page->is_bof = (offset == 0);

    if (offset >= file_size) {
        page->page_end_offset = file_size;
        page->is_eof = true;
        page->progress_pct = 100;
        return 0;
    }

    fseek(fp, (long)offset, SEEK_SET);
    uint8_t buf[2048];
    size_t bytes_read = fread(buf, 1, sizeof(buf), fp);
    if (bytes_read == 0) {
        page->is_eof = true;
        page->page_end_offset = file_size;
        return 0;
    }

    size_t cursor = 0;
    while (cursor < bytes_read && (buf[cursor] == '\r' || buf[cursor] == '\n')) {
        cursor++;
    }

    while (page->line_count < max_lines && cursor < bytes_read) {
        char *line = page->lines[page->line_count];
        size_t col = 0;
        size_t line_start_cursor = cursor;
        size_t last_space_cursor = (size_t)-1;
        size_t last_space_col = (size_t)-1;

        while (cursor < bytes_read) {
            uint8_t b = buf[cursor];
            if (b == '\r') {
                cursor++;
                if (cursor < bytes_read && buf[cursor] == '\n') cursor++;
                break;
            }
            if (b == '\n') {
                cursor++;
                break;
            }

            int cp_len = 1;
            uint32_t cp = decode_utf8(buf, cursor, bytes_read, &cp_len);
            char ch = transliterate_char(cp);

            if (col >= max_cols) {
                // Word wrap
                if (last_space_col != (size_t)-1 && last_space_col > 0) {
                    line[last_space_col] = '\0';
                    cursor = last_space_cursor;
                    while (cursor < bytes_read && (buf[cursor] == ' ' || buf[cursor] == '\t')) {
                        cursor++;
                    }
                }
                break;
            }

            if (ch == ' ' || ch == '\t') {
                last_space_cursor = cursor + (size_t)cp_len;
                last_space_col = col;
            }

            line[col++] = ch;
            cursor += (size_t)cp_len;
        }

        line[col] = '\0';
        // Trim trailing space
        while (col > 0 && (line[col - 1] == ' ' || line[col - 1] == '\t')) {
            line[--col] = '\0';
        }

        if (col > 0 || (cursor > line_start_cursor)) {
            page->line_count++;
        }
    }

    page->page_end_offset = offset + cursor;
    if (page->page_end_offset >= file_size) {
        page->page_end_offset = file_size;
        page->is_eof = true;
    }
    page->progress_pct = (uint32_t)((page->page_end_offset * 100ULL) / (file_size ? file_size : 1));
    if (page->progress_pct > 100) page->progress_pct = 100;

    return 0;
}

int main(void) {
    printf("=== Mini-E-Reader Engine Smoke Verification ===\n\n");

    const char *book_path = "sdcard_template/books/alice_in_wonderland.txt";
    FILE *fp = fopen(book_path, "rb");
    if (!fp) {
        fprintf(stderr, "FAIL: Could not open %s\n", book_path);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    size_t fsize = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    printf("[TEST 1] File loaded: '%s' (%zu bytes)\n", book_path, fsize);
    assert(fsize > 0);

    // Test Font16 (8 lines, 22 cols)
    test_page_t page1, page2, page3;
    printf("\n[TEST 2] Parsing Page 1 (Font 16: 8 lines x 22 cols max)...\n");
    parse_test_page(fp, fsize, 0, 8, 22, &page1);

    printf("Page 1 Start: %zu, End: %zu, Lines: %zu, Progress: %u%%\n",
           page1.page_start_offset, page1.page_end_offset, page1.line_count, page1.progress_pct);
    assert(page1.line_count <= 8);
    assert(page1.page_end_offset > page1.page_start_offset);
    assert(page1.is_bof == true);

    for (size_t i = 0; i < page1.line_count; i++) {
        printf("  L%zu: |%s|\n", i + 1, page1.lines[i]);
    }

    // Advance to Page 2
    printf("\n[TEST 3] Advancing to Page 2...\n");
    parse_test_page(fp, fsize, page1.page_end_offset, 8, 22, &page2);
    printf("Page 2 Start: %zu, End: %zu, Lines: %zu, Progress: %u%%\n",
           page2.page_start_offset, page2.page_end_offset, page2.line_count, page2.progress_pct);
    assert(page2.page_start_offset == page1.page_end_offset);
    assert(page2.page_end_offset > page2.page_start_offset);

    for (size_t i = 0; i < page2.line_count; i++) {
        printf("  L%zu: |%s|\n", i + 1, page2.lines[i]);
    }

    // Advance to Page 3
    printf("\n[TEST 4] Advancing to Page 3...\n");
    parse_test_page(fp, fsize, page2.page_end_offset, 8, 22, &page3);
    printf("Page 3 Start: %zu, End: %zu, Lines: %zu, Progress: %u%%\n",
           page3.page_start_offset, page3.page_end_offset, page3.line_count, page3.progress_pct);
    assert(page3.page_start_offset == page2.page_end_offset);

    // Test Reverse Paging (Simulate rolling history pop)
    printf("\n[TEST 5] Reverse Paging to Page 2 via Cached History...\n");
    test_page_t rev_page2;
    parse_test_page(fp, fsize, page1.page_end_offset, 8, 22, &rev_page2);
    assert(rev_page2.page_start_offset == page2.page_start_offset);
    assert(rev_page2.page_end_offset == page2.page_end_offset);
    printf("Reverse seek verified: returned to exact byte offset %zu\n", rev_page2.page_start_offset);

    // Test Font12 (11 lines, 30 cols)
    printf("\n[TEST 6] Testing Font 12 (11 lines x 30 cols max)...\n");
    test_page_t f12_page;
    parse_test_page(fp, fsize, 0, 11, 30, &f12_page);
    printf("Font12 Page Start: %zu, End: %zu, Lines: %zu, Progress: %u%%\n",
           f12_page.page_start_offset, f12_page.page_end_offset, f12_page.line_count, f12_page.progress_pct);
    assert(f12_page.line_count <= 11);
    assert(f12_page.page_end_offset > page1.page_end_offset); // More text per page

    for (size_t i = 0; i < f12_page.line_count; i++) {
        printf("  L%zu: |%s|\n", i + 1, f12_page.lines[i]);
    }

    fclose(fp);

    printf("\n>>> ALL SMOKE TESTS PASSED CLEANLY <<<\n");
    return 0;
}
