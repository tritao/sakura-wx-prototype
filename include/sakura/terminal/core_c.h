#ifndef SAKURA_TERMINAL_CORE_C_H
#define SAKURA_TERMINAL_CORE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAKURA_TERMINAL_CORE_ABI_VERSION 6u
#define SAKURA_TERMINAL_INVALID UINT32_MAX
/* A renderer may request a different span bound through the frame API. */
#define SAKURA_TERMINAL_DEFAULT_RUN_SPAN_MAX_CELLS 32u
#define SAKURA_TERMINAL_THEME_ANSI_COLORS 16u

typedef struct SakuraTerminal SakuraTerminal;
typedef struct SakuraTerminalFrame SakuraTerminalFrame;

typedef void (*SakuraTerminalWriteCallback)(void *userdata,
                                             const char *data,
                                             size_t length);

/* Toolkit-neutral terminal colors. The ANSI entries are indexed in the
 * standard order (black, red, green, yellow, blue, magenta, cyan, white,
 * followed by their bright variants). Foreground/background are the implicit
 * colors used by cells without an explicit SGR color. */
typedef struct SakuraTerminalTheme {
    uint8_t ansi[SAKURA_TERMINAL_THEME_ANSI_COLORS][3];
    uint8_t foreground[3];
    uint8_t background[3];
} SakuraTerminalTheme;

typedef enum SakuraTerminalCursorStyle {
    SAKURA_TERMINAL_CURSOR_BLOCK = 0,
    SAKURA_TERMINAL_CURSOR_UNDERLINE = 1,
    SAKURA_TERMINAL_CURSOR_BAR = 2,
} SakuraTerminalCursorStyle;

enum {
    SAKURA_TERMINAL_SHIFT = 1u << 0,
    SAKURA_TERMINAL_CONTROL = 1u << 2,
    SAKURA_TERMINAL_ALT = 1u << 3,
    SAKURA_TERMINAL_LOGO = 1u << 4,
};

enum {
    SAKURA_TERMINAL_MOUSE_LEFT = 0,
    SAKURA_TERMINAL_MOUSE_MIDDLE = 1,
    SAKURA_TERMINAL_MOUSE_RIGHT = 2,
    SAKURA_TERMINAL_MOUSE_WHEEL_UP = 4,
    SAKURA_TERMINAL_MOUSE_WHEEL_DOWN = 5,
};

enum {
    SAKURA_TERMINAL_MOUSE_SHIFT = 4,
    SAKURA_TERMINAL_MOUSE_META = 8,
    SAKURA_TERMINAL_MOUSE_CONTROL = 16,
};

enum {
    SAKURA_TERMINAL_MOUSE_PRESSED = 1,
    SAKURA_TERMINAL_MOUSE_RELEASED = 2,
    SAKURA_TERMINAL_MOUSE_MOVED = 4,
};

typedef struct SakuraTerminalDirtyRegion {
    unsigned int left;
    unsigned int top;
    unsigned int right;
    unsigned int bottom;
} SakuraTerminalDirtyRegion;

typedef struct SakuraTerminalDirtySpan {
    unsigned int row;
    unsigned int left;
    unsigned int right;
} SakuraTerminalDirtySpan;

typedef enum SakuraTerminalScrollKind {
    SAKURA_TERMINAL_SCROLL_NONE = 0,
    SAKURA_TERMINAL_SCROLL_CONTENT = 1,
    SAKURA_TERMINAL_SCROLL_VIEWPORT = 2,
    SAKURA_TERMINAL_SCROLL_MIXED = 3,
} SakuraTerminalScrollKind;

typedef struct SakuraTerminalFrameInfo {
    uint64_t generation;
    int changed;
    int full_repaint;
    /* Signed visible-content movement. Positive means rows moved toward the
     * top of the viewport; negative means rows moved toward the bottom. A
     * non-zero value is a hint for framebuffer scroll/blit optimization. */
    int scroll_delta;
    /* Identifies whether scroll_delta came from output, the viewport, or both. */
    SakuraTerminalScrollKind scroll_kind;
    unsigned int columns;
    unsigned int rows;
    unsigned int cursor_x;
    unsigned int cursor_y;
    int cursor_visible;
    SakuraTerminalCursorStyle cursor_style;
    int alternate_screen;
    SakuraTerminalDirtyRegion dirty;
} SakuraTerminalFrameInfo;

typedef struct SakuraTerminalCellView {
    uint32_t codepoint;
    const char *text;
    size_t text_length;
    unsigned int width;
    uint8_t foreground[3];
    uint8_t background[3];
    uint8_t attributes;
} SakuraTerminalCellView;

typedef struct SakuraTerminalRunView {
    unsigned int row;
    unsigned int left;
    unsigned int cell_count;
    uint64_t style_id;
    const char *text;
    size_t text_length;
    uint8_t foreground[3];
    uint8_t background[3];
    uint8_t attributes;
} SakuraTerminalRunView;

/* Called synchronously for each row span while the frame remains alive. The
 * run view and its text are borrowed from the frame and must not be retained. */
typedef void (*SakuraTerminalFrameSpanCallback)(
    void *userdata, const SakuraTerminalRunView *span);

/* Run text is UTF-8 borrowed from the frame. Row runs are homogeneous in
 * style; `cell_count` counts terminal grid cells, so it includes the two-cell
 * advance of a wide glyph. Bounded spans are exposed separately below. */

typedef struct SakuraTerminalMetrics {
    uint64_t output_bytes;
    uint64_t output_chunks;
    uint64_t input_events;
    uint64_t transport_write_bytes;
    uint64_t transport_write_events;
    uint64_t rendered_frames;
    uint64_t frame_cells_decoded;
    uint64_t frame_cells_reused;
    uint64_t render_latency_samples;
    uint64_t max_render_latency_us;
    uint64_t selection_copies;
    uint64_t paste_bytes;
    uint64_t mouse_mode_changes;
    uint64_t mouse_events;
    uint64_t mouse_events_forwarded;
    uint64_t title_changes;
    uint64_t cursor_style_changes;
} SakuraTerminalMetrics;

/*
 * The terminal and all frames are single-owner-thread objects. The caller
 * owns the terminal and must release every returned frame. Frame cell text is
 * borrowed and remains valid until its frame is released.
 */
/* Populate a theme with the library default palette and implicit colors. */
void sakura_terminal_theme_default(SakuraTerminalTheme *theme);

/* The legacy constructor uses the library default theme. */
SakuraTerminal *sakura_terminal_new(SakuraTerminalWriteCallback callback,
                                     void *userdata);
SakuraTerminal *sakura_terminal_new_with_theme(
    SakuraTerminalWriteCallback callback, void *userdata,
    const SakuraTerminalTheme *theme);
void sakura_terminal_free(SakuraTerminal *terminal);
void sakura_terminal_rebind_owner_thread(SakuraTerminal *terminal);

int sakura_terminal_is_ready(const SakuraTerminal *terminal);
const char *sakura_terminal_error(const SakuraTerminal *terminal);
const char *sakura_terminal_title(const SakuraTerminal *terminal);

int sakura_terminal_resize(SakuraTerminal *terminal,
                           unsigned int columns, unsigned int rows);
void sakura_terminal_feed_output(SakuraTerminal *terminal,
                                 const char *data, size_t length);
int sakura_terminal_handle_key(SakuraTerminal *terminal,
                               uint32_t keysym, uint32_t ascii,
                               unsigned int modifiers, uint32_t unicode);
void sakura_terminal_paste(SakuraTerminal *terminal,
                           const char *text, size_t length);
int sakura_terminal_handle_mouse(SakuraTerminal *terminal,
                                 unsigned int cell_x, unsigned int cell_y,
                                 unsigned int pixel_x, unsigned int pixel_y,
                                 unsigned int button, unsigned int event,
                                 unsigned char modifiers);
int sakura_terminal_mouse_reporting_enabled(
    const SakuraTerminal *terminal);

void sakura_terminal_scroll_page_up(SakuraTerminal *terminal,
                                    unsigned int pages);
void sakura_terminal_scroll_page_down(SakuraTerminal *terminal,
                                      unsigned int pages);
void sakura_terminal_scroll_lines(SakuraTerminal *terminal, int lines);

void sakura_terminal_start_selection(SakuraTerminal *terminal,
                                     unsigned int column, unsigned int row);
void sakura_terminal_update_selection(SakuraTerminal *terminal,
                                      unsigned int column, unsigned int row);
void sakura_terminal_select_word(SakuraTerminal *terminal,
                                 unsigned int column, unsigned int row);
void sakura_terminal_select_line(SakuraTerminal *terminal,
                                 unsigned int row);
void sakura_terminal_clear_selection(SakuraTerminal *terminal);
int sakura_terminal_has_selection(const SakuraTerminal *terminal);
char *sakura_terminal_copy_selection(SakuraTerminal *terminal);
void sakura_terminal_free_string(char *text);

SakuraTerminalFrame *sakura_terminal_take_frame(SakuraTerminal *terminal);
void sakura_terminal_frame_free(SakuraTerminalFrame *frame);
int sakura_terminal_frame_info(const SakuraTerminalFrame *frame,
                               SakuraTerminalFrameInfo *info);
size_t sakura_terminal_frame_dirty_span_count(
    const SakuraTerminalFrame *frame);
int sakura_terminal_frame_dirty_span(const SakuraTerminalFrame *frame,
                                     size_t index,
                                     SakuraTerminalDirtySpan *span);
int sakura_terminal_frame_cell(const SakuraTerminalFrame *frame,
                               unsigned int column, unsigned int row,
                               SakuraTerminalCellView *cell);
size_t sakura_terminal_frame_row_run_count(
    const SakuraTerminalFrame *frame, unsigned int row);
int sakura_terminal_frame_row_run(const SakuraTerminalFrame *frame,
                                  unsigned int row, size_t index,
                                  SakuraTerminalRunView *run);
/* Return flattened, style-homogeneous spans for a row. `max_cells` must be
 * non-zero; a wide glyph may make an individual span exceed a bound of one
 * cell so its two-cell advance is never split. */
size_t sakura_terminal_frame_row_span_count(
    const SakuraTerminalFrame *frame, unsigned int row,
    unsigned int max_cells);
int sakura_terminal_frame_row_span(const SakuraTerminalFrame *frame,
                                   unsigned int row, size_t index,
                                   unsigned int max_cells,
                                   SakuraTerminalRunView *span);
/* Visit all flattened row spans in one pass. This avoids the repeated
 * rescanning inherent in count-plus-index access when a renderer needs every
 * span. Returns zero for invalid arguments or an invalid frame. */
int sakura_terminal_frame_for_each_row_span(
    const SakuraTerminalFrame *frame, unsigned int row,
    unsigned int max_cells, SakuraTerminalFrameSpanCallback callback,
    void *userdata);

void sakura_terminal_get_metrics(const SakuraTerminal *terminal,
                                 SakuraTerminalMetrics *metrics);

#ifdef __cplusplus
}
#endif

#endif
