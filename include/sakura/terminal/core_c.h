#ifndef SAKURA_TERMINAL_CORE_C_H
#define SAKURA_TERMINAL_CORE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAKURA_TERMINAL_CORE_ABI_VERSION 1u
#define SAKURA_TERMINAL_INVALID UINT32_MAX

typedef struct SakuraTerminal SakuraTerminal;
typedef struct SakuraTerminalFrame SakuraTerminalFrame;

typedef void (*SakuraTerminalWriteCallback)(void *userdata,
                                             const char *data,
                                             size_t length);

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

typedef struct SakuraTerminalFrameInfo {
    uint64_t generation;
    int changed;
    int full_repaint;
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

/* Run text is UTF-8 borrowed from the frame. `cell_count` counts terminal
 * grid cells, so it includes the two-cell advance of a wide glyph. */

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
SakuraTerminal *sakura_terminal_new(SakuraTerminalWriteCallback callback,
                                     void *userdata);
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

void sakura_terminal_get_metrics(const SakuraTerminal *terminal,
                                 SakuraTerminalMetrics *metrics);

#ifdef __cplusplus
}
#endif

#endif
