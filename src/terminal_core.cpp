#include <sakura/terminal/core_c.h>

#include <tsm/libtsm.h>

#include "tsm_scroll_observer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct PackedCell {
    uint32_t codepoint = ' ';
    uint32_t text_offset = 0;
    uint32_t text_length = 0;
    uint16_t style_index = 0;
    uint8_t width = 1;
    uint8_t flags = 0;
};

struct PackedStyle {
    std::array<uint8_t, 3> foreground {};
    std::array<uint8_t, 3> background {};
    uint8_t attributes = 0;
};

struct PackedRun {
    unsigned int left = 0;
    unsigned int cell_count = 0;
    uint16_t style_index = 0;
    uint32_t text_offset = 0;
    uint32_t text_length = 0;
};

struct PackedRow {
    std::vector<PackedCell> cells;
    std::string text;
    std::vector<PackedRun> runs;
};

struct PackedGrid {
    unsigned int columns = 0;
    unsigned int rows = 0;
    std::vector<PackedStyle> styles;
    std::vector<std::shared_ptr<const PackedRow>> row_data;
};

constexpr unsigned int kPackedRunSpanMaxCells =
    SAKURA_TERMINAL_RUN_SPAN_MAX_CELLS;

struct PackedFrame {
    uint64_t generation = 0;
    bool changed = false;
    bool full_repaint = false;
    int scroll_delta = 0;
    SakuraTerminalDirtyRegion dirty {};
    std::vector<SakuraTerminalDirtySpan> dirty_spans;
    unsigned int columns = 0;
    unsigned int rows = 0;
    unsigned int cursor_x = 0;
    unsigned int cursor_y = 0;
    bool cursor_visible = false;
    SakuraTerminalCursorStyle cursor_style = SAKURA_TERMINAL_CURSOR_BLOCK;
    bool alternate_screen = false;
    std::shared_ptr<const PackedGrid> grid;
};

class TerminalBackend final {
public:
    using WriteCallback = std::function<void(const char*, std::size_t)>;

    explicit TerminalBackend(WriteCallback write_callback);
    ~TerminalBackend();

    TerminalBackend(const TerminalBackend&) = delete;
    TerminalBackend& operator=(const TerminalBackend&) = delete;
    TerminalBackend(TerminalBackend&& other) noexcept;
    TerminalBackend& operator=(TerminalBackend&& other) noexcept;

    bool IsReady() const;
    const std::string& Error() const;
    const std::string& Title() const;
    bool Resize(unsigned int columns, unsigned int rows);
    void FeedOutput(const char* data, std::size_t length);
    bool HandleKey(uint32_t keysym, uint32_t ascii, unsigned int modifiers,
                   uint32_t unicode);
    void Paste(const std::string& text);
    bool HandleMouse(unsigned int cell_x, unsigned int cell_y,
                     unsigned int pixel_x, unsigned int pixel_y,
                     unsigned int button, unsigned int event,
                     unsigned char modifiers);
    bool MouseReportingEnabled() const;
    void ScrollPageUp(unsigned int pages);
    void ScrollPageDown(unsigned int pages);
    void ScrollLines(int lines);
    void StartSelection(unsigned int column, unsigned int row);
    void UpdateSelection(unsigned int column, unsigned int row);
    void SelectWord(unsigned int column, unsigned int row);
    void SelectLine(unsigned int row);
    void ClearSelection();
    bool HasSelection() const;
    std::string CopySelection();
    std::shared_ptr<const PackedFrame> TakePackedFrame();
    SakuraTerminalMetrics GetMetrics() const;
    void RebindOwnerThread();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class TerminalBackend::Impl {
public:
    explicit Impl(TerminalBackend::WriteCallback write_callback)
        : write_callback_(std::move(write_callback))
    {
        if (tsm_screen_new(&screen_, nullptr, nullptr) != 0) {
            error_ = "libtsm could not create a screen";
            return;
        }

        tsm_screen_set_max_sb(screen_, 2000);
        if (tsm_vte_new(&vte_, screen_, &Impl::VteWrite, this,
                        nullptr, nullptr) != 0) {
            error_ = "libtsm could not create a VTE";
            tsm_screen_unref(screen_);
            screen_ = nullptr;
            return;
        }

        tsm_vte_set_mouse_cb(vte_, &Impl::VteMouse, this);
        tsm_vte_set_osc_cb(vte_, &Impl::VteOsc, this);
        tsm_vte_set_palette(vte_, "base16-dark");
        ResetScreenObservation();
    }

    ~Impl()
    {
        if (vte_ != nullptr)
            tsm_vte_unref(vte_);
        if (screen_ != nullptr)
            tsm_screen_unref(screen_);
    }

    bool IsReady() const
    {
        return screen_ != nullptr && vte_ != nullptr;
    }

    void AssertOwnerThread() const
    {
        assert(std::this_thread::get_id() == owner_thread_);
    }

    void RebindOwnerThread()
    {
        owner_thread_ = std::this_thread::get_id();
    }

    void FeedOutput(const char* data, std::size_t length)
    {
        if (vte_ != nullptr && data != nullptr && length > 0) {
            if (observed_lines_.size() != sakura_tsm_screen_height(screen_))
                ResetScreenObservation();
            const std::vector<std::uintptr_t> previous_lines = observed_lines_;
            const bool previous_alternate = observed_alternate_screen_;
            metrics_.output_bytes += length;
            ++metrics_.output_chunks;
            last_output_time_ = std::chrono::steady_clock::now();
            output_waiting_for_render_ = true;
            TrackCursorStyle(data, length);
            tsm_vte_input(vte_, data, length);
            ObserveScreenScroll(previous_lines, previous_alternate);
        }
    }

private:
    struct PackedChangedCell {
        unsigned int column = 0;
        unsigned int row = 0;
        unsigned int width = 1;
        uint32_t codepoint = ' ';
        uint16_t style_index = 0;
        uint32_t text_offset = 0;
        uint32_t text_length = 0;
    };

    struct PackedDrawContext {
        Impl* impl = nullptr;
        PackedGrid* grid = nullptr;
        std::vector<PackedChangedCell> changed_cells;
        std::string changed_text;
        std::vector<SakuraTerminalDirtySpan> dirty_spans;
        SakuraTerminalDirtyRegion dirty {};
        tsm_age_t previous_age = 0;
        bool full_repaint = false;
        bool cursor_changed = false;
        unsigned int previous_cursor_x = 0;
        unsigned int previous_cursor_y = 0;
        unsigned int cursor_x = 0;
        unsigned int cursor_y = 0;
        uint64_t* decoded_cells = nullptr;
        bool reuse_scrolled_rows = false;
    };

    void ResetScreenObservation()
    {
        observed_lines_.clear();
        if (screen_ == nullptr)
            return;
        const unsigned int height = sakura_tsm_screen_height(screen_);
        observed_lines_.resize(height);
        for (unsigned int row = 0; row < height; ++row)
            observed_lines_[row] = reinterpret_cast<std::uintptr_t>(
                sakura_tsm_screen_line(screen_, row));
        observed_alternate_screen_ = sakura_tsm_screen_is_alternate(screen_) != 0;
    }

    static int DetectFullScreenShift(
        const std::vector<std::uintptr_t>& previous_lines,
        const struct tsm_screen* screen)
    {
        const unsigned int height = sakura_tsm_screen_height(screen);
        if (screen == nullptr || previous_lines.size() != height || height < 2)
            return 0;

        for (unsigned int delta = 1; delta < height; ++delta) {
            bool moved_up = true;
            bool moved_down = true;
            for (unsigned int row = 0; row < height - delta; ++row) {
                if (reinterpret_cast<std::uintptr_t>(
                        sakura_tsm_screen_line(screen, row)) !=
                    previous_lines[row + delta])
                    moved_up = false;
                if (reinterpret_cast<std::uintptr_t>(
                        sakura_tsm_screen_line(screen, row + delta)) !=
                    previous_lines[row])
                    moved_down = false;
                if (!moved_up && !moved_down)
                    break;
            }
            if (moved_up)
                return static_cast<int>(delta);
            if (moved_down)
                return -static_cast<int>(delta);
        }
        return 0;
    }

    void ObserveScreenScroll(
        const std::vector<std::uintptr_t>& previous_lines,
        bool previous_alternate)
    {
        const bool alternate = screen_ != nullptr &&
            sakura_tsm_screen_is_alternate(screen_) != 0;
        if (screen_ != nullptr && previous_alternate == alternate &&
            !sakura_tsm_screen_is_scrollback(screen_)) {
            const int delta = DetectFullScreenShift(previous_lines, screen_);
            if (delta != 0)
                pending_content_scroll_delta_ += delta;
        }
        ResetScreenObservation();
    }

    static bool DirtyEmpty(const SakuraTerminalDirtyRegion& dirty)
    {
        return dirty.left >= dirty.right || dirty.top >= dirty.bottom;
    }

    static void IncludeDirty(SakuraTerminalDirtyRegion& dirty,
                             unsigned int column, unsigned int row,
                             unsigned int width, unsigned int columns,
                             unsigned int rows)
    {
        if (columns == 0 || rows == 0 || row >= rows || column >= columns)
            return;
        unsigned int left = column;
        if (width == 0 && left > 0)
            --left;
        const unsigned int span = width == 0 ? 2 : std::max(1u, width);
        const unsigned int right = std::min(columns, column + span);
        if (DirtyEmpty(dirty)) {
            dirty.left = left;
            dirty.top = row;
            dirty.right = right;
            dirty.bottom = row + 1;
        } else {
            dirty.left = std::min(dirty.left, left);
            dirty.top = std::min(dirty.top, row);
            dirty.right = std::max(dirty.right, right);
            dirty.bottom = std::max(dirty.bottom, row + 1);
        }
    }

    static void IncludeCell(SakuraTerminalDirtyRegion& dirty,
                            unsigned int column, unsigned int row,
                            unsigned int columns, unsigned int rows)
    {
        IncludeDirty(dirty, column, row, 1, columns, rows);
    }

    static void IncludeSpan(std::vector<SakuraTerminalDirtySpan>& spans,
                            unsigned int row, unsigned int left,
                            unsigned int right)
    {
        if (left >= right)
            return;
        if (!spans.empty()) {
            SakuraTerminalDirtySpan& previous = spans.back();
            if (previous.row == row && left <= previous.right) {
                previous.left = std::min(previous.left, left);
                previous.right = std::max(previous.right, right);
                return;
            }
        }
        spans.push_back({row, left, right});
    }

    static void IncludeCellSpan(std::vector<SakuraTerminalDirtySpan>& spans,
                                unsigned int column, unsigned int row,
                                unsigned int width, unsigned int columns,
                                unsigned int rows)
    {
        if (columns == 0 || rows == 0 || row >= rows || column >= columns)
            return;
        unsigned int left = column;
        if (width == 0 && left > 0)
            --left;
        const unsigned int span = width == 0 ? 2 : std::max(1u, width);
        const unsigned int right = std::min(columns, column + span);
        IncludeSpan(spans, row, left, right);
    }

    static void IncludeFullSpans(std::vector<SakuraTerminalDirtySpan>& spans,
                                 unsigned int columns, unsigned int rows)
    {
        spans.clear();
        if (columns == 0)
            return;
        spans.reserve(rows);
        for (unsigned int row = 0; row < rows; ++row)
            spans.push_back({row, 0, columns});
    }

    static PackedStyle PackedStyleFor(const struct tsm_screen_attr* attr)
    {
        PackedStyle style;
        style.foreground = {attr->fr, attr->fg, attr->fb};
        style.background = {attr->br, attr->bg, attr->bb};
        if (attr->inverse)
            std::swap(style.foreground, style.background);
        if (attr->dim) {
            for (std::size_t index = 0; index < style.foreground.size(); ++index)
                style.foreground[index] = static_cast<uint8_t>(
                    style.background[index] / 2 + style.foreground[index] / 2);
        }
        if (attr->bold) style.attributes |= 0x01;
        if (attr->italic) style.attributes |= 0x02;
        if (attr->underline) style.attributes |= 0x04;
        if (attr->blink) style.attributes |= 0x08;
        if (attr->dim) style.attributes |= 0x10;
        return style;
    }

    static uint16_t StyleIndex(PackedGrid& grid, const PackedStyle& style)
    {
        for (std::size_t index = 0; index < grid.styles.size(); ++index) {
            const PackedStyle& existing = grid.styles[index];
            if (existing.foreground == style.foreground &&
                existing.background == style.background &&
                existing.attributes == style.attributes)
                return static_cast<uint16_t>(index);
        }
        if (grid.styles.size() >= std::numeric_limits<uint16_t>::max())
            return 0;
        grid.styles.push_back(style);
        return static_cast<uint16_t>(grid.styles.size() - 1);
    }

    static std::shared_ptr<PackedRow> BlankRow(unsigned int columns)
    {
        auto row = std::make_shared<PackedRow>();
        row->cells.resize(columns);
        row->text.assign(columns, ' ');
        for (unsigned int column = 0; column < columns; ++column) {
            row->cells[column].codepoint = ' ';
            row->cells[column].text_offset = column;
            row->cells[column].text_length = 1;
            row->cells[column].style_index = 0;
            row->cells[column].width = 1;
        }
        for (unsigned int column = 0; column < columns; ++column) {
            if (row->runs.empty() ||
                row->runs.back().cell_count >= kPackedRunSpanMaxCells) {
                row->runs.push_back({column, 1, 0,
                                     row->cells[column].text_offset,
                                     row->cells[column].text_length});
            } else {
                PackedRun& run = row->runs.back();
                ++run.cell_count;
                const uint32_t end = row->cells[column].text_offset +
                    row->cells[column].text_length;
                run.text_length = end - run.text_offset;
            }
        }
        return row;
    }

    static void EnsurePackedGrid(PackedGrid& grid, unsigned int columns,
                                 unsigned int rows)
    {
        if (grid.styles.empty())
            grid.styles.push_back({});
        if (grid.columns == columns && grid.rows == rows &&
            grid.row_data.size() == rows)
            return;
        grid.columns = columns;
        grid.rows = rows;
        grid.row_data.clear();
        grid.row_data.reserve(rows);
        for (unsigned int row = 0; row < rows; ++row)
            grid.row_data.push_back(BlankRow(columns));
    }

    static void RotatePackedRows(PackedGrid& grid, int delta)
    {
        if (delta == 0 || grid.rows == 0)
            return;
        std::vector<std::shared_ptr<const PackedRow>> rotated(grid.rows);
        for (unsigned int row = 0; row < grid.rows; ++row) {
            const int source = static_cast<int>(row) + delta;
            if (source >= 0 && source < static_cast<int>(grid.rows))
                rotated[row] = grid.row_data[static_cast<unsigned int>(source)];
            else
                rotated[row] = BlankRow(grid.columns);
        }
        grid.row_data = std::move(rotated);
    }

    static void IncludeRows(SakuraTerminalDirtyRegion& dirty,
                            std::vector<SakuraTerminalDirtySpan>& spans,
                            unsigned int top, unsigned int bottom,
                            unsigned int columns, unsigned int rows)
    {
        top = std::min(top, rows);
        bottom = std::min(bottom, rows);
        if (columns == 0 || top >= bottom)
            return;
        if (DirtyEmpty(dirty)) {
            dirty.left = 0;
            dirty.top = top;
            dirty.right = columns;
            dirty.bottom = bottom;
        } else {
            dirty.left = 0;
            dirty.top = std::min(dirty.top, top);
            dirty.right = std::max(dirty.right, columns);
            dirty.bottom = std::max(dirty.bottom, bottom);
        }
        for (unsigned int row = top; row < bottom; ++row)
            IncludeSpan(spans, row, 0, columns);
    }

    static int DrawPackedCell(struct tsm_screen*, uint64_t,
                              const uint32_t* codepoints, std::size_t length,
                              unsigned int width, unsigned int column,
                              unsigned int row,
                              const struct tsm_screen_attr* attr,
                              tsm_age_t age, void* user_data)
    {
        auto* context = static_cast<PackedDrawContext*>(user_data);
        if (context == nullptr || context->impl == nullptr ||
            context->grid == nullptr || attr == nullptr ||
            column >= context->grid->columns || row >= context->grid->rows)
            return 0;

        const bool cursor_cell = context->cursor_changed &&
            ((column == context->previous_cursor_x &&
              row == context->previous_cursor_y) ||
             (column == context->cursor_x && row == context->cursor_y));
        const bool changed = context->full_repaint || age == 0 ||
            age > context->previous_age || cursor_cell;
        if (!changed)
            return 0;

        const PackedStyle style = PackedStyleFor(attr);
        const uint16_t style_index = StyleIndex(*context->grid, style);
        std::array<char, 128> encoded_text {};
        std::size_t encoded_length = 0;
        for (std::size_t index = 0; index < length; ++index) {
            char utf8[8] {};
            const std::size_t utf8_length = tsm_ucs4_to_utf8(
                codepoints[index], utf8);
            if (encoded_length + utf8_length > encoded_text.size())
                return 0;
            std::memcpy(encoded_text.data() + encoded_length,
                        utf8, utf8_length);
            encoded_length += utf8_length;
        }
        if (length == 0 && width > 0)
            encoded_text[encoded_length++] = ' ';

        if (context->reuse_scrolled_rows &&
            row < context->grid->row_data.size() &&
            context->grid->row_data[row] != nullptr) {
            const PackedRow& cached_row = *context->grid->row_data[row];
            if (column < cached_row.cells.size()) {
                const PackedCell& cached_cell = cached_row.cells[column];
                const bool text_valid =
                    cached_cell.text_offset <= cached_row.text.size() &&
                    cached_cell.text_length <= cached_row.text.size() -
                                               cached_cell.text_offset;
                const bool text_matches = text_valid &&
                    cached_cell.text_length == encoded_length &&
                    (encoded_length == 0 || std::memcmp(
                        cached_row.text.data() + cached_cell.text_offset,
                        encoded_text.data(), encoded_length) == 0);
                if (cached_cell.style_index == style_index &&
                    cached_cell.width == width &&
                    cached_cell.codepoint ==
                        (length == 0 ? (width == 0 ? 0 : ' ') : codepoints[0]) &&
                    text_matches)
                    return 0;
            }
        }

        if (context->decoded_cells != nullptr)
            ++*context->decoded_cells;
        IncludeDirty(context->dirty, column, row, width,
                     context->grid->columns, context->grid->rows);
        IncludeCellSpan(context->dirty_spans, column, row, width,
                        context->grid->columns, context->grid->rows);

        PackedChangedCell changed_cell;
        changed_cell.column = column;
        changed_cell.row = row;
        changed_cell.width = width;
        changed_cell.codepoint = length == 0
            ? (width == 0 ? 0 : ' ')
            : codepoints[0];
        changed_cell.style_index = style_index;
        changed_cell.text_offset = static_cast<uint32_t>(
            context->changed_text.size());
        context->changed_text.append(encoded_text.data(), encoded_length);
        changed_cell.text_length = static_cast<uint32_t>(
            context->changed_text.size() - changed_cell.text_offset);
        context->changed_cells.push_back(changed_cell);
        return 0;
    }

    static void RebuildPackedRows(PackedGrid& grid,
                                  std::vector<PackedChangedCell>& changes,
                                  const std::string& changed_text)
    {
        if (changes.empty())
            return;
        std::sort(changes.begin(), changes.end(),
                  [](const PackedChangedCell& left,
                     const PackedChangedCell& right) {
                      if (left.row != right.row)
                          return left.row < right.row;
                      return left.column < right.column;
                  });

        std::size_t change_index = 0;
        while (change_index < changes.size()) {
            const unsigned int row_index = changes[change_index].row;
            const std::shared_ptr<const PackedRow> old_row =
                grid.row_data[row_index];
            auto row = std::make_shared<PackedRow>();
            row->cells.resize(grid.columns);
            row->text.reserve(old_row == nullptr ? grid.columns
                                                   : old_row->text.size());
            std::size_t row_change = change_index;
            while (row_change < changes.size() &&
                   changes[row_change].row == row_index)
                ++row_change;

            std::size_t current_change = change_index;
            for (unsigned int column = 0; column < grid.columns; ++column) {
                PackedCell cell;
                const char* source_text = nullptr;
                std::size_t source_length = 0;
                if (current_change < row_change &&
                    changes[current_change].column == column) {
                    const PackedChangedCell& change = changes[current_change];
                    cell.codepoint = change.codepoint;
                    cell.style_index = change.style_index;
                    cell.width = static_cast<uint8_t>(change.width);
                    source_text = changed_text.data() + change.text_offset;
                    source_length = change.text_length;
                    ++current_change;
                } else if (old_row != nullptr && column < old_row->cells.size()) {
                    const PackedCell& old_cell = old_row->cells[column];
                    cell = old_cell;
                    if (old_cell.text_offset <= old_row->text.size() &&
                        old_cell.text_length <= old_row->text.size() -
                                                  old_cell.text_offset) {
                        source_text = old_row->text.data() + old_cell.text_offset;
                        source_length = old_cell.text_length;
                    }
                }
                cell.text_offset = static_cast<uint32_t>(row->text.size());
                cell.text_length = static_cast<uint32_t>(source_length);
                if (source_text != nullptr && source_length > 0)
                    row->text.append(source_text, source_length);
                row->cells[column] = cell;
            }

            for (unsigned int column = 0; column < grid.columns; ++column) {
                const PackedCell& cell = row->cells[column];
                const bool starts_wide_glyph = cell.width > 1;
                const bool would_end_with_wide_glyph =
                    !row->runs.empty() && starts_wide_glyph &&
                    row->runs.back().cell_count + 1 ==
                        kPackedRunSpanMaxCells;
                if (row->runs.empty() ||
                    row->runs.back().style_index != cell.style_index ||
                    row->runs.back().cell_count >= kPackedRunSpanMaxCells ||
                    would_end_with_wide_glyph) {
                    row->runs.push_back({column, 1, cell.style_index,
                                         cell.text_offset, cell.text_length});
                } else {
                    PackedRun& run = row->runs.back();
                    ++run.cell_count;
                    const uint32_t end = cell.text_offset + cell.text_length;
                    run.text_length = end - run.text_offset;
                }
            }
            grid.row_data[row_index] = row;
            change_index = row_change;
        }
    }

    static void VteWrite(struct tsm_vte*, const char* data,
                         std::size_t length, void* user_data)
    {
        auto* impl = static_cast<Impl*>(user_data);
        impl->metrics_.transport_write_bytes += length;
        ++impl->metrics_.transport_write_events;
        if (impl->write_callback_)
            impl->write_callback_(data, length);
    }

    static void VteMouse(struct tsm_vte*,
                         enum tsm_mouse_track_mode,
                         bool, void* user_data)
    {
        auto* impl = static_cast<Impl*>(user_data);
        ++impl->metrics_.mouse_mode_changes;
    }

    static void VteOsc(struct tsm_vte*, const char* data,
                       std::size_t length, void* user_data)
    {
        auto* impl = static_cast<Impl*>(user_data);
        if (data == nullptr || length < 2)
            return;
        const std::string sequence(data, length);
        const std::size_t separator = sequence.find(';');
        if (separator == std::string::npos)
            return;
        const std::string command = sequence.substr(0, separator);
        if (command == "0" || command == "1" || command == "2") {
            impl->title_ = sequence.substr(separator + 1);
            ++impl->metrics_.title_changes;
        }
    }

    void TrackCursorStyle(const char* data, std::size_t length)
    {
        for (std::size_t index = 0; index < length; ++index) {
            const char ch = data[index];
            switch (cursor_sequence_state_) {
            case CursorSequenceState::Ground:
                if (ch == '\x1b')
                    cursor_sequence_state_ = CursorSequenceState::Escape;
                break;
            case CursorSequenceState::Escape:
                if (ch == '[') {
                    cursor_sequence_parameters_.clear();
                    cursor_sequence_state_ = CursorSequenceState::Csi;
                } else if (ch != '\x1b') {
                    cursor_sequence_state_ = CursorSequenceState::Ground;
                }
                break;
            case CursorSequenceState::Csi:
                if ((ch >= '0' && ch <= '9') || ch == ';') {
                    if (cursor_sequence_parameters_.size() < 16)
                        cursor_sequence_parameters_.push_back(ch);
                } else if (ch == ' ') {
                    cursor_sequence_state_ = CursorSequenceState::CsiIntermediate;
                } else {
                    cursor_sequence_state_ = ch == '\x1b'
                        ? CursorSequenceState::Escape
                        : CursorSequenceState::Ground;
                }
                break;
            case CursorSequenceState::CsiIntermediate:
                if (ch == 'q') {
                    unsigned int value = 0;
                    if (!cursor_sequence_parameters_.empty())
                        value = static_cast<unsigned int>(
                            std::strtoul(cursor_sequence_parameters_.c_str(),
                                         nullptr, 10));

                    SakuraTerminalCursorStyle style = cursor_style_;
                    switch (value) {
                    case 0:
                    case 1:
                    case 2:
                        style = SAKURA_TERMINAL_CURSOR_BLOCK;
                        break;
                    case 3:
                    case 4:
                        style = SAKURA_TERMINAL_CURSOR_UNDERLINE;
                        break;
                    case 5:
                    case 6:
                        style = SAKURA_TERMINAL_CURSOR_BAR;
                        break;
                    default:
                        break;
                    }
                    if (style != cursor_style_) {
                        cursor_style_ = style;
                        ++metrics_.cursor_style_changes;
                    }
                    cursor_sequence_state_ = CursorSequenceState::Ground;
                } else {
                    cursor_sequence_state_ = ch == '\x1b'
                        ? CursorSequenceState::Escape
                        : CursorSequenceState::Ground;
                }
                break;
            }
        }
    }

    std::shared_ptr<const PackedFrame> TakePackedFrame()
    {
        if (screen_ == nullptr)
            return {};

        ++metrics_.rendered_frames;
        if (output_waiting_for_render_) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - last_output_time_).count();
            const uint64_t latency = static_cast<uint64_t>(std::max<int64_t>(0, elapsed));
            ++metrics_.render_latency_samples;
            metrics_.max_render_latency_us = std::max(
                metrics_.max_render_latency_us, latency);
            output_waiting_for_render_ = false;
        }

        if (!packed_grid_cache_ || packed_grid_cache_.use_count() > 1) {
            packed_grid_cache_ = packed_grid_cache_
                ? std::make_shared<PackedGrid>(*packed_grid_cache_)
                : std::make_shared<PackedGrid>();
        }
        PackedGrid& grid = *packed_grid_cache_;
        const unsigned int columns = tsm_screen_get_width(screen_);
        const unsigned int rows = tsm_screen_get_height(screen_);
        EnsurePackedGrid(grid, columns, rows);

        const int content_scroll_delta = pending_content_scroll_delta_;
        pending_content_scroll_delta_ = 0;
        const int viewport_scroll_delta = pending_viewport_scroll_delta_;
        pending_viewport_scroll_delta_ = 0;

        auto frame = std::make_shared<PackedFrame>();
        frame->columns = columns;
        frame->rows = rows;
        frame->cursor_x = tsm_screen_get_cursor_x(screen_);
        frame->cursor_y = tsm_screen_get_cursor_y(screen_);
        frame->cursor_visible = vte_ != nullptr &&
            (tsm_vte_get_flags(vte_) & TSM_VTE_FLAG_TEXT_CURSOR_MODE) != 0;
        frame->cursor_style = cursor_style_;
        frame->alternate_screen = (tsm_screen_get_flags(screen_) &
                                   TSM_SCREEN_ALTERNATE) != 0;
        frame->scroll_delta = content_scroll_delta != 0
            ? content_scroll_delta : viewport_scroll_delta;
        frame->full_repaint = !packed_frame_valid_ ||
            (columns != packed_last_columns_ || rows != packed_last_rows_ ||
             frame->alternate_screen != packed_last_alternate_screen_);

        const bool reuse_scrolled_rows = !frame->full_repaint &&
            content_scroll_delta != 0 && viewport_scroll_delta == 0 &&
            !sakura_tsm_screen_is_scrollback(screen_) &&
            std::abs(content_scroll_delta) < static_cast<int>(rows);
        if (content_scroll_delta != 0 && !reuse_scrolled_rows)
            frame->full_repaint = true;
        if (reuse_scrolled_rows)
            RotatePackedRows(grid, content_scroll_delta);

        PackedDrawContext context;
        context.impl = this;
        context.grid = &grid;
        context.previous_age = packed_last_draw_age_;
        context.full_repaint = frame->full_repaint;
        context.cursor_changed = packed_frame_valid_ &&
            (frame->cursor_x != packed_last_cursor_x_ ||
             frame->cursor_y != packed_last_cursor_y_ ||
             frame->cursor_visible != packed_last_cursor_visible_ ||
             frame->cursor_style != packed_last_cursor_style_);
        context.previous_cursor_x = packed_last_cursor_x_;
        context.previous_cursor_y = packed_last_cursor_y_;
        context.cursor_x = frame->cursor_x;
        context.cursor_y = frame->cursor_y;
        context.decoded_cells = &metrics_.frame_cells_decoded;
        context.reuse_scrolled_rows = reuse_scrolled_rows;
        const uint64_t decoded_before = metrics_.frame_cells_decoded;
        const tsm_age_t since = frame->full_repaint ? 0 : packed_last_draw_age_;
        const tsm_age_t age = tsm_screen_draw_since(
            screen_, since, &Impl::DrawPackedCell, &context);
        const uint64_t decoded_cells = metrics_.frame_cells_decoded -
                                       decoded_before;
        const uint64_t total_cells = static_cast<uint64_t>(columns) * rows;
        if (decoded_cells < total_cells)
            metrics_.frame_cells_reused += total_cells - decoded_cells;

        if (context.cursor_changed) {
            IncludeCell(context.dirty, packed_last_cursor_x_,
                        packed_last_cursor_y_, columns, rows);
            IncludeCell(context.dirty, frame->cursor_x, frame->cursor_y,
                        columns, rows);
            IncludeCellSpan(context.dirty_spans, packed_last_cursor_x_,
                            packed_last_cursor_y_, 1, columns, rows);
            IncludeCellSpan(context.dirty_spans, frame->cursor_x,
                            frame->cursor_y, 1, columns, rows);
        }

        if (reuse_scrolled_rows) {
            const unsigned int distance = static_cast<unsigned int>(
                std::abs(content_scroll_delta));
            if (content_scroll_delta > 0)
                IncludeRows(context.dirty, context.dirty_spans,
                            rows - distance, rows, columns, rows);
            else
                IncludeRows(context.dirty, context.dirty_spans,
                            0, distance, columns, rows);
        }

        if (age == 0) {
            frame->full_repaint = true;
            context.dirty.left = 0;
            context.dirty.top = 0;
            context.dirty.right = columns;
            context.dirty.bottom = rows;
        }
        if (frame->full_repaint && DirtyEmpty(context.dirty)) {
            context.dirty.left = 0;
            context.dirty.top = 0;
            context.dirty.right = columns;
            context.dirty.bottom = rows;
        }
        if (!DirtyEmpty(context.dirty) && context.dirty.left == 0 &&
            context.dirty.top == 0 && context.dirty.right == columns &&
            context.dirty.bottom == rows)
            frame->full_repaint = true;
        if (frame->full_repaint)
            IncludeFullSpans(context.dirty_spans, columns, rows);

        RebuildPackedRows(grid, context.changed_cells, context.changed_text);
        frame->dirty = {context.dirty.left, context.dirty.top,
                        context.dirty.right, context.dirty.bottom};
        frame->dirty_spans = std::move(context.dirty_spans);
        frame->changed = frame->full_repaint || !DirtyEmpty(frame->dirty);
        frame->generation = frame->changed ? ++packed_generation_
                                           : packed_generation_;
        frame->grid = packed_grid_cache_;

        packed_frame_valid_ = age != 0;
        packed_last_draw_age_ = age;
        packed_last_columns_ = columns;
        packed_last_rows_ = rows;
        packed_last_alternate_screen_ = frame->alternate_screen;
        packed_last_cursor_x_ = frame->cursor_x;
        packed_last_cursor_y_ = frame->cursor_y;
        packed_last_cursor_visible_ = frame->cursor_visible;
        packed_last_cursor_style_ = frame->cursor_style;
        return frame;
    }

    enum class CursorSequenceState {
        Ground,
        Escape,
        Csi,
        CsiIntermediate,
    } cursor_sequence_state_ = CursorSequenceState::Ground;
    std::string cursor_sequence_parameters_;

    TerminalBackend::WriteCallback write_callback_;
    struct tsm_screen* screen_ = nullptr;
    struct tsm_vte* vte_ = nullptr;
    std::string error_;
    std::string title_;
    SakuraTerminalCursorStyle cursor_style_ = SAKURA_TERMINAL_CURSOR_BLOCK;
    bool selection_active_ = false;
    SakuraTerminalMetrics metrics_ {};
    std::chrono::steady_clock::time_point last_output_time_;
    bool output_waiting_for_render_ = false;
    std::thread::id owner_thread_ = std::this_thread::get_id();
    std::vector<std::uintptr_t> observed_lines_;
    bool observed_alternate_screen_ = false;
    int pending_content_scroll_delta_ = 0;
    int pending_viewport_scroll_delta_ = 0;
    std::shared_ptr<PackedGrid> packed_grid_cache_;
    bool packed_frame_valid_ = false;
    tsm_age_t packed_last_draw_age_ = 0;
    uint64_t packed_generation_ = 0;
    unsigned int packed_last_columns_ = 0;
    unsigned int packed_last_rows_ = 0;
    unsigned int packed_last_cursor_x_ = 0;
    unsigned int packed_last_cursor_y_ = 0;
    bool packed_last_cursor_visible_ = false;
    bool packed_last_alternate_screen_ = false;
    SakuraTerminalCursorStyle packed_last_cursor_style_ =
        SAKURA_TERMINAL_CURSOR_BLOCK;

    friend class TerminalBackend;
};

TerminalBackend::TerminalBackend(WriteCallback write_callback)
    : impl_(std::make_unique<Impl>(std::move(write_callback)))
{
}

TerminalBackend::~TerminalBackend() = default;

TerminalBackend::TerminalBackend(TerminalBackend&& other) noexcept
    : impl_(std::move(other.impl_))
{
    if (impl_ != nullptr)
        impl_->RebindOwnerThread();
}

TerminalBackend& TerminalBackend::operator=(TerminalBackend&& other) noexcept
{
    if (this == &other)
        return *this;
    impl_ = std::move(other.impl_);
    if (impl_ != nullptr)
        impl_->RebindOwnerThread();
    return *this;
}

bool TerminalBackend::IsReady() const
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ != nullptr && impl_->IsReady();
}

const std::string& TerminalBackend::Error() const
{
    static const std::string empty;
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ == nullptr ? empty : impl_->error_;
}

const std::string& TerminalBackend::Title() const
{
    static const std::string empty;
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ == nullptr ? empty : impl_->title_;
}

bool TerminalBackend::Resize(unsigned int columns, unsigned int rows)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    const bool resized = impl_ != nullptr && impl_->screen_ != nullptr &&
        tsm_screen_resize(impl_->screen_, columns, rows) == 0;
    if (resized) {
        impl_->ResetScreenObservation();
        impl_->pending_content_scroll_delta_ = 0;
        impl_->pending_viewport_scroll_delta_ = 0;
    }
    return resized;
}

void TerminalBackend::FeedOutput(const char* data, std::size_t length)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr)
        impl_->FeedOutput(data, length);
}

bool TerminalBackend::HandleKey(uint32_t keysym, uint32_t ascii,
                             unsigned int modifiers, uint32_t unicode)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr)
        return false;
    ++impl_->metrics_.input_events;
    return impl_->vte_ != nullptr &&
           tsm_vte_handle_keyboard(impl_->vte_, keysym, ascii, modifiers, unicode);
}

void TerminalBackend::Paste(const std::string& text)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr || impl_->vte_ == nullptr || text.empty())
        return;
    impl_->metrics_.paste_bytes += text.size();
    tsm_vte_paste(impl_->vte_, text.c_str());
}

bool TerminalBackend::HandleMouse(unsigned int cell_x, unsigned int cell_y,
                               unsigned int pixel_x, unsigned int pixel_y,
                               unsigned int button, unsigned int event,
                               unsigned char modifiers)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr || impl_->vte_ == nullptr)
        return false;
    ++impl_->metrics_.mouse_events;
    const bool handled = tsm_vte_handle_mouse(
        impl_->vte_, cell_x, cell_y, pixel_x, pixel_y, button, event, modifiers);
    if (handled)
        ++impl_->metrics_.mouse_events_forwarded;
    return handled;
}

bool TerminalBackend::MouseReportingEnabled() const
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ != nullptr && impl_->vte_ != nullptr &&
           tsm_vte_get_mouse_mode(impl_->vte_) != 0;
}

void TerminalBackend::ScrollPageUp(unsigned int pages)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr && impl_->screen_ != nullptr) {
        const unsigned int before = tsm_screen_sb_get_line_pos(impl_->screen_);
        tsm_screen_sb_page_up(impl_->screen_, pages);
        const unsigned int after = tsm_screen_sb_get_line_pos(impl_->screen_);
        impl_->pending_viewport_scroll_delta_ +=
            static_cast<int>(after) - static_cast<int>(before);
    }
}

void TerminalBackend::ScrollPageDown(unsigned int pages)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr && impl_->screen_ != nullptr) {
        const unsigned int before = tsm_screen_sb_get_line_pos(impl_->screen_);
        tsm_screen_sb_page_down(impl_->screen_, pages);
        const unsigned int after = tsm_screen_sb_get_line_pos(impl_->screen_);
        impl_->pending_viewport_scroll_delta_ +=
            static_cast<int>(after) - static_cast<int>(before);
    }
}

void TerminalBackend::ScrollLines(int lines)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr || impl_->screen_ == nullptr || lines == 0)
        return;
    const unsigned int before = tsm_screen_sb_get_line_pos(impl_->screen_);
    if (lines > 0)
        tsm_screen_sb_up(impl_->screen_, static_cast<unsigned int>(lines));
    else
        tsm_screen_sb_down(impl_->screen_, static_cast<unsigned int>(-lines));
    const unsigned int after = tsm_screen_sb_get_line_pos(impl_->screen_);
    impl_->pending_viewport_scroll_delta_ +=
        static_cast<int>(after) - static_cast<int>(before);
}

void TerminalBackend::StartSelection(unsigned int column, unsigned int row)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr || impl_->screen_ == nullptr ||
        column >= tsm_screen_get_width(impl_->screen_) ||
        row >= tsm_screen_get_height(impl_->screen_))
        return;
    tsm_screen_selection_start(impl_->screen_, column, row);
    impl_->selection_active_ = true;
}

void TerminalBackend::UpdateSelection(unsigned int column, unsigned int row)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr && impl_->screen_ != nullptr && impl_->selection_active_)
        tsm_screen_selection_target(impl_->screen_, column, row);
}

void TerminalBackend::SelectWord(unsigned int column, unsigned int row)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr || impl_->screen_ == nullptr ||
        column >= tsm_screen_get_width(impl_->screen_) ||
        row >= tsm_screen_get_height(impl_->screen_))
        return;
    tsm_screen_selection_reset(impl_->screen_);
    tsm_screen_selection_word(impl_->screen_, column, row);
    impl_->selection_active_ = true;
}

void TerminalBackend::SelectLine(unsigned int row)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr || impl_->screen_ == nullptr ||
        row >= tsm_screen_get_height(impl_->screen_))
        return;
    StartSelection(0, row);
    UpdateSelection(tsm_screen_get_width(impl_->screen_) - 1, row);
}

void TerminalBackend::ClearSelection()
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr && impl_->screen_ != nullptr)
        tsm_screen_selection_reset(impl_->screen_);
    if (impl_ != nullptr)
        impl_->selection_active_ = false;
}

bool TerminalBackend::HasSelection() const
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ != nullptr && impl_->selection_active_;
}

std::string TerminalBackend::CopySelection()
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr || impl_->screen_ == nullptr ||
        !impl_->selection_active_)
        return {};

    char* text = nullptr;
    // libtsm returns the copied byte count on success and a negative errno
    // value on failure, rather than zero for success.
    if (tsm_screen_selection_copy(impl_->screen_, &text) < 0 || text == nullptr) {
        impl_->selection_active_ = false;
        return {};
    }

    std::string result(text);
    std::free(text);
    if (!result.empty())
        ++impl_->metrics_.selection_copies;
    return result;
}

std::shared_ptr<const PackedFrame> TerminalBackend::TakePackedFrame()
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ == nullptr ? std::shared_ptr<const PackedFrame> {}
                            : impl_->TakePackedFrame();
}

SakuraTerminalMetrics TerminalBackend::GetMetrics() const
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ == nullptr ? SakuraTerminalMetrics {} : impl_->metrics_;
}

void TerminalBackend::RebindOwnerThread()
{
    if (impl_ != nullptr)
        impl_->RebindOwnerThread();
}

struct SakuraTerminal {
    SakuraTerminalWriteCallback callback = nullptr;
    void* userdata = nullptr;
    std::unique_ptr<TerminalBackend> backend;

    SakuraTerminal(SakuraTerminalWriteCallback callback_value,
                   void* userdata_value)
        : callback(callback_value), userdata(userdata_value),
          backend(std::make_unique<TerminalBackend>(
              [this](const char* data, std::size_t length) {
                  if (callback != nullptr) {
                      try {
                          callback(userdata, data, length);
                      } catch (...) {
                          // Exceptions must never cross the C ABI boundary.
                      }
                  }
              }))
    {
    }
};

struct SakuraTerminalFrame {
    std::shared_ptr<const PackedFrame> packed_frame;
};

static uint64_t PackedStyleId(const PackedStyle& style)
{
    uint64_t value = style.attributes;
    value |= static_cast<uint64_t>(style.foreground[0]) << 8;
    value |= static_cast<uint64_t>(style.foreground[1]) << 16;
    value |= static_cast<uint64_t>(style.foreground[2]) << 24;
    value |= static_cast<uint64_t>(style.background[0]) << 32;
    value |= static_cast<uint64_t>(style.background[1]) << 40;
    value |= static_cast<uint64_t>(style.background[2]) << 48;
    return value;
}

extern "C" {

SakuraTerminal* sakura_terminal_new(SakuraTerminalWriteCallback callback,
                                     void* userdata)
{
    try {
        return new SakuraTerminal(callback, userdata);
    } catch (...) {
        return nullptr;
    }
}

void sakura_terminal_free(SakuraTerminal* terminal)
{
    delete terminal;
}

void sakura_terminal_rebind_owner_thread(SakuraTerminal* terminal)
{
    if (terminal != nullptr && terminal->backend != nullptr)
        terminal->backend->RebindOwnerThread();
}

int sakura_terminal_is_ready(const SakuraTerminal* terminal)
{
    return terminal != nullptr && terminal->backend != nullptr &&
           terminal->backend->IsReady();
}

const char* sakura_terminal_error(const SakuraTerminal* terminal)
{
    static const char empty[] = "";
    return terminal == nullptr || terminal->backend == nullptr
        ? empty : terminal->backend->Error().c_str();
}

const char* sakura_terminal_title(const SakuraTerminal* terminal)
{
    static const char empty[] = "";
    return terminal == nullptr || terminal->backend == nullptr
        ? empty : terminal->backend->Title().c_str();
}

int sakura_terminal_resize(SakuraTerminal* terminal,
                           unsigned int columns, unsigned int rows)
{
    try {
        return terminal != nullptr && terminal->backend != nullptr &&
               terminal->backend->Resize(columns, rows);
    } catch (...) {
        return 0;
    }
}

void sakura_terminal_feed_output(SakuraTerminal* terminal,
                                 const char* data, size_t length)
{
    try {
        if (terminal != nullptr && terminal->backend != nullptr)
            terminal->backend->FeedOutput(data, length);
    } catch (...) {
    }
}

int sakura_terminal_handle_key(SakuraTerminal* terminal,
                               uint32_t keysym, uint32_t ascii,
                               unsigned int modifiers, uint32_t unicode)
{
    try {
        return terminal != nullptr && terminal->backend != nullptr &&
               terminal->backend->HandleKey(keysym, ascii, modifiers, unicode);
    } catch (...) {
        return 0;
    }
}

void sakura_terminal_paste(SakuraTerminal* terminal,
                           const char* text, size_t length)
{
    try {
        if (terminal != nullptr && terminal->backend != nullptr &&
            text != nullptr && length > 0)
            terminal->backend->Paste(std::string(text, length));
    } catch (...) {
    }
}

int sakura_terminal_handle_mouse(SakuraTerminal* terminal,
                                 unsigned int cell_x, unsigned int cell_y,
                                 unsigned int pixel_x, unsigned int pixel_y,
                                 unsigned int button, unsigned int event,
                                 unsigned char modifiers)
{
    try {
        return terminal != nullptr && terminal->backend != nullptr &&
               terminal->backend->HandleMouse(cell_x, cell_y, pixel_x, pixel_y,
                                              button, event, modifiers);
    } catch (...) {
        return 0;
    }
}

int sakura_terminal_mouse_reporting_enabled(const SakuraTerminal* terminal)
{
    return terminal != nullptr && terminal->backend != nullptr &&
           terminal->backend->MouseReportingEnabled();
}

void sakura_terminal_scroll_page_up(SakuraTerminal* terminal,
                                    unsigned int pages)
{
    try {
        if (terminal != nullptr && terminal->backend != nullptr)
            terminal->backend->ScrollPageUp(pages);
    } catch (...) {
    }
}

void sakura_terminal_scroll_page_down(SakuraTerminal* terminal,
                                      unsigned int pages)
{
    try {
        if (terminal != nullptr && terminal->backend != nullptr)
            terminal->backend->ScrollPageDown(pages);
    } catch (...) {
    }
}

void sakura_terminal_scroll_lines(SakuraTerminal* terminal, int lines)
{
    try {
        if (terminal != nullptr && terminal->backend != nullptr)
            terminal->backend->ScrollLines(lines);
    } catch (...) {
    }
}

void sakura_terminal_start_selection(SakuraTerminal* terminal,
                                     unsigned int column, unsigned int row)
{
    try {
        if (terminal != nullptr && terminal->backend != nullptr)
            terminal->backend->StartSelection(column, row);
    } catch (...) {
    }
}

void sakura_terminal_update_selection(SakuraTerminal* terminal,
                                      unsigned int column, unsigned int row)
{
    try {
        if (terminal != nullptr && terminal->backend != nullptr)
            terminal->backend->UpdateSelection(column, row);
    } catch (...) {
    }
}

void sakura_terminal_select_word(SakuraTerminal* terminal,
                                 unsigned int column, unsigned int row)
{
    try {
        if (terminal != nullptr && terminal->backend != nullptr)
            terminal->backend->SelectWord(column, row);
    } catch (...) {
    }
}

void sakura_terminal_select_line(SakuraTerminal* terminal,
                                 unsigned int row)
{
    try {
        if (terminal != nullptr && terminal->backend != nullptr)
            terminal->backend->SelectLine(row);
    } catch (...) {
    }
}

void sakura_terminal_clear_selection(SakuraTerminal* terminal)
{
    try {
        if (terminal != nullptr && terminal->backend != nullptr)
            terminal->backend->ClearSelection();
    } catch (...) {
    }
}

int sakura_terminal_has_selection(const SakuraTerminal* terminal)
{
    return terminal != nullptr && terminal->backend != nullptr &&
           terminal->backend->HasSelection();
}

char* sakura_terminal_copy_selection(SakuraTerminal* terminal)
{
    try {
        if (terminal == nullptr || terminal->backend == nullptr)
            return nullptr;
        const std::string text = terminal->backend->CopySelection();
        char* result = static_cast<char*>(std::malloc(text.size() + 1));
        if (result == nullptr)
            return nullptr;
        if (!text.empty())
            std::memcpy(result, text.data(), text.size());
        result[text.size()] = '\0';
        return result;
    } catch (...) {
        return nullptr;
    }
}

void sakura_terminal_free_string(char* text)
{
    std::free(text);
}

SakuraTerminalFrame* sakura_terminal_take_frame(SakuraTerminal* terminal)
{
    if (terminal == nullptr || terminal->backend == nullptr)
        return nullptr;
    try {
        auto* frame = new SakuraTerminalFrame;
        frame->packed_frame = terminal->backend->TakePackedFrame();
        return frame;
    } catch (...) {
        return nullptr;
    }
}

void sakura_terminal_frame_free(SakuraTerminalFrame* frame)
{
    delete frame;
}

int sakura_terminal_frame_info(const SakuraTerminalFrame* frame,
                               SakuraTerminalFrameInfo* info)
{
    if (frame == nullptr || info == nullptr || frame->packed_frame == nullptr)
        return 0;
    const PackedFrame& source = *frame->packed_frame;
    info->generation = source.generation;
    info->changed = source.changed;
    info->full_repaint = source.full_repaint;
    info->scroll_delta = source.scroll_delta;
    info->columns = source.columns;
    info->rows = source.rows;
    info->cursor_x = source.cursor_x;
    info->cursor_y = source.cursor_y;
    info->cursor_visible = source.cursor_visible;
    info->cursor_style = source.cursor_style;
    info->alternate_screen = source.alternate_screen;
    info->dirty = source.dirty;
    return 1;
}

size_t sakura_terminal_frame_dirty_span_count(
    const SakuraTerminalFrame* frame)
{
    return frame == nullptr || frame->packed_frame == nullptr
        ? 0 : frame->packed_frame->dirty_spans.size();
}

int sakura_terminal_frame_dirty_span(const SakuraTerminalFrame* frame,
                                     size_t index,
                                     SakuraTerminalDirtySpan* span)
{
    if (frame == nullptr || span == nullptr || frame->packed_frame == nullptr ||
        index >= frame->packed_frame->dirty_spans.size())
        return 0;
    *span = frame->packed_frame->dirty_spans[index];
    return 1;
}

int sakura_terminal_frame_cell(const SakuraTerminalFrame* frame,
                               unsigned int column, unsigned int row,
                               SakuraTerminalCellView* cell)
{
    if (frame == nullptr || cell == nullptr || frame->packed_frame == nullptr ||
        frame->packed_frame->grid == nullptr)
        return 0;
    const PackedFrame& packed = *frame->packed_frame;
    const PackedGrid& grid = *packed.grid;
    if (column >= packed.columns || row >= packed.rows ||
        row >= grid.row_data.size() || grid.row_data[row] == nullptr)
        return 0;
    const PackedRow& source = *grid.row_data[row];
    if (column >= source.cells.size())
        return 0;
    const PackedCell& source_cell = source.cells[column];
    if (source_cell.style_index >= grid.styles.size() ||
        source_cell.text_offset > source.text.size() ||
        source_cell.text_length > source.text.size() - source_cell.text_offset)
        return 0;
    const PackedStyle& style = grid.styles[source_cell.style_index];
    cell->codepoint = source_cell.codepoint;
    cell->text = source.text.data() + source_cell.text_offset;
    cell->text_length = source_cell.text_length;
    cell->width = source_cell.width;
    std::memcpy(cell->foreground, style.foreground.data(), 3);
    std::memcpy(cell->background, style.background.data(), 3);
    cell->attributes = style.attributes;
    return 1;
}

size_t sakura_terminal_frame_row_run_count(const SakuraTerminalFrame* frame,
                                           unsigned int row)
{
    try {
        if (frame == nullptr || frame->packed_frame == nullptr ||
            frame->packed_frame->grid == nullptr)
            return 0;
        const PackedFrame& packed = *frame->packed_frame;
        if (row >= packed.rows || row >= packed.grid->row_data.size() ||
            packed.grid->row_data[row] == nullptr)
            return 0;
        return packed.grid->row_data[row]->runs.size();
    } catch (...) {
        return 0;
    }
}

int sakura_terminal_frame_row_run(const SakuraTerminalFrame* frame,
                                  unsigned int row, size_t index,
                                  SakuraTerminalRunView* run)
{
    try {
        if (frame == nullptr || run == nullptr || frame->packed_frame == nullptr ||
            frame->packed_frame->grid == nullptr)
            return 0;
        const PackedFrame& packed = *frame->packed_frame;
        const PackedGrid& grid = *packed.grid;
        if (row >= packed.rows || row >= grid.row_data.size() ||
            grid.row_data[row] == nullptr)
            return 0;
        const PackedRow& source_row = *grid.row_data[row];
        if (index >= source_row.runs.size())
            return 0;
        const PackedRun& source = source_row.runs[index];
        if (source.style_index >= grid.styles.size() ||
            source.text_offset > source_row.text.size() ||
            source.text_length > source_row.text.size() - source.text_offset)
            return 0;
        const PackedStyle& style = grid.styles[source.style_index];
        run->row = row;
        run->left = source.left;
        run->cell_count = source.cell_count;
        run->style_id = PackedStyleId(style);
        run->text = source_row.text.data() + source.text_offset;
        run->text_length = source.text_length;
        std::memcpy(run->foreground, style.foreground.data(), 3);
        std::memcpy(run->background, style.background.data(), 3);
        run->attributes = style.attributes;
        return 1;
    } catch (...) {
        return 0;
    }
}

void sakura_terminal_get_metrics(const SakuraTerminal* terminal,
                                 SakuraTerminalMetrics* metrics)
{
    if (metrics == nullptr)
        return;
    std::memset(metrics, 0, sizeof(*metrics));
    if (terminal == nullptr || terminal->backend == nullptr)
        return;
    *metrics = terminal->backend->GetMetrics();
}

} // extern "C"
