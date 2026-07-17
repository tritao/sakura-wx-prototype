#include <sakura/terminal/core.h>

#include <tsm/libtsm.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

class TerminalCore::Impl {
public:
    explicit Impl(TerminalCore::WriteCallback write_callback)
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
                        tsm_age_t, void* user_data)
    {
        auto* snapshot = static_cast<TerminalSnapshot*>(user_data);
        if (snapshot == nullptr || column >= snapshot->columns ||
            row >= snapshot->rows || attr == nullptr)
            return 0;

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

    enum class CursorSequenceState {
        Ground,
        Escape,
        Csi,
        CsiIntermediate,
    } cursor_sequence_state_ = CursorSequenceState::Ground;
    std::string cursor_sequence_parameters_;

    TerminalCore::WriteCallback write_callback_;
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

    friend class TerminalCore;
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
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ != nullptr && impl_->IsReady();
}

const std::string& TerminalCore::Error() const
{
    static const std::string empty;
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ == nullptr ? empty : impl_->error_;
}

const std::string& TerminalCore::Title() const
{
    static const std::string empty;
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ == nullptr ? empty : impl_->title_;
}

bool TerminalCore::Resize(unsigned int columns, unsigned int rows)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ != nullptr && impl_->screen_ != nullptr &&
           tsm_screen_resize(impl_->screen_, columns, rows) == 0;
}

void TerminalCore::FeedOutput(const char* data, std::size_t length)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr)
        impl_->FeedOutput(data, length);
}

bool TerminalCore::HandleKey(uint32_t keysym, uint32_t ascii,
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

void TerminalCore::Paste(const std::string& text)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr || impl_->vte_ == nullptr || text.empty())
        return;
    impl_->metrics_.paste_bytes += text.size();
    tsm_vte_paste(impl_->vte_, text.c_str());
}

bool TerminalCore::HandleMouse(unsigned int cell_x, unsigned int cell_y,
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

bool TerminalCore::MouseReportingEnabled() const
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ != nullptr && impl_->vte_ != nullptr &&
           tsm_vte_get_mouse_mode(impl_->vte_) != 0;
}

void TerminalCore::ScrollPageUp(unsigned int pages)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr && impl_->screen_ != nullptr)
        tsm_screen_sb_page_up(impl_->screen_, pages);
}

void TerminalCore::ScrollPageDown(unsigned int pages)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr && impl_->screen_ != nullptr)
        tsm_screen_sb_page_down(impl_->screen_, pages);
}

void TerminalCore::ScrollLines(int lines)
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

void TerminalCore::StartSelection(unsigned int column, unsigned int row)
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

void TerminalCore::UpdateSelection(unsigned int column, unsigned int row)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr && impl_->screen_ != nullptr && impl_->selection_active_)
        tsm_screen_selection_target(impl_->screen_, column, row);
}

void TerminalCore::SelectWord(unsigned int column, unsigned int row)
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

void TerminalCore::SelectLine(unsigned int row)
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ == nullptr || impl_->screen_ == nullptr ||
        row >= tsm_screen_get_height(impl_->screen_))
        return;
    StartSelection(0, row);
    UpdateSelection(tsm_screen_get_width(impl_->screen_) - 1, row);
}

void TerminalCore::ClearSelection()
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    if (impl_ != nullptr && impl_->screen_ != nullptr)
        tsm_screen_selection_reset(impl_->screen_);
    if (impl_ != nullptr)
        impl_->selection_active_ = false;
}

bool TerminalCore::HasSelection() const
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ != nullptr && impl_->selection_active_;
}

std::string TerminalCore::CopySelection()
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

TerminalSnapshot TerminalCore::TakeSnapshot()
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    TerminalSnapshot snapshot;
    if (impl_ == nullptr || impl_->screen_ == nullptr)
        return snapshot;

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

    snapshot.columns = tsm_screen_get_width(impl_->screen_);
    snapshot.rows = tsm_screen_get_height(impl_->screen_);
    snapshot.cursor_x = tsm_screen_get_cursor_x(impl_->screen_);
    snapshot.cursor_y = tsm_screen_get_cursor_y(impl_->screen_);
    snapshot.cursor_visible = impl_->vte_ != nullptr &&
        (tsm_vte_get_flags(impl_->vte_) & TSM_VTE_FLAG_TEXT_CURSOR_MODE) != 0;
    snapshot.cursor_style = impl_->cursor_style_;
    snapshot.alternate_screen = (tsm_screen_get_flags(impl_->screen_) &
                                 TSM_SCREEN_ALTERNATE) != 0;

    snapshot.cells.resize(snapshot.columns * snapshot.rows);
    tsm_screen_draw(impl_->screen_, &Impl::DrawCell, &snapshot);
    return snapshot;
}

TerminalMetrics TerminalCore::GetMetrics() const
{
    if (impl_ != nullptr)
        impl_->AssertOwnerThread();
    return impl_ == nullptr ? TerminalMetrics {} : impl_->metrics_;
}
