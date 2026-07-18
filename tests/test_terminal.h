#pragma once

#include <sakura/terminal/core_c.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct TestCell {
    uint32_t codepoint = ' ';
    std::string text;
    unsigned int width = 1;
    std::array<uint8_t, 3> foreground {};
    std::array<uint8_t, 3> background {};
    uint8_t attributes = 0;
};

struct TestSnapshot {
    unsigned int columns = 0;
    unsigned int rows = 0;
    unsigned int cursor_x = 0;
    unsigned int cursor_y = 0;
    bool cursor_visible = false;
    SakuraTerminalCursorStyle cursor_style = SAKURA_TERMINAL_CURSOR_BLOCK;
    bool alternate_screen = false;
    std::vector<TestCell> cells;
};

struct TestDirtyRegion {
    unsigned int left = 0;
    unsigned int top = 0;
    unsigned int right = 0;
    unsigned int bottom = 0;

    bool IsEmpty() const
    {
        return left >= right || top >= bottom;
    }
};

struct TestFrame {
    uint64_t generation = 0;
    bool changed = false;
    bool full_repaint = false;
    int scroll_delta = 0;
    TestDirtyRegion dirty;
    std::vector<SakuraTerminalDirtySpan> dirty_spans;
    std::shared_ptr<const TestSnapshot> snapshot;
};

class TestTerminal final {
public:
    using WriteCallback = std::function<void(const char*, std::size_t)>;

    explicit TestTerminal(WriteCallback callback)
        : callback_(std::move(callback)),
          terminal_(sakura_terminal_new(&TestTerminal::WriteBridge, this))
    {
    }

    ~TestTerminal()
    {
        sakura_terminal_free(terminal_);
    }

    TestTerminal(const TestTerminal&) = delete;
    TestTerminal& operator=(const TestTerminal&) = delete;

    bool IsReady() const { return sakura_terminal_is_ready(terminal_) != 0; }

    bool Resize(unsigned int columns, unsigned int rows)
    {
        return sakura_terminal_resize(terminal_, columns, rows) != 0;
    }

    void FeedOutput(const char* data, std::size_t length)
    {
        sakura_terminal_feed_output(terminal_, data, length);
    }

    bool HandleKey(uint32_t keysym, uint32_t ascii, unsigned int modifiers,
                   uint32_t unicode)
    {
        return sakura_terminal_handle_key(terminal_, keysym, ascii, modifiers,
                                          unicode) != 0;
    }

    void Paste(const std::string& text)
    {
        sakura_terminal_paste(terminal_, text.data(), text.size());
    }

    bool HandleMouse(unsigned int cell_x, unsigned int cell_y,
                     unsigned int pixel_x, unsigned int pixel_y,
                     unsigned int button, unsigned int event,
                     unsigned char modifiers)
    {
        return sakura_terminal_handle_mouse(terminal_, cell_x, cell_y,
                                             pixel_x, pixel_y, button, event,
                                             modifiers) != 0;
    }

    bool MouseReportingEnabled() const
    {
        return sakura_terminal_mouse_reporting_enabled(terminal_) != 0;
    }

    void ScrollPageUp(unsigned int pages)
    {
        sakura_terminal_scroll_page_up(terminal_, pages);
    }

    void ScrollPageDown(unsigned int pages)
    {
        sakura_terminal_scroll_page_down(terminal_, pages);
    }

    void ScrollLines(int lines)
    {
        sakura_terminal_scroll_lines(terminal_, lines);
    }

    void StartSelection(unsigned int column, unsigned int row)
    {
        sakura_terminal_start_selection(terminal_, column, row);
    }

    void UpdateSelection(unsigned int column, unsigned int row)
    {
        sakura_terminal_update_selection(terminal_, column, row);
    }

    void SelectWord(unsigned int column, unsigned int row)
    {
        sakura_terminal_select_word(terminal_, column, row);
    }

    void SelectLine(unsigned int row)
    {
        sakura_terminal_select_line(terminal_, row);
    }

    void ClearSelection()
    {
        sakura_terminal_clear_selection(terminal_);
    }

    bool HasSelection() const
    {
        return sakura_terminal_has_selection(terminal_) != 0;
    }

    std::string CopySelection()
    {
        char* text = sakura_terminal_copy_selection(terminal_);
        const std::string result = text == nullptr ? "" : text;
        sakura_terminal_free_string(text);
        return result;
    }

    std::string Title() const
    {
        const char* title = sakura_terminal_title(terminal_);
        return title == nullptr ? "" : title;
    }

    TestFrame TakeFrame()
    {
        TestFrame result;
        SakuraTerminalFrame* frame = sakura_terminal_take_frame(terminal_);
        SakuraTerminalFrameInfo info {};
        if (frame == nullptr || !sakura_terminal_frame_info(frame, &info)) {
            sakura_terminal_frame_free(frame);
            return result;
        }
        result.generation = info.generation;
        result.changed = info.changed != 0;
        result.full_repaint = info.full_repaint != 0;
        result.scroll_delta = info.scroll_delta;
        result.dirty = {info.dirty.left, info.dirty.top, info.dirty.right,
                        info.dirty.bottom};
        const std::size_t span_count =
            sakura_terminal_frame_dirty_span_count(frame);
        result.dirty_spans.reserve(span_count);
        for (std::size_t index = 0; index < span_count; ++index) {
            SakuraTerminalDirtySpan span {};
            if (sakura_terminal_frame_dirty_span(frame, index, &span))
                result.dirty_spans.push_back(span);
        }
        auto snapshot = std::make_shared<TestSnapshot>();
        snapshot->columns = info.columns;
        snapshot->rows = info.rows;
        snapshot->cursor_x = info.cursor_x;
        snapshot->cursor_y = info.cursor_y;
        snapshot->cursor_visible = info.cursor_visible != 0;
        snapshot->cursor_style = info.cursor_style;
        snapshot->alternate_screen = info.alternate_screen != 0;
        snapshot->cells.resize(static_cast<std::size_t>(info.columns) *
                               info.rows);
        for (unsigned int row = 0; row < info.rows; ++row) {
            for (unsigned int column = 0; column < info.columns; ++column) {
                SakuraTerminalCellView cell {};
                if (!sakura_terminal_frame_cell(frame, column, row, &cell))
                    continue;
                TestCell& destination = snapshot->cells[
                    row * info.columns + column];
                destination.codepoint = cell.codepoint;
                destination.text.assign(cell.text == nullptr ? "" : cell.text,
                                        cell.text_length);
                destination.width = cell.width;
                for (std::size_t index = 0; index < 3; ++index) {
                    destination.foreground[index] = cell.foreground[index];
                    destination.background[index] = cell.background[index];
                }
                destination.attributes = cell.attributes;
            }
        }
        result.snapshot = snapshot;
        sakura_terminal_frame_free(frame);
        return result;
    }

    TestSnapshot TakeSnapshot()
    {
        const auto snapshot = TakeFrame().snapshot;
        return snapshot == nullptr ? TestSnapshot {} : *snapshot;
    }

    SakuraTerminalMetrics GetMetrics() const
    {
        SakuraTerminalMetrics metrics {};
        sakura_terminal_get_metrics(terminal_, &metrics);
        return metrics;
    }

private:
    static void WriteBridge(void* userdata, const char* data, std::size_t length)
    {
        auto* self = static_cast<TestTerminal*>(userdata);
        if (self != nullptr && self->callback_)
            self->callback_(data, length);
    }

    WriteCallback callback_;
    SakuraTerminal* terminal_ = nullptr;
};
