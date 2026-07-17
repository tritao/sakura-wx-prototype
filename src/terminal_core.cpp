#include "terminal_core.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

TerminalCore::TerminalCore(WriteCallback write_callback)
    : write_callback_(std::move(write_callback))
{
    if (tsm_screen_new(&screen_, nullptr, nullptr) != 0) {
        error_ = "libtsm could not create a screen";
        return;
    }

    tsm_screen_set_max_sb(screen_, 2000);
    if (tsm_vte_new(&vte_, screen_, &TerminalCore::VteWrite, this,
                    nullptr, nullptr) != 0) {
        error_ = "libtsm could not create a VTE";
        tsm_screen_unref(screen_);
        screen_ = nullptr;
        return;
    }

    tsm_vte_set_palette(vte_, "base16-dark");
}

TerminalCore::~TerminalCore()
{
    if (vte_ != nullptr)
        tsm_vte_unref(vte_);
    if (screen_ != nullptr)
        tsm_screen_unref(screen_);
}

bool TerminalCore::Resize(unsigned int columns, unsigned int rows)
{
    return screen_ != nullptr && tsm_screen_resize(screen_, columns, rows) == 0;
}

void TerminalCore::FeedOutput(const char* data, std::size_t length)
{
    if (vte_ != nullptr && data != nullptr && length > 0) {
        metrics_.output_bytes += length;
        ++metrics_.output_chunks;
        last_output_time_ = std::chrono::steady_clock::now();
        output_waiting_for_render_ = true;
        tsm_vte_input(vte_, data, length);
    }
}

bool TerminalCore::HandleKey(uint32_t keysym, uint32_t ascii,
                             unsigned int modifiers, uint32_t unicode)
{
    ++metrics_.input_events;
    return vte_ != nullptr &&
           tsm_vte_handle_keyboard(vte_, keysym, ascii, modifiers, unicode);
}

void TerminalCore::Paste(const std::string& text)
{
    if (vte_ == nullptr || text.empty())
        return;
    metrics_.paste_bytes += text.size();
    tsm_vte_paste(vte_, text.c_str());
}

void TerminalCore::ScrollPageUp(unsigned int pages)
{
    if (screen_ != nullptr)
        tsm_screen_sb_page_up(screen_, pages);
}

void TerminalCore::ScrollPageDown(unsigned int pages)
{
    if (screen_ != nullptr)
        tsm_screen_sb_page_down(screen_, pages);
}

void TerminalCore::StartSelection(unsigned int column, unsigned int row)
{
    if (screen_ == nullptr || column >= tsm_screen_get_width(screen_) ||
        row >= tsm_screen_get_height(screen_))
        return;
    tsm_screen_selection_start(screen_, column, row);
    selection_active_ = true;
}

void TerminalCore::UpdateSelection(unsigned int column, unsigned int row)
{
    if (screen_ != nullptr && selection_active_)
        tsm_screen_selection_target(screen_, column, row);
}

void TerminalCore::ClearSelection()
{
    if (screen_ != nullptr)
        tsm_screen_selection_reset(screen_);
    selection_active_ = false;
}

std::string TerminalCore::CopySelection()
{
    if (screen_ == nullptr || !selection_active_)
        return {};

    char* text = nullptr;
    // libtsm returns the copied byte count on success and a negative errno
    // value on failure, rather than zero for success.
    if (tsm_screen_selection_copy(screen_, &text) < 0 || text == nullptr)
        return {};

    std::string result(text);
    std::free(text);
    if (!result.empty())
        ++metrics_.selection_copies;
    return result;
}

TerminalSnapshot TerminalCore::TakeSnapshot()
{
    TerminalSnapshot snapshot;
    if (screen_ == nullptr)
        return snapshot;

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

    snapshot.columns = tsm_screen_get_width(screen_);
    snapshot.rows = tsm_screen_get_height(screen_);
    snapshot.cursor_x = tsm_screen_get_cursor_x(screen_);
    snapshot.cursor_y = tsm_screen_get_cursor_y(screen_);
    snapshot.cursor_visible = vte_ != nullptr &&
        (tsm_vte_get_flags(vte_) & TSM_VTE_FLAG_TEXT_CURSOR_MODE) != 0;

    const struct tsm_screen_cell* cells = tsm_screen_draw2(screen_);
    if (cells == nullptr)
        return snapshot;

    snapshot.cells.resize(snapshot.columns * snapshot.rows);
    for (std::size_t index = 0; index < snapshot.cells.size(); ++index) {
        TerminalCell& target = snapshot.cells[index];
        const struct tsm_screen_cell& source = cells[index];
        target.codepoint = source.ch;
        target.foreground = {source.fg.r, source.fg.g, source.fg.b};
        target.background = {source.bg.r, source.bg.g, source.bg.b};
        target.attributes = source.attr2.u8;
    }
    return snapshot;
}

void TerminalCore::VteWrite(struct tsm_vte*, const char* data,
                            std::size_t length, void* user_data)
{
    auto* core = static_cast<TerminalCore*>(user_data);
    core->metrics_.transport_write_bytes += length;
    ++core->metrics_.transport_write_events;
    if (core->write_callback_)
        core->write_callback_(data, length);
}
