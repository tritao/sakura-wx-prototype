#include <sakura/terminal/core.h>
#include <sakura/terminal/core_c.h>

#include <tsm/libtsm.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <utility>

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
    TerminalFrame TakeFrame();
    TerminalSnapshot TakeSnapshot();
    TerminalMetrics GetMetrics() const;
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
            metrics_.output_bytes += length;
            ++metrics_.output_chunks;
            last_output_time_ = std::chrono::steady_clock::now();
            output_waiting_for_render_ = true;
            TrackCursorStyle(data, length);
            tsm_vte_input(vte_, data, length);
        }
    }

private:
    struct FrameDrawContext {
        TerminalSnapshot* snapshot = nullptr;
        TerminalDirtyRegion* dirty = nullptr;
        std::vector<TerminalDirtySpan>* dirty_spans = nullptr;
        tsm_age_t previous_age = 0;
        bool full_repaint = false;
        bool cursor_changed = false;
        unsigned int previous_cursor_x = 0;
        unsigned int previous_cursor_y = 0;
        unsigned int cursor_x = 0;
        unsigned int cursor_y = 0;
        uint64_t* decoded_cells = nullptr;
    };

    static void IncludeDirty(TerminalDirtyRegion& dirty,
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
        dirty.left = dirty.IsEmpty() ? left : std::min(dirty.left, left);
        dirty.top = dirty.IsEmpty() ? row : std::min(dirty.top, row);
        dirty.right = dirty.IsEmpty() ? right : std::max(dirty.right, right);
        dirty.bottom = dirty.IsEmpty() ? row + 1
                                       : std::max(dirty.bottom, row + 1);
    }

    static void IncludeCell(TerminalDirtyRegion& dirty,
                            unsigned int column, unsigned int row,
                            unsigned int columns, unsigned int rows)
    {
        IncludeDirty(dirty, column, row, 1, columns, rows);
    }

    static void IncludeSpan(std::vector<TerminalDirtySpan>& spans,
                            unsigned int row, unsigned int left,
                            unsigned int right)
    {
        if (left >= right)
            return;
        if (!spans.empty()) {
            TerminalDirtySpan& previous = spans.back();
            if (previous.row == row && left <= previous.right) {
                previous.left = std::min(previous.left, left);
                previous.right = std::max(previous.right, right);
                return;
            }
        }
        spans.push_back({row, left, right});
    }

    static void IncludeCellSpan(std::vector<TerminalDirtySpan>& spans,
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

    static void IncludeFullSpans(std::vector<TerminalDirtySpan>& spans,
                                 unsigned int columns, unsigned int rows)
    {
        spans.clear();
        if (columns == 0)
            return;
        spans.reserve(rows);
        for (unsigned int row = 0; row < rows; ++row)
            spans.push_back({row, 0, columns});
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

    static int DrawCell(struct tsm_screen*, uint64_t,
                        const uint32_t* codepoints, std::size_t length,
                        unsigned int width, unsigned int column,
                        unsigned int row, const struct tsm_screen_attr* attr,
                        tsm_age_t age, void* user_data)
    {
        auto* context = static_cast<FrameDrawContext*>(user_data);
        if (context == nullptr || context->snapshot == nullptr ||
            context->dirty == nullptr || attr == nullptr)
            return 0;
        TerminalSnapshot* snapshot = context->snapshot;
        if (column >= snapshot->columns || row >= snapshot->rows)
            return 0;

        const bool cursor_cell = context->cursor_changed &&
            ((column == context->previous_cursor_x &&
              row == context->previous_cursor_y) ||
             (column == context->cursor_x && row == context->cursor_y));
        const bool changed = context->full_repaint || age == 0 ||
            age > context->previous_age || cursor_cell;
        if (!changed)
            return 0;
        if (context->decoded_cells != nullptr)
            ++*context->decoded_cells;
        IncludeDirty(*context->dirty, column, row, width,
                     snapshot->columns, snapshot->rows);
        if (context->dirty_spans != nullptr)
            IncludeCellSpan(*context->dirty_spans, column, row, width,
                            snapshot->columns, snapshot->rows);

        TerminalCell& cell = snapshot->cells[row * snapshot->columns + column];
        cell.text.clear();
        cell.width = width;
        cell.codepoint = width == 0 ? 0 : ' ';
        for (std::size_t index = 0; index < length; ++index) {
            char utf8[8] {};
            const std::size_t utf8_length = tsm_ucs4_to_utf8(
                codepoints[index], utf8);
            cell.text.append(utf8, utf8_length);
            if (index == 0)
                cell.codepoint = codepoints[index];
        }
        if (length == 0 && width > 0)
            cell.text = " ";

        cell.attributes = 0;
        if (attr->bold) cell.attributes |= 0x01;
        if (attr->italic) cell.attributes |= 0x02;
        if (attr->underline) cell.attributes |= 0x04;
        if (attr->blink) cell.attributes |= 0x08;
        if (attr->dim) cell.attributes |= 0x10;

        std::array<uint8_t, 3> foreground {attr->fr, attr->fg, attr->fb};
        std::array<uint8_t, 3> background {attr->br, attr->bg, attr->bb};
        if (attr->inverse)
            std::swap(foreground, background);
        if (attr->dim) {
            for (std::size_t index = 0; index < foreground.size(); ++index)
                foreground[index] = static_cast<uint8_t>(
                    background[index] / 2 + foreground[index] / 2);
        }
        cell.foreground = foreground;
        cell.background = background;
        return 0;
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

                    TerminalCursorStyle style = cursor_style_;
                    switch (value) {
                    case 0:
                    case 1:
                    case 2:
                        style = TerminalCursorStyle::Block;
                        break;
                    case 3:
                    case 4:
                        style = TerminalCursorStyle::Underline;
                        break;
                    case 5:
                    case 6:
                        style = TerminalCursorStyle::Bar;
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

    TerminalFrame TakeFrame()
    {
        TerminalFrame frame;
        if (screen_ == nullptr)
            return frame;

        if (!snapshot_cache_ || snapshot_cache_.use_count() > 1) {
            snapshot_cache_ = snapshot_cache_
                ? std::make_shared<TerminalSnapshot>(*snapshot_cache_)
                : std::make_shared<TerminalSnapshot>();
        }
        TerminalSnapshot& snapshot = *snapshot_cache_;
        frame.dirty_spans.clear();
        snapshot.columns = tsm_screen_get_width(screen_);
        snapshot.rows = tsm_screen_get_height(screen_);
        snapshot.cursor_x = tsm_screen_get_cursor_x(screen_);
        snapshot.cursor_y = tsm_screen_get_cursor_y(screen_);
        snapshot.cursor_visible = vte_ != nullptr &&
            (tsm_vte_get_flags(vte_) & TSM_VTE_FLAG_TEXT_CURSOR_MODE) != 0;
        snapshot.cursor_style = cursor_style_;
        snapshot.alternate_screen = (tsm_screen_get_flags(screen_) &
                                     TSM_SCREEN_ALTERNATE) != 0;

        const bool metadata_changed = frame_valid_ &&
            (snapshot.columns != last_columns_ || snapshot.rows != last_rows_ ||
             snapshot.alternate_screen != last_alternate_screen_);
        frame.full_repaint = !frame_valid_ || metadata_changed;

        snapshot.cells.resize(snapshot.columns * snapshot.rows);
        FrameDrawContext context;
        context.snapshot = &snapshot;
        context.dirty = &frame.dirty;
        context.dirty_spans = &frame.dirty_spans;
        context.previous_age = last_draw_age_;
        context.full_repaint = frame.full_repaint;
        context.cursor_changed = frame_valid_ &&
            (snapshot.cursor_x != last_cursor_x_ ||
             snapshot.cursor_y != last_cursor_y_ ||
             snapshot.cursor_visible != last_cursor_visible_ ||
             snapshot.cursor_style != last_cursor_style_);
        context.previous_cursor_x = last_cursor_x_;
        context.previous_cursor_y = last_cursor_y_;
        context.cursor_x = snapshot.cursor_x;
        context.cursor_y = snapshot.cursor_y;
        context.decoded_cells = &metrics_.frame_cells_decoded;
        const uint64_t decoded_before = metrics_.frame_cells_decoded;
        const tsm_age_t since = frame.full_repaint ? 0 : last_draw_age_;
        const tsm_age_t age = tsm_screen_draw_since(
            screen_, since, &Impl::DrawCell, &context);
        const uint64_t decoded_cells = metrics_.frame_cells_decoded -
                                       decoded_before;
        const uint64_t total_cells = static_cast<uint64_t>(snapshot.columns) *
                                     snapshot.rows;
        if (decoded_cells < total_cells)
            metrics_.frame_cells_reused += total_cells - decoded_cells;

        if (context.cursor_changed) {
            IncludeCell(frame.dirty, last_cursor_x_, last_cursor_y_,
                        snapshot.columns, snapshot.rows);
            IncludeCell(frame.dirty, snapshot.cursor_x, snapshot.cursor_y,
                        snapshot.columns, snapshot.rows);
            IncludeCellSpan(frame.dirty_spans, last_cursor_x_, last_cursor_y_,
                            1, snapshot.columns, snapshot.rows);
            IncludeCellSpan(frame.dirty_spans, snapshot.cursor_x, snapshot.cursor_y,
                            1, snapshot.columns, snapshot.rows);
        }

        if (age == 0) {
            frame.full_repaint = true;
            frame.dirty.left = 0;
            frame.dirty.top = 0;
            frame.dirty.right = snapshot.columns;
            frame.dirty.bottom = snapshot.rows;
        }
        if (frame.full_repaint)
            IncludeFullSpans(frame.dirty_spans, snapshot.columns, snapshot.rows);
        if (frame.full_repaint && frame.dirty.IsEmpty()) {
            frame.dirty.left = 0;
            frame.dirty.top = 0;
            frame.dirty.right = snapshot.columns;
            frame.dirty.bottom = snapshot.rows;
        }
        if (!frame.dirty.IsEmpty() && frame.dirty.left == 0 &&
            frame.dirty.top == 0 &&
            frame.dirty.right == snapshot.columns &&
            frame.dirty.bottom == snapshot.rows)
            frame.full_repaint = true;
        if (frame.full_repaint)
            IncludeFullSpans(frame.dirty_spans, snapshot.columns, snapshot.rows);

        frame.changed = frame.full_repaint || !frame.dirty.IsEmpty();
        if (frame.changed)
            frame.generation = ++frame_generation_;
        else
            frame.generation = frame_generation_;

        // libtsm returns age zero when its age counter rolls over. Keep the
        // next frame conservative so the backing store is re-established
        // with a fresh age baseline.
        frame_valid_ = age != 0;
        last_draw_age_ = age;
        last_columns_ = snapshot.columns;
        last_rows_ = snapshot.rows;
        last_alternate_screen_ = snapshot.alternate_screen;
        last_cursor_x_ = snapshot.cursor_x;
        last_cursor_y_ = snapshot.cursor_y;
        last_cursor_visible_ = snapshot.cursor_visible;
        last_cursor_style_ = snapshot.cursor_style;
        frame.snapshot = snapshot_cache_;
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
    TerminalCursorStyle cursor_style_ = TerminalCursorStyle::Block;
    bool selection_active_ = false;
    TerminalMetrics metrics_;
    std::chrono::steady_clock::time_point last_output_time_;
    bool output_waiting_for_render_ = false;
    std::thread::id owner_thread_ = std::this_thread::get_id();
    bool frame_valid_ = false;
    tsm_age_t last_draw_age_ = 0;
    uint64_t frame_generation_ = 0;
    unsigned int last_columns_ = 0;
    unsigned int last_rows_ = 0;
    unsigned int last_cursor_x_ = 0;
    unsigned int last_cursor_y_ = 0;
    bool last_cursor_visible_ = false;
    bool last_alternate_screen_ = false;
    TerminalCursorStyle last_cursor_style_ = TerminalCursorStyle::Block;
    std::shared_ptr<TerminalSnapshot> snapshot_cache_;

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
    return impl_ != nullptr && impl_->screen_ != nullptr &&
           tsm_screen_resize(impl_->screen_, columns, rows) == 0;
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
    if (impl_ != nullptr && impl_->screen_ != nullptr)
        tsm_screen_sb_page_up(impl_->screen_, pages);
}

void TerminalBackend::ScrollPageDown(unsigned int pages)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr && impl_->screen_ != nullptr)
        tsm_screen_sb_page_down(impl_->screen_, pages);
}

void TerminalBackend::ScrollLines(int lines)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr || impl_->screen_ == nullptr || lines == 0)
        return;
    if (lines > 0)
        tsm_screen_sb_up(impl_->screen_, static_cast<unsigned int>(lines));
    else
        tsm_screen_sb_down(impl_->screen_, static_cast<unsigned int>(-lines));
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

TerminalFrame TerminalBackend::TakeFrame()
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr)
        return {};

    ++impl_->metrics_.rendered_frames;
    if (impl_->output_waiting_for_render_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - impl_->last_output_time_).count();
        const uint64_t latency = static_cast<uint64_t>(std::max<int64_t>(0, elapsed));
        ++impl_->metrics_.render_latency_samples;
        impl_->metrics_.max_render_latency_us = std::max(
            impl_->metrics_.max_render_latency_us, latency);
        impl_->output_waiting_for_render_ = false;
    }

    return impl_->TakeFrame();
}

TerminalSnapshot TerminalBackend::TakeSnapshot()
{
    TerminalFrame frame = TakeFrame();
    return frame.snapshot == nullptr ? TerminalSnapshot {} : *frame.snapshot;
}

TerminalMetrics TerminalBackend::GetMetrics() const
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ == nullptr ? TerminalMetrics {} : impl_->metrics_;
}

void TerminalBackend::RebindOwnerThread()
{
    if (impl_ != nullptr)
        impl_->RebindOwnerThread();
}

static TerminalFrame TakeCppFrame(SakuraTerminal* terminal);
static TerminalMetrics TakeCppMetrics(const SakuraTerminal* terminal);

class TerminalCore::Impl {
public:
    static void WriteBridge(void* userdata, const char* data,
                            std::size_t length)
    {
        auto* impl = static_cast<Impl*>(userdata);
        if (impl != nullptr && impl->write_callback_)
            impl->write_callback_(data, length);
    }

    explicit Impl(WriteCallback write_callback)
        : write_callback_(std::move(write_callback)),
          terminal_(sakura_terminal_new(&Impl::WriteBridge, this))
    {
    }

    ~Impl()
    {
        sakura_terminal_free(terminal_);
    }

    void RebindOwnerThread()
    {
        sakura_terminal_rebind_owner_thread(terminal_);
    }

    WriteCallback write_callback_;
    SakuraTerminal* terminal_ = nullptr;
    mutable std::string error_cache_;
    mutable std::string title_cache_;
};

TerminalCore::TerminalCore(WriteCallback write_callback)
    : impl_(std::make_unique<Impl>(std::move(write_callback)))
{
}

TerminalCore::~TerminalCore() = default;

TerminalCore::TerminalCore(TerminalCore&& other) noexcept
    : impl_(std::move(other.impl_))
{
    if (impl_ != nullptr)
        impl_->RebindOwnerThread();
}

TerminalCore& TerminalCore::operator=(TerminalCore&& other) noexcept
{
    if (this == &other)
        return *this;
    impl_ = std::move(other.impl_);
    if (impl_ != nullptr)
        impl_->RebindOwnerThread();
    return *this;
}

bool TerminalCore::IsReady() const
{
    return impl_ != nullptr && sakura_terminal_is_ready(impl_->terminal_);
}

const std::string& TerminalCore::Error() const
{
    static const std::string empty;
    if (impl_ == nullptr)
        return empty;
    const char* error = sakura_terminal_error(impl_->terminal_);
    impl_->error_cache_ = error == nullptr ? "" : error;
    return impl_->error_cache_;
}

const std::string& TerminalCore::Title() const
{
    static const std::string empty;
    if (impl_ == nullptr)
        return empty;
    const char* title = sakura_terminal_title(impl_->terminal_);
    impl_->title_cache_ = title == nullptr ? "" : title;
    return impl_->title_cache_;
}

bool TerminalCore::Resize(unsigned int columns, unsigned int rows)
{
    return impl_ != nullptr &&
           sakura_terminal_resize(impl_->terminal_, columns, rows);
}

void TerminalCore::FeedOutput(const char* data, std::size_t length)
{
    if (impl_ != nullptr)
        sakura_terminal_feed_output(impl_->terminal_, data, length);
}

bool TerminalCore::HandleKey(uint32_t keysym, uint32_t ascii,
                             unsigned int modifiers, uint32_t unicode)
{
    return impl_ != nullptr &&
           sakura_terminal_handle_key(impl_->terminal_, keysym, ascii,
                                      modifiers, unicode);
}

void TerminalCore::Paste(const std::string& text)
{
    if (impl_ != nullptr)
        sakura_terminal_paste(impl_->terminal_, text.data(), text.size());
}

bool TerminalCore::HandleMouse(unsigned int cell_x, unsigned int cell_y,
                               unsigned int pixel_x, unsigned int pixel_y,
                               unsigned int button, unsigned int event,
                               unsigned char modifiers)
{
    return impl_ != nullptr &&
           sakura_terminal_handle_mouse(impl_->terminal_, cell_x, cell_y,
                                        pixel_x, pixel_y, button, event,
                                        modifiers);
}

bool TerminalCore::MouseReportingEnabled() const
{
    return impl_ != nullptr &&
           sakura_terminal_mouse_reporting_enabled(impl_->terminal_);
}

void TerminalCore::ScrollPageUp(unsigned int pages)
{
    if (impl_ != nullptr)
        sakura_terminal_scroll_page_up(impl_->terminal_, pages);
}

void TerminalCore::ScrollPageDown(unsigned int pages)
{
    if (impl_ != nullptr)
        sakura_terminal_scroll_page_down(impl_->terminal_, pages);
}

void TerminalCore::ScrollLines(int lines)
{
    if (impl_ != nullptr)
        sakura_terminal_scroll_lines(impl_->terminal_, lines);
}

void TerminalCore::StartSelection(unsigned int column, unsigned int row)
{
    if (impl_ != nullptr)
        sakura_terminal_start_selection(impl_->terminal_, column, row);
}

void TerminalCore::UpdateSelection(unsigned int column, unsigned int row)
{
    if (impl_ != nullptr)
        sakura_terminal_update_selection(impl_->terminal_, column, row);
}

void TerminalCore::SelectWord(unsigned int column, unsigned int row)
{
    if (impl_ != nullptr)
        sakura_terminal_select_word(impl_->terminal_, column, row);
}

void TerminalCore::SelectLine(unsigned int row)
{
    if (impl_ != nullptr)
        sakura_terminal_select_line(impl_->terminal_, row);
}

void TerminalCore::ClearSelection()
{
    if (impl_ != nullptr)
        sakura_terminal_clear_selection(impl_->terminal_);
}

bool TerminalCore::HasSelection() const
{
    return impl_ != nullptr &&
           sakura_terminal_has_selection(impl_->terminal_);
}

std::string TerminalCore::CopySelection()
{
    if (impl_ == nullptr)
        return {};
    char* text = sakura_terminal_copy_selection(impl_->terminal_);
    if (text == nullptr)
        return {};
    std::string result(text);
    sakura_terminal_free_string(text);
    return result;
}

TerminalFrame TerminalCore::TakeFrame()
{
    return impl_ == nullptr ? TerminalFrame {} : TakeCppFrame(impl_->terminal_);
}

TerminalSnapshot TerminalCore::TakeSnapshot()
{
    TerminalFrame frame = TakeFrame();
    return frame.snapshot == nullptr ? TerminalSnapshot {} : *frame.snapshot;
}

TerminalMetrics TerminalCore::GetMetrics() const
{
    return impl_ == nullptr ? TerminalMetrics {}
                            : TakeCppMetrics(impl_->terminal_);
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
    TerminalFrame frame;
    mutable bool runs_built = false;
    mutable std::vector<std::size_t> row_run_offsets;
    mutable std::vector<std::string> run_texts;
    mutable std::vector<SakuraTerminalRunView> runs;
};

static uint64_t CellStyleId(const TerminalCell& cell)
{
    uint64_t style = cell.attributes;
    style |= static_cast<uint64_t>(cell.foreground[0]) << 8;
    style |= static_cast<uint64_t>(cell.foreground[1]) << 16;
    style |= static_cast<uint64_t>(cell.foreground[2]) << 24;
    style |= static_cast<uint64_t>(cell.background[0]) << 32;
    style |= static_cast<uint64_t>(cell.background[1]) << 40;
    style |= static_cast<uint64_t>(cell.background[2]) << 48;
    return style;
}

struct PendingTerminalRun {
    unsigned int row = 0;
    unsigned int left = 0;
    unsigned int cell_count = 0;
    uint64_t style_id = 0;
    std::size_t text_index = 0;
    std::array<uint8_t, 3> foreground {};
    std::array<uint8_t, 3> background {};
    uint8_t attributes = 0;
};

static void BuildFrameRuns(const SakuraTerminalFrame* frame)
{
    if (frame == nullptr || frame->runs_built)
        return;
    frame->row_run_offsets.clear();
    frame->run_texts.clear();
    frame->runs.clear();
    if (frame->frame.snapshot == nullptr) {
        frame->row_run_offsets.push_back(0);
        frame->runs_built = true;
        return;
    }

    const TerminalSnapshot& snapshot = *frame->frame.snapshot;
    const std::size_t cell_count = static_cast<std::size_t>(snapshot.columns) *
                                   snapshot.rows;
    frame->row_run_offsets.reserve(static_cast<std::size_t>(snapshot.rows) + 1);
    frame->run_texts.reserve(cell_count);
    frame->runs.reserve(cell_count);

    std::vector<PendingTerminalRun> pending;
    pending.reserve(cell_count);
    frame->row_run_offsets.push_back(0);
    for (unsigned int row = 0; row < snapshot.rows; ++row) {
        unsigned int column = 0;
        while (column < snapshot.columns) {
            const TerminalCell& first =
                snapshot.cells[row * snapshot.columns + column];
            PendingTerminalRun next;
            next.row = row;
            next.left = column;
            next.style_id = CellStyleId(first);
            next.foreground = first.foreground;
            next.background = first.background;
            next.attributes = first.attributes;
            next.text_index = frame->run_texts.size();
            frame->run_texts.emplace_back();

            std::string& text = frame->run_texts.back();
            while (column < snapshot.columns) {
                const TerminalCell& cell =
                    snapshot.cells[row * snapshot.columns + column];
                if (CellStyleId(cell) != next.style_id)
                    break;
                text.append(cell.text);
                ++next.cell_count;
                ++column;
            }
            pending.push_back(next);
        }
        frame->row_run_offsets.push_back(pending.size());
    }

    frame->runs.reserve(pending.size());
    for (const PendingTerminalRun& source : pending) {
        const std::string& text = frame->run_texts[source.text_index];
        SakuraTerminalRunView run {};
        run.row = source.row;
        run.left = source.left;
        run.cell_count = source.cell_count;
        run.style_id = source.style_id;
        run.text = text.c_str();
        run.text_length = text.size();
        std::memcpy(run.foreground, source.foreground.data(), 3);
        std::memcpy(run.background, source.background.data(), 3);
        run.attributes = source.attributes;
        frame->runs.push_back(run);
    }
    frame->runs_built = true;
}

static TerminalFrame TakeCppFrame(SakuraTerminal* terminal)
{
    SakuraTerminalFrame* frame = sakura_terminal_take_frame(terminal);
    if (frame == nullptr)
        return {};
    TerminalFrame result = std::move(frame->frame);
    sakura_terminal_frame_free(frame);
    return result;
}

static TerminalMetrics TakeCppMetrics(const SakuraTerminal* terminal)
{
    SakuraTerminalMetrics source {};
    sakura_terminal_get_metrics(terminal, &source);
    TerminalMetrics result;
    result.output_bytes = source.output_bytes;
    result.output_chunks = source.output_chunks;
    result.input_events = source.input_events;
    result.transport_write_bytes = source.transport_write_bytes;
    result.transport_write_events = source.transport_write_events;
    result.rendered_frames = source.rendered_frames;
    result.frame_cells_decoded = source.frame_cells_decoded;
    result.frame_cells_reused = source.frame_cells_reused;
    result.render_latency_samples = source.render_latency_samples;
    result.max_render_latency_us = source.max_render_latency_us;
    result.selection_copies = source.selection_copies;
    result.paste_bytes = source.paste_bytes;
    result.mouse_mode_changes = source.mouse_mode_changes;
    result.mouse_events = source.mouse_events;
    result.mouse_events_forwarded = source.mouse_events_forwarded;
    result.title_changes = source.title_changes;
    result.cursor_style_changes = source.cursor_style_changes;
    return result;
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
        frame->frame = terminal->backend->TakeFrame();
        return frame;
    } catch (...) {
        return nullptr;
    }
}

void sakura_terminal_frame_free(SakuraTerminalFrame* frame)
{
    delete frame;
}

static SakuraTerminalCursorStyle ToCStyle(TerminalCursorStyle style)
{
    switch (style) {
    case TerminalCursorStyle::Underline:
        return SAKURA_TERMINAL_CURSOR_UNDERLINE;
    case TerminalCursorStyle::Bar:
        return SAKURA_TERMINAL_CURSOR_BAR;
    case TerminalCursorStyle::Block:
    default:
        return SAKURA_TERMINAL_CURSOR_BLOCK;
    }
}

int sakura_terminal_frame_info(const SakuraTerminalFrame* frame,
                               SakuraTerminalFrameInfo* info)
{
    if (frame == nullptr || info == nullptr || frame->frame.snapshot == nullptr)
        return 0;
    const TerminalSnapshot& snapshot = *frame->frame.snapshot;
    info->generation = frame->frame.generation;
    info->changed = frame->frame.changed;
    info->full_repaint = frame->frame.full_repaint;
    info->columns = snapshot.columns;
    info->rows = snapshot.rows;
    info->cursor_x = snapshot.cursor_x;
    info->cursor_y = snapshot.cursor_y;
    info->cursor_visible = snapshot.cursor_visible;
    info->cursor_style = ToCStyle(snapshot.cursor_style);
    info->alternate_screen = snapshot.alternate_screen;
    info->dirty.left = frame->frame.dirty.left;
    info->dirty.top = frame->frame.dirty.top;
    info->dirty.right = frame->frame.dirty.right;
    info->dirty.bottom = frame->frame.dirty.bottom;
    return 1;
}

size_t sakura_terminal_frame_dirty_span_count(
    const SakuraTerminalFrame* frame)
{
    return frame == nullptr ? 0 : frame->frame.dirty_spans.size();
}

int sakura_terminal_frame_dirty_span(const SakuraTerminalFrame* frame,
                                     size_t index,
                                     SakuraTerminalDirtySpan* span)
{
    if (frame == nullptr || span == nullptr ||
        index >= frame->frame.dirty_spans.size())
        return 0;
    const TerminalDirtySpan& source = frame->frame.dirty_spans[index];
    span->row = source.row;
    span->left = source.left;
    span->right = source.right;
    return 1;
}

int sakura_terminal_frame_cell(const SakuraTerminalFrame* frame,
                               unsigned int column, unsigned int row,
                               SakuraTerminalCellView* cell)
{
    if (frame == nullptr || cell == nullptr || frame->frame.snapshot == nullptr)
        return 0;
    const TerminalSnapshot& snapshot = *frame->frame.snapshot;
    if (column >= snapshot.columns || row >= snapshot.rows)
        return 0;
    const TerminalCell& source = snapshot.cells[row * snapshot.columns + column];
    cell->codepoint = source.codepoint;
    cell->text = source.text.c_str();
    cell->text_length = source.text.size();
    cell->width = source.width;
    std::memcpy(cell->foreground, source.foreground.data(), 3);
    std::memcpy(cell->background, source.background.data(), 3);
    cell->attributes = source.attributes;
    return 1;
}

size_t sakura_terminal_frame_row_run_count(const SakuraTerminalFrame* frame,
                                           unsigned int row)
{
    try {
        if (frame == nullptr || frame->frame.snapshot == nullptr)
            return 0;
        const TerminalSnapshot& snapshot = *frame->frame.snapshot;
        if (row >= snapshot.rows)
            return 0;
        BuildFrameRuns(frame);
        return frame->row_run_offsets[row + 1] - frame->row_run_offsets[row];
    } catch (...) {
        return 0;
    }
}

int sakura_terminal_frame_row_run(const SakuraTerminalFrame* frame,
                                  unsigned int row, size_t index,
                                  SakuraTerminalRunView* run)
{
    try {
        if (frame == nullptr || run == nullptr ||
            frame->frame.snapshot == nullptr)
            return 0;
        const TerminalSnapshot& snapshot = *frame->frame.snapshot;
        if (row >= snapshot.rows)
            return 0;
        BuildFrameRuns(frame);
        const std::size_t first = frame->row_run_offsets[row];
        const std::size_t last = frame->row_run_offsets[row + 1];
        if (index >= last - first)
            return 0;
        *run = frame->runs[first + index];
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
    const TerminalMetrics source = terminal->backend->GetMetrics();
    metrics->output_bytes = source.output_bytes;
    metrics->output_chunks = source.output_chunks;
    metrics->input_events = source.input_events;
    metrics->transport_write_bytes = source.transport_write_bytes;
    metrics->transport_write_events = source.transport_write_events;
    metrics->rendered_frames = source.rendered_frames;
    metrics->frame_cells_decoded = source.frame_cells_decoded;
    metrics->frame_cells_reused = source.frame_cells_reused;
    metrics->render_latency_samples = source.render_latency_samples;
    metrics->max_render_latency_us = source.max_render_latency_us;
    metrics->selection_copies = source.selection_copies;
    metrics->paste_bytes = source.paste_bytes;
    metrics->mouse_mode_changes = source.mouse_mode_changes;
    metrics->mouse_events = source.mouse_events;
    metrics->mouse_events_forwarded = source.mouse_events_forwarded;
    metrics->title_changes = source.title_changes;
    metrics->cursor_style_changes = source.cursor_style_changes;
}

} // extern "C"
