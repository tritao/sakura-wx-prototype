#include <sakura/wx/terminal_ctrl.h>

#include "terminal_renderer.h"

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <xkbcommon/xkbcommon-keysyms.h>

class WxTerminalCtrl::Impl {
public:
    Impl(WxTerminalCtrl& owner, std::unique_ptr<TerminalTransport> transport,
         TerminalConfig config, TerminalCallbacks callbacks)
        : owner_(owner),
          config_(std::move(config)),
          callbacks_(std::move(callbacks)),
          renderer_(owner, config_, paint_metrics_),
          output_timer_(&owner_),
          transport_(std::move(transport))
    {
        trace_scroll_ = std::getenv("SAKURA_TRACE_SCROLL") != nullptr;
        trace_keys_ = std::getenv("SAKURA_TRACE_KEYS") != nullptr;
        core_ = sakura_terminal_new_with_theme(&Impl::WriteBridge, this,
                                                &config_.theme);
    }

    ~Impl()
    {
        sakura_terminal_frame_free(pending_frame_);
        sakura_terminal_free(core_);
    }

    static void WriteBridge(void* userdata, const char* data,
                            std::size_t length)
    {
        auto* impl = static_cast<Impl*>(userdata);
        if (impl != nullptr && impl->transport_ != nullptr)
            impl->transport_->Write(data, length);
    }

    void AssertOwnerThread() const
    {
        assert(std::this_thread::get_id() == owner_thread_);
    }

    void TraceScroll(const char* format, ...) const
    {
        if (!trace_scroll_)
            return;
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                       trace_started_).count();
        std::fprintf(stderr, "[scroll +%lluus] ",
                     static_cast<unsigned long long>(std::max<int64_t>(
                         0, elapsed)));
        va_list arguments;
        va_start(arguments, format);
        std::vfprintf(stderr, format, arguments);
        va_end(arguments);
        std::fputc('\n', stderr);
    }

    void TraceKey(const wxKeyEvent& event, uint32_t keysym,
                  uint32_t ascii, uint32_t unicode) const
    {
        if (!trace_keys_)
            return;
        std::fprintf(stderr,
                     "[key] code=%d unicode=%u control=%d shift=%d alt=%d meta=%d keysym=%u ascii=%u forwarded-unicode=%u\n",
                     event.GetKeyCode(), event.GetUnicodeKey(),
                     event.ControlDown() ? 1 : 0, event.ShiftDown() ? 1 : 0,
                     event.AltDown() ? 1 : 0, event.MetaDown() ? 1 : 0,
                     keysym, ascii == SAKURA_TERMINAL_INVALID ? 0 : ascii,
                     unicode);
    }

    WxTerminalCtrl& owner_;
    std::thread::id owner_thread_ = std::this_thread::get_id();
    TerminalConfig config_;
    TerminalCallbacks callbacks_;
    SakuraTerminalFrame* pending_frame_ = nullptr;
    SakuraTerminalFrameInfo pending_info_ {};
    SakuraTerminalDirtyRegion pending_dirty_ {};
    bool pending_full_repaint_ = false;
    WxPaintMetrics paint_metrics_;
    WxRenderer renderer_;
    wxTimer output_timer_;
    std::unique_ptr<TerminalTransport> transport_;
    SakuraTerminal* core_ = nullptr;
    wxString error_;
    bool selection_dragging_ = false;
    bool pointer_down_ = false;
    bool mouse_reporting_gesture_ = false;
    bool trace_metrics_ = false;
    int wheel_rotation_ = 0;
    int pending_wheel_lines_ = 0;
    int click_count_ = 0;
    wxPoint pointer_down_position_;
    unsigned int selection_anchor_column_ = 0;
    unsigned int selection_anchor_row_ = 0;
    unsigned int last_click_column_ = 0;
    unsigned int last_click_row_ = 0;
    unsigned int mouse_reporting_button_ = SAKURA_TERMINAL_MOUSE_LEFT;
    int auto_scroll_direction_ = 0;
    wxPoint last_pointer_position_;
    std::chrono::steady_clock::time_point last_click_time_;
    TransportState last_transport_state_ = TransportState::Stopped;
    std::chrono::steady_clock::time_point last_metrics_log_;
    std::chrono::steady_clock::time_point last_metrics_callback_;
    std::chrono::steady_clock::time_point trace_started_ =
        std::chrono::steady_clock::now();
    std::string last_title_;
    std::string last_error_;
    bool trace_scroll_ = false;
    bool trace_keys_ = false;
};

WxTerminalCtrl::WxTerminalCtrl(
    wxWindow* parent, std::unique_ptr<TerminalTransport> transport,
    TerminalConfig config, TerminalCallbacks callbacks)
    : wxPanel(parent),
      impl_(std::make_unique<Impl>(*this, std::move(transport),
                                  std::move(config), std::move(callbacks)))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetFocus();

    Bind(wxEVT_PAINT, &WxTerminalCtrl::OnPaint, this);
    Bind(wxEVT_SIZE, &WxTerminalCtrl::OnSize, this);
    Bind(wxEVT_CHAR, &WxTerminalCtrl::OnChar, this);
    Bind(wxEVT_MOUSEWHEEL, &WxTerminalCtrl::OnMouseWheel, this);
    Bind(wxEVT_LEFT_DOWN, &WxTerminalCtrl::OnLeftDown, this);
    Bind(wxEVT_LEFT_DCLICK, &WxTerminalCtrl::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &WxTerminalCtrl::OnLeftUp, this);
    Bind(wxEVT_RIGHT_DOWN, &WxTerminalCtrl::OnMouseButtonDown, this);
    Bind(wxEVT_RIGHT_UP, &WxTerminalCtrl::OnMouseButtonUp, this);
    Bind(wxEVT_MIDDLE_DOWN, &WxTerminalCtrl::OnMouseButtonDown, this);
    Bind(wxEVT_MIDDLE_UP, &WxTerminalCtrl::OnMouseButtonUp, this);
    Bind(wxEVT_MOTION, &WxTerminalCtrl::OnMotion, this);
    Bind(wxEVT_TIMER, &WxTerminalCtrl::OnTimer, this);

    if (!sakura_terminal_is_ready(impl_->core_)) {
        impl_->error_ = wxString::FromUTF8(sakura_terminal_error(impl_->core_));
        ReportError(sakura_terminal_error(impl_->core_));
        return;
    }

    UpdateGeometry();
    const auto metrics_start = std::chrono::steady_clock::now();
    impl_->last_metrics_log_ = metrics_start;
    impl_->last_metrics_callback_ = metrics_start;
    if (impl_->transport_ != nullptr && impl_->config_.start_transport) {
        const char* shell = std::getenv("SHELL");
        if (!impl_->transport_->Start(impl_->renderer_.Columns(),
                                      impl_->renderer_.Rows(),
                                      shell == nullptr ? "" : shell)) {
            impl_->error_ = "No platform terminal transport is available";
            ReportError("No platform terminal transport is available");
            const char* notice =
                "\x1b[1;33mSakura wx terminal\x1b[0m\r\n"
                "This build has libtsm and wxWidgets, but could not start "
                "the platform process backend.\r\n";
            sakura_terminal_feed_output(impl_->core_, notice, std::strlen(notice));
        }
        const TransportStatus initial_status = impl_->transport_->GetStatus();
        impl_->last_transport_state_ = initial_status.state == TransportState::Failed
            ? TransportState::Failed
            : TransportState::Stopped;
        NotifyTitleChanged();
        NotifyTransportStatus(initial_status);
        impl_->trace_metrics_ = std::getenv("SAKURA_TRACE_METRICS") != nullptr;
        impl_->trace_scroll_ = impl_->trace_scroll_ || impl_->trace_metrics_;
        impl_->renderer_.SetTraceScroll(impl_->trace_scroll_);
    }
    impl_->output_timer_.Start(static_cast<int>(std::max(1u,
        impl_->config_.timer_interval_ms)));
}

WxTerminalCtrl::~WxTerminalCtrl()
{
    impl_->AssertOwnerThread();
    impl_->output_timer_.Stop();
    if (impl_->transport_ != nullptr)
        impl_->transport_->Stop();
}

SakuraTerminal* WxTerminalCtrl::Core()
{
    impl_->AssertOwnerThread();
    return impl_->core_;
}

const SakuraTerminal* WxTerminalCtrl::Core() const
{
    impl_->AssertOwnerThread();
    return impl_->core_;
}

std::string WxTerminalCtrl::GetFontFamily() const
{
    impl_->AssertOwnerThread();
    return impl_->renderer_.FontFamily();
}

int WxTerminalCtrl::GetFontSize() const
{
    impl_->AssertOwnerThread();
    return impl_->renderer_.FontSize();
}

wxSize WxTerminalCtrl::GetCellSize() const
{
    impl_->AssertOwnerThread();
    return impl_->renderer_.CellSize();
}

void WxTerminalCtrl::RefreshFrame()
{
    RequestFrameRefresh();
}

WxPaintMetrics WxTerminalCtrl::GetPaintMetrics() const
{
    impl_->AssertOwnerThread();
    return impl_->paint_metrics_;
}

void WxTerminalCtrl::RequestFrameRefresh()
{
    impl_->AssertOwnerThread();
    SakuraTerminalFrame* frame = sakura_terminal_take_frame(impl_->core_);
    SakuraTerminalFrameInfo info {};
    if (frame == nullptr || !sakura_terminal_frame_info(frame, &info)) {
        sakura_terminal_frame_free(frame);
        return;
    }
    if (!info.changed) {
        impl_->TraceScroll("paint unchanged animation_active=%d",
                           impl_->renderer_.ScrollAnimationActive() ? 1 : 0);
        sakura_terminal_frame_free(frame);
        return;
    }

    SakuraTerminalDirtyRegion dirty = info.dirty;
    bool full_repaint = info.full_repaint != 0;
    int scroll_delta = info.scroll_delta;
    SakuraTerminalScrollKind scroll_kind = info.scroll_kind;
    if (full_repaint)
        dirty = {0, 0, info.columns, info.rows};

    if (impl_->pending_frame_ != nullptr) {
        scroll_delta += impl_->pending_info_.scroll_delta;
        if (impl_->pending_info_.scroll_kind !=
                SAKURA_TERMINAL_SCROLL_NONE &&
            scroll_kind != SAKURA_TERMINAL_SCROLL_NONE &&
            impl_->pending_info_.scroll_kind != scroll_kind)
            scroll_kind = SAKURA_TERMINAL_SCROLL_MIXED;
        else if (scroll_kind == SAKURA_TERMINAL_SCROLL_NONE)
            scroll_kind = impl_->pending_info_.scroll_kind;
        if (impl_->pending_full_repaint_)
            dirty = {0, 0, info.columns, info.rows};
        else if (dirty.left < dirty.right && dirty.top < dirty.bottom) {
            SakuraTerminalDirtyRegion& pending_dirty = impl_->pending_dirty_;
            if (pending_dirty.left < pending_dirty.right &&
                pending_dirty.top < pending_dirty.bottom) {
                dirty.left = std::min(dirty.left, pending_dirty.left);
                dirty.top = std::min(dirty.top, pending_dirty.top);
                dirty.right = std::max(dirty.right, pending_dirty.right);
                dirty.bottom = std::max(dirty.bottom, pending_dirty.bottom);
            }
        }
        full_repaint = full_repaint || impl_->pending_full_repaint_;
        sakura_terminal_frame_free(impl_->pending_frame_);
    }
    impl_->pending_frame_ = frame;
    impl_->pending_info_ = info;
    impl_->pending_info_.scroll_delta = scroll_delta;
    impl_->pending_info_.scroll_kind = scroll_kind;
    impl_->pending_full_repaint_ = full_repaint;
    impl_->pending_dirty_ = dirty;
    impl_->pending_info_.full_repaint = full_repaint ? 1 : 0;
    impl_->pending_info_.dirty = dirty;

    ++impl_->paint_metrics_.refresh_requests;
    const SakuraTerminalFrameInfo& pending = impl_->pending_info_;
    if (pending.full_repaint || pending.dirty.left >= pending.dirty.right ||
        pending.dirty.top >= pending.dirty.bottom) {
        ++impl_->paint_metrics_.full_refresh_requests;
        Refresh(false);
        return;
    }

    const unsigned int left = std::min(pending.dirty.left, pending.columns);
    const unsigned int top = std::min(pending.dirty.top, pending.rows);
    const unsigned int right = std::min(pending.dirty.right, pending.columns);
    const unsigned int bottom = std::min(pending.dirty.bottom, pending.rows);
    if (left >= right || top >= bottom) {
        ++impl_->paint_metrics_.full_refresh_requests;
        Refresh(false);
        return;
    }

    ++impl_->paint_metrics_.dirty_refresh_requests;
    RefreshRect(wxRect(left * impl_->renderer_.CellWidth(),
                       top * impl_->renderer_.CellHeight(),
                       (right - left) * impl_->renderer_.CellWidth(),
                       (bottom - top) * impl_->renderer_.CellHeight()), false);
}

void WxTerminalCtrl::UpdateGeometry()
{
    if (!impl_->renderer_.UpdateGeometry())
        return;
    sakura_terminal_resize(impl_->core_, impl_->renderer_.Columns(),
                           impl_->renderer_.Rows());
    if (impl_->transport_ != nullptr)
        impl_->transport_->Resize(impl_->renderer_.Columns(),
                                  impl_->renderer_.Rows());
}

uint32_t WxTerminalCtrl::KeySymFor(const wxKeyEvent& event)
{
    switch (event.GetKeyCode()) {
    case WXK_BACK: return XKB_KEY_BackSpace;
    case WXK_TAB: return XKB_KEY_Tab;
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER: return XKB_KEY_Return;
    case WXK_ESCAPE: return XKB_KEY_Escape;
    case WXK_DELETE: return XKB_KEY_Delete;
    case WXK_INSERT: return XKB_KEY_Insert;
    case WXK_HOME: return XKB_KEY_Home;
    case WXK_END: return XKB_KEY_End;
    case WXK_PAGEUP: return XKB_KEY_Page_Up;
    case WXK_PAGEDOWN: return XKB_KEY_Page_Down;
    case WXK_LEFT: return XKB_KEY_Left;
    case WXK_RIGHT: return XKB_KEY_Right;
    case WXK_UP: return XKB_KEY_Up;
    case WXK_DOWN: return XKB_KEY_Down;
    default: break;
    }

    const int key = event.GetKeyCode();
    if (key >= WXK_F1 && key <= WXK_F12)
        return XKB_KEY_F1 + static_cast<uint32_t>(key - WXK_F1);
    return event.GetUnicodeKey();
}

void WxTerminalCtrl::OnPaint(wxPaintEvent&)
{
    const auto paint_start = std::chrono::steady_clock::now();
    ++impl_->paint_metrics_.paint_events;
    const auto record_paint = [this, &paint_start]() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - paint_start).count();
        const uint64_t elapsed_us = static_cast<uint64_t>(
            std::max<int64_t>(0, elapsed));
        impl_->renderer_.RecordPaintDuration(elapsed_us);
    };
    wxAutoBufferedPaintDC dc(this);
    const auto& background = impl_->config_.theme.background;
    dc.SetBackground(wxBrush(wxColour(background[0], background[1],
                                       background[2])));

    if (!sakura_terminal_is_ready(impl_->core_)) {
        dc.Clear();
        const auto& error_foreground = impl_->config_.error_foreground;
        dc.SetTextForeground(wxColour(error_foreground[0], error_foreground[1],
                                      error_foreground[2]));
        dc.DrawText(impl_->error_, 12, 12);
        record_paint();
        return;
    }

    SakuraTerminalFrame* frame = nullptr;
    SakuraTerminalFrameInfo info {};
    SakuraTerminalDirtyRegion dirty {};
    bool full_repaint = false;
    if (impl_->pending_frame_ != nullptr) {
        frame = impl_->pending_frame_;
        info = impl_->pending_info_;
        dirty = impl_->pending_dirty_;
        full_repaint = impl_->pending_full_repaint_;
        impl_->pending_frame_ = nullptr;
        impl_->pending_info_ = {};
        impl_->pending_dirty_ = {};
        impl_->pending_full_repaint_ = false;
    } else {
        frame = sakura_terminal_take_frame(impl_->core_);
        if (frame == nullptr || !sakura_terminal_frame_info(frame, &info)) {
            sakura_terminal_frame_free(frame);
            impl_->renderer_.Draw(dc);
            record_paint();
            return;
        }
        dirty = info.dirty;
        full_repaint = info.full_repaint != 0;
    }
    if (frame == nullptr) {
        impl_->renderer_.Draw(dc);
        record_paint();
        return;
    }
    if (!info.changed) {
        sakura_terminal_frame_free(frame);
        impl_->renderer_.Draw(dc);
        record_paint();
        return;
    }
    uint64_t painted_cells = 0;
    impl_->renderer_.PaintFrame(dc, frame, info, dirty, full_repaint,
                                &painted_cells);
    sakura_terminal_frame_free(frame);
    record_paint();
}

void WxTerminalCtrl::OnSize(wxSizeEvent& event)
{
    impl_->renderer_.CancelScrollAnimation(true, "size");
    UpdateGeometry();
    ++impl_->paint_metrics_.refresh_requests;
    ++impl_->paint_metrics_.full_refresh_requests;
    Refresh(false);
    event.Skip();
}

void WxTerminalCtrl::OnChar(wxKeyEvent& event)
{
    impl_->renderer_.CancelScrollAnimation(true, "key");
    const uint32_t key = static_cast<uint32_t>(event.GetKeyCode());
    const uint32_t unicode = event.GetUnicodeKey();
    const bool copy_key = key == 'c' || key == 'C' || unicode == 'c' ||
                          unicode == 'C' || key == WXK_CONTROL_C ||
                          unicode == WXK_CONTROL_C;
    const bool paste_key = key == 'v' || key == 'V' || unicode == 'v' ||
                           unicode == 'V' || key == WXK_CONTROL_V ||
                           unicode == WXK_CONTROL_V;
    const bool restart_key = key == 'r' || key == 'R' || unicode == 'r' ||
                             unicode == 'R' || key == WXK_CONTROL_R ||
                             unicode == WXK_CONTROL_R;
    const bool shortcut_modifier = event.CmdDown() || event.MetaDown();
    const bool physical_control = event.RawControlDown();
    const bool interrupt_key = physical_control &&
        (key == 'c' || key == 'C' || unicode == 'c' || unicode == 'C' ||
         key == WXK_CONTROL_C || unicode == WXK_CONTROL_C);
    if (shortcut_modifier && event.ShiftDown()) {
        if (restart_key) {
            RestartTransport();
            return;
        }
        if (copy_key) {
            CopySelectionToClipboard();
            return;
        }
        if (paste_key) {
            PasteFromClipboard();
            return;
        }
    }

    unsigned int modifiers = 0;
    if (event.ShiftDown()) modifiers |= SAKURA_TERMINAL_SHIFT;
    if (physical_control) modifiers |= SAKURA_TERMINAL_CONTROL;
    if (event.AltDown()) modifiers |= SAKURA_TERMINAL_ALT;
    // wx uses ControlDown() for the primary Command modifier on macOS. Do
    // not turn that into a terminal Ctrl byte; preserve it as the logo
    // modifier while RawControlDown() represents physical Ctrl.
    if (event.MetaDown() || (shortcut_modifier && !physical_control))
        modifiers |= SAKURA_TERMINAL_LOGO;

    uint32_t ascii = unicode <= 0x7f ? unicode : SAKURA_TERMINAL_INVALID;
    uint32_t keysym = KeySymFor(event);
    // wx backends report Ctrl-A..Ctrl-Z as control-code values (for example
    // Ctrl-C is U+0003). Normalize those values back to their letter keysyms
    // so libtsm takes its explicit control-key path on every platform
    // instead of relying on its Unicode fallback.
    const uint32_t control_code = unicode >= WXK_CONTROL_A &&
                                  unicode <= WXK_CONTROL_Z
        ? unicode
        : key;
    if (physical_control && control_code >= WXK_CONTROL_A &&
        control_code <= WXK_CONTROL_Z) {
        const uint32_t control_letter =
            static_cast<uint32_t>('a' + control_code - WXK_CONTROL_A);
        ascii = control_letter;
        keysym = control_letter;
    }
    impl_->TraceKey(event, keysym, ascii, unicode);
    if (sakura_terminal_handle_key(impl_->core_, keysym, ascii,
                                   modifiers, unicode)) {
        if (interrupt_key && impl_->transport_ != nullptr)
            impl_->transport_->DiscardOutput();
        RequestFrameRefresh();
        return;
    }
    event.Skip();
}

void WxTerminalCtrl::OnMouseWheel(wxMouseEvent& event)
{
    if (!event.ShiftDown() &&
        sakura_terminal_mouse_reporting_enabled(impl_->core_)) {
        impl_->TraceScroll("wheel route=application rotation=%d",
                           event.GetWheelRotation());
        impl_->renderer_.CancelScrollAnimation(true, "mouse-reporting");
        const unsigned int button = event.GetWheelRotation() > 0
            ? SAKURA_TERMINAL_MOUSE_WHEEL_UP
            : SAKURA_TERMINAL_MOUSE_WHEEL_DOWN;
        const auto [column, row] = CellAt(event.GetPosition());
        sakura_terminal_handle_mouse(
            impl_->core_, column, row,
            static_cast<unsigned int>(std::max(0, event.GetX())),
            static_cast<unsigned int>(std::max(0, event.GetY())), button,
            SAKURA_TERMINAL_MOUSE_PRESSED, MouseModifiers(event));
        return;
    }
    QueueWheelScroll(event);
}

void WxTerminalCtrl::QueueWheelScroll(const wxMouseEvent& event)
{
    impl_->AssertOwnerThread();
    const int rotation = event.GetWheelRotation();
    if (rotation == 0)
        return;

    constexpr int kWheelDelta = 120;
    const int event_delta = std::max(1, event.GetWheelDelta());
    const int normalized_rotation = static_cast<int>(
        (static_cast<int64_t>(rotation) * kWheelDelta) / event_delta);
    impl_->wheel_rotation_ += normalized_rotation;
    ++impl_->paint_metrics_.wheel_events;

    const int actions = impl_->wheel_rotation_ / kWheelDelta;
    impl_->TraceScroll(
        "wheel route=scrollback rotation=%d delta=%d lines=%d page=%d "
        "normalized=%d accumulator=%d actions=%d",
        rotation, event_delta, event.GetLinesPerAction(),
        event.IsPageScroll() ? 1 : 0, normalized_rotation,
        impl_->wheel_rotation_, actions);
    if (actions == 0) {
        ++impl_->paint_metrics_.wheel_partial_events;
        return;
    }
    impl_->wheel_rotation_ -= actions * kWheelDelta;

    const int lines_per_action = event.IsPageScroll()
        ? std::max(1, impl_->renderer_.Rows() > 1
            ? static_cast<int>(impl_->renderer_.Rows() - 1) : 1)
        : std::max(1, event.GetLinesPerAction());
    constexpr int kMaxPendingWheelLines = 64;
    impl_->pending_wheel_lines_ = std::clamp(
        impl_->pending_wheel_lines_ + actions * lines_per_action,
        -kMaxPendingWheelLines, kMaxPendingWheelLines);
}

bool WxTerminalCtrl::FlushWheelScroll()
{
    impl_->AssertOwnerThread();
    if (impl_->pending_wheel_lines_ == 0 ||
        (impl_->config_.smooth_scrolling &&
         impl_->renderer_.ScrollAnimationActive()))
        return false;

    int max_lines = 1;
    if (impl_->config_.smooth_scrolling)
        max_lines = std::max(1, impl_->renderer_.Rows() > 1
            ? static_cast<int>(impl_->renderer_.Rows() - 1) : 1);
    const int pending_magnitude = std::abs(impl_->pending_wheel_lines_);
    const int magnitude = std::min(pending_magnitude, max_lines);
    const int lines = impl_->pending_wheel_lines_ > 0
        ? magnitude : -magnitude;
    impl_->pending_wheel_lines_ -= lines;
    sakura_terminal_scroll_lines(impl_->core_, lines);
    impl_->TraceScroll("queue lines=%d pending=%d", lines,
                       impl_->pending_wheel_lines_);
    ++impl_->paint_metrics_.wheel_scroll_updates;
    impl_->paint_metrics_.wheel_lines_scrolled +=
        static_cast<uint64_t>(magnitude);
    return true;
}

std::pair<unsigned int, unsigned int>
WxTerminalCtrl::CellAt(const wxPoint& point) const
{
    const int column = std::max(0, point.x) /
        std::max(1, impl_->renderer_.CellWidth());
    const int row = std::max(0, point.y) /
        std::max(1, impl_->renderer_.CellHeight());
    return {
        std::min(static_cast<unsigned int>(column),
                 impl_->renderer_.Columns() - 1),
        std::min(static_cast<unsigned int>(row),
                 impl_->renderer_.Rows() - 1),
    };
}

unsigned char WxTerminalCtrl::MouseModifiers(const wxMouseEvent& event)
{
    unsigned char modifiers = 0;
    if (event.ShiftDown()) modifiers |= SAKURA_TERMINAL_MOUSE_SHIFT;
    if (event.MetaDown()) modifiers |= SAKURA_TERMINAL_MOUSE_META;
    if (event.ControlDown()) modifiers |= SAKURA_TERMINAL_MOUSE_CONTROL;
    return modifiers;
}

bool WxTerminalCtrl::ForwardMouse(const wxMouseEvent& event,
                                  unsigned int button,
                                  unsigned int mouse_event)
{
    if (event.ShiftDown() ||
        !sakura_terminal_mouse_reporting_enabled(impl_->core_))
        return false;
    const auto [column, row] = CellAt(event.GetPosition());
    return sakura_terminal_handle_mouse(
        impl_->core_, column, row,
        static_cast<unsigned int>(std::max(0, event.GetX())),
        static_cast<unsigned int>(std::max(0, event.GetY())),
        button, mouse_event, MouseModifiers(event));
}

void WxTerminalCtrl::BeginMouseReporting(unsigned int button)
{
    impl_->mouse_reporting_gesture_ = true;
    impl_->mouse_reporting_button_ = button;
    CaptureMouse();
}

void WxTerminalCtrl::EndMouseReporting(const wxMouseEvent& event)
{
    if (!impl_->mouse_reporting_gesture_)
        return;
    const auto [column, row] = CellAt(event.GetPosition());
    sakura_terminal_handle_mouse(
        impl_->core_, column, row,
        static_cast<unsigned int>(std::max(0, event.GetX())),
        static_cast<unsigned int>(std::max(0, event.GetY())),
        impl_->mouse_reporting_button_, SAKURA_TERMINAL_MOUSE_RELEASED,
        MouseModifiers(event));
    impl_->mouse_reporting_gesture_ = false;
    if (HasCapture())
        ReleaseMouse();
}

void WxTerminalCtrl::OnLeftDown(wxMouseEvent& event)
{
    impl_->renderer_.CancelScrollAnimation(true, "mouse-down");
    SetFocus();
    const auto [column, row] = CellAt(event.GetPosition());
    if (ForwardMouse(event, SAKURA_TERMINAL_MOUSE_LEFT,
                     SAKURA_TERMINAL_MOUSE_PRESSED)) {
        BeginMouseReporting(SAKURA_TERMINAL_MOUSE_LEFT);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool same_cell = column == impl_->last_click_column_ &&
                           row == impl_->last_click_row_;
    const bool quick_click = now - impl_->last_click_time_ <
                             std::chrono::milliseconds(500);
    if (event.GetClickCount() >= 2 || (same_cell && quick_click))
        impl_->click_count_ = impl_->click_count_ % 3 + 1;
    else
        impl_->click_count_ = 1;
    impl_->last_click_time_ = now;
    impl_->last_click_column_ = column;
    impl_->last_click_row_ = row;

    const bool extend_selection = event.ShiftDown() &&
        sakura_terminal_has_selection(impl_->core_);
    if (extend_selection) {
        sakura_terminal_update_selection(impl_->core_, column, row);
    } else {
        sakura_terminal_clear_selection(impl_->core_);
        if (impl_->click_count_ == 2)
            sakura_terminal_select_word(impl_->core_, column, row);
        else if (impl_->click_count_ == 3)
            sakura_terminal_select_line(impl_->core_, row);
    }

    impl_->pointer_down_ = true;
    impl_->selection_dragging_ = extend_selection;
    impl_->pointer_down_position_ = event.GetPosition();
    impl_->selection_anchor_column_ = column;
    impl_->selection_anchor_row_ = row;
    CaptureMouse();
    RequestFrameRefresh();
}

void WxTerminalCtrl::OnLeftUp(wxMouseEvent& event)
{
    if (impl_->mouse_reporting_gesture_) {
        EndMouseReporting(event);
        return;
    }
    if (!impl_->pointer_down_)
        return;
    const auto [column, row] = CellAt(event.GetPosition());
    const bool should_copy = impl_->selection_dragging_ || impl_->click_count_ >= 2;
    if (impl_->selection_dragging_)
        sakura_terminal_update_selection(impl_->core_, column, row);
    impl_->selection_dragging_ = false;
    impl_->pointer_down_ = false;
    impl_->auto_scroll_direction_ = 0;
    if (HasCapture())
        ReleaseMouse();
    if (should_copy)
        CopySelectionToClipboard();
    RequestFrameRefresh();
}

void WxTerminalCtrl::OnMouseButtonDown(wxMouseEvent& event)
{
    const unsigned int button = event.RightDown()
        ? SAKURA_TERMINAL_MOUSE_RIGHT
        : SAKURA_TERMINAL_MOUSE_MIDDLE;
    if (ForwardMouse(event, button, SAKURA_TERMINAL_MOUSE_PRESSED))
        BeginMouseReporting(button);
}

void WxTerminalCtrl::OnMouseButtonUp(wxMouseEvent& event)
{
    if (impl_->mouse_reporting_gesture_)
        EndMouseReporting(event);
}

void WxTerminalCtrl::UpdateSelectionAt(const wxPoint& position)
{
    impl_->renderer_.CancelScrollAnimation(true, "selection");
    impl_->auto_scroll_direction_ = 0;
    if (position.y < 0)
        impl_->auto_scroll_direction_ = -1;
    else if (position.y >= GetClientSize().GetHeight())
        impl_->auto_scroll_direction_ = 1;

    if (impl_->auto_scroll_direction_ != 0)
        sakura_terminal_scroll_lines(impl_->core_,
                                     impl_->auto_scroll_direction_ < 0 ? 1 : -1);

    const auto [column, row] = CellAt(position);
    sakura_terminal_update_selection(impl_->core_, column, row);
}

void WxTerminalCtrl::OnMotion(wxMouseEvent& event)
{
    if (impl_->mouse_reporting_gesture_) {
        if (event.Dragging()) {
            const auto [column, row] = CellAt(event.GetPosition());
            sakura_terminal_handle_mouse(
                impl_->core_, column, row,
                static_cast<unsigned int>(std::max(0, event.GetX())),
                static_cast<unsigned int>(std::max(0, event.GetY())),
                32 + impl_->mouse_reporting_button_, SAKURA_TERMINAL_MOUSE_MOVED,
                MouseModifiers(event));
        }
        return;
    }
    if (!impl_->pointer_down_ || !event.Dragging()) {
        if (!event.ShiftDown() &&
            sakura_terminal_mouse_reporting_enabled(impl_->core_)) {
            const auto [column, row] = CellAt(event.GetPosition());
            sakura_terminal_handle_mouse(
                impl_->core_, column, row,
                static_cast<unsigned int>(std::max(0, event.GetX())),
                static_cast<unsigned int>(std::max(0, event.GetY())),
                SAKURA_TERMINAL_MOUSE_LEFT, SAKURA_TERMINAL_MOUSE_MOVED,
                MouseModifiers(event));
        }
        return;
    }
    const wxPoint position = event.GetPosition();
    const int distance_x = std::abs(position.x - impl_->pointer_down_position_.x);
    const int distance_y = std::abs(position.y - impl_->pointer_down_position_.y);
    if (!impl_->selection_dragging_ && (distance_x >= 4 || distance_y >= 4)) {
        sakura_terminal_start_selection(impl_->core_,
                                        impl_->selection_anchor_column_,
                                        impl_->selection_anchor_row_);
        impl_->selection_dragging_ = true;
    }
    if (!impl_->selection_dragging_)
        return;
    impl_->last_pointer_position_ = position;
    UpdateSelectionAt(position);
    RequestFrameRefresh();
}

bool WxTerminalCtrl::CopySelectionToClipboard()
{
    char* copied_text = sakura_terminal_copy_selection(impl_->core_);
    if (copied_text == nullptr || *copied_text == '\0' ||
        wxTheClipboard == nullptr || !wxTheClipboard->Open()) {
        sakura_terminal_free_string(copied_text);
        return false;
    }
    const std::string text(copied_text);
    sakura_terminal_free_string(copied_text);
    const bool copied = wxTheClipboard->SetData(
        new wxTextDataObject(wxString::FromUTF8(text)));
    wxTheClipboard->Close();
    return copied;
}

bool WxTerminalCtrl::PasteFromClipboard()
{
    if (wxTheClipboard == nullptr || !wxTheClipboard->Open())
        return false;
    wxTextDataObject data;
    const bool available = wxTheClipboard->GetData(data);
    wxTheClipboard->Close();
    if (!available)
        return false;

    const wxString text = data.GetText();
    const auto utf8 = text.ToUTF8();
    if (utf8.data() == nullptr)
        return false;
    sakura_terminal_paste(impl_->core_, utf8.data(), utf8.length());
    RequestFrameRefresh();
    return true;
}

void WxTerminalCtrl::NotifyTitleChanged()
{
    impl_->AssertOwnerThread();
    const char* title_value = sakura_terminal_title(impl_->core_);
    const std::string title = title_value == nullptr ? "" : title_value;
    if (title == impl_->last_title_)
        return;
    impl_->last_title_ = title;
    if (impl_->callbacks_.on_title_changed)
        impl_->callbacks_.on_title_changed(title);
}

void WxTerminalCtrl::NotifyTransportStatus(const TransportStatus& status)
{
    impl_->AssertOwnerThread();
    if (impl_->callbacks_.on_transport_status_changed)
        impl_->callbacks_.on_transport_status_changed(status);
}

void WxTerminalCtrl::ReportError(const std::string& message)
{
    impl_->AssertOwnerThread();
    if (message.empty() || message == impl_->last_error_)
        return;
    impl_->last_error_ = message;
    if (impl_->callbacks_.on_error)
        impl_->callbacks_.on_error(message);
}

void WxTerminalCtrl::ShowTransportNotice(const TransportStatus& status)
{
    std::string notice = "\r\n\x1b[1;33m[terminal process ";
    switch (status.state) {
    case TransportState::Running:
        notice += "started";
        break;
    case TransportState::Exited:
        notice += "exited";
        if (status.exit_code_valid && status.signal == 0)
            notice += " with code " + std::to_string(status.exit_code);
        else if (status.exit_code_valid && status.signal != 0)
            notice += " on signal " + std::to_string(status.signal);
        break;
    case TransportState::Failed:
        notice += "failed to start";
        break;
    case TransportState::Starting:
        notice += "starting";
        break;
    case TransportState::Stopped:
        notice += "stopped";
        break;
    }
    notice += "]\x1b[0m\r\n";
    sakura_terminal_feed_output(impl_->core_, notice.data(), notice.size());
}

void WxTerminalCtrl::RestartTransport()
{
    if (impl_->transport_ == nullptr)
        return;
    impl_->transport_->Stop();
    sakura_terminal_clear_selection(impl_->core_);
    const char* shell = std::getenv("SHELL");
    impl_->transport_->Start(impl_->renderer_.Columns(),
                             impl_->renderer_.Rows(),
                             shell == nullptr ? "" : shell);
    const char* clear = "\x1b[2J\x1b[H";
    sakura_terminal_feed_output(impl_->core_, clear, std::strlen(clear));
    const TransportStatus status = impl_->transport_->GetStatus();
    ShowTransportNotice(status);
    impl_->last_transport_state_ = status.state == TransportState::Failed
        ? TransportState::Failed
        : TransportState::Stopped;
    NotifyTransportStatus(status);
    if (status.state == TransportState::Failed)
        ReportError("Terminal process failed to start");
    RequestFrameRefresh();
}

void WxTerminalCtrl::UpdateTransportStatus()
{
    if (impl_->transport_ == nullptr)
        return;
    const TransportStatus status = impl_->transport_->GetStatus();
    if (status.state == impl_->last_transport_state_)
        return;
    impl_->last_transport_state_ = status.state;
    NotifyTransportStatus(status);
    if (status.state == TransportState::Failed)
        ReportError("Terminal process failed to start");
    if (status.state == TransportState::Exited ||
        status.state == TransportState::Failed)
        ShowTransportNotice(status);
    RequestFrameRefresh();
}

void WxTerminalCtrl::OnTimer(wxTimerEvent&)
{
    impl_->AssertOwnerThread();
    const bool animation_changed = impl_->renderer_.AdvanceScrollAnimation();
    if (!impl_->renderer_.ScrollAnimationActive() && FlushWheelScroll())
        RequestFrameRefresh();
    else if (animation_changed)
        Refresh(false);
    if (impl_->selection_dragging_ && impl_->auto_scroll_direction_ != 0) {
        impl_->renderer_.CancelScrollAnimation(true, "selection-autoscroll");
        sakura_terminal_scroll_lines(impl_->core_,
                                     impl_->auto_scroll_direction_ < 0 ? 1 : -1);
        const auto [column, row] = CellAt(impl_->last_pointer_position_);
        sakura_terminal_update_selection(impl_->core_, column, row);
        RequestFrameRefresh();
    }
    const auto output = impl_->transport_ != nullptr
        ? impl_->transport_->TakeOutput(std::max<std::size_t>(
              1, impl_->config_.output_bytes_per_tick))
        : std::vector<std::string> {};
    for (const auto& chunk : output)
        sakura_terminal_feed_output(impl_->core_, chunk.data(), chunk.size());
    if (!output.empty()) {
        impl_->renderer_.CancelScrollAnimation(true, "output");
        RequestFrameRefresh();
    }
    NotifyTitleChanged();
    UpdateTransportStatus();

    const auto now = std::chrono::steady_clock::now();
    if (impl_->callbacks_.on_metrics && impl_->config_.metrics_interval_ms > 0 &&
        now - impl_->last_metrics_callback_ >= std::chrono::milliseconds(
            impl_->config_.metrics_interval_ms)) {
        SakuraTerminalMetrics core_metrics {};
        sakura_terminal_get_metrics(impl_->core_, &core_metrics);
        const TransportMetrics transport_metrics = impl_->transport_ != nullptr
            ? impl_->transport_->GetMetrics() : TransportMetrics {};
        impl_->callbacks_.on_metrics(core_metrics, transport_metrics);
        impl_->last_metrics_callback_ = now;
    }

    if (impl_->trace_metrics_) {
        if (now - impl_->last_metrics_log_ >= std::chrono::seconds(1)) {
            SakuraTerminalMetrics core_metrics {};
            sakura_terminal_get_metrics(impl_->core_, &core_metrics);
            const TransportMetrics transport_metrics = impl_->transport_ != nullptr
                ? impl_->transport_->GetMetrics() : TransportMetrics {};
            std::fprintf(stderr,
                         "[metrics] output=%lluB/%llu chunks input=%llu "
                         "writes=%lluB/%llu renders=%llu selection=%llu "
                         "latency-max=%lluus/%llu paste=%lluB "
                         "mouse=%llu/%llu modes=%llu "
                         "paint=%llu full/%llu partial cells=%llu "
                         "paint-us=%llu/%llu/%llu max=%llu refresh=%llu/%llu dirty "
                         "wheel=%llu partial=%llu updates=%llu lines=%llu "
                         "animation=%llu/%llu/%llu/%llu settles=%llu "
                         "glyph-cache=%llu/%llu bypass=%llu evictions=%llu "
                         "bytes=%llu/%llu "
                         "transport-read=%lluB/%llu "
                         "queue-high-water=%llu resize=%llu\n",
                         static_cast<unsigned long long>(core_metrics.output_bytes),
                         static_cast<unsigned long long>(core_metrics.output_chunks),
                         static_cast<unsigned long long>(core_metrics.input_events),
                         static_cast<unsigned long long>(core_metrics.transport_write_bytes),
                         static_cast<unsigned long long>(core_metrics.transport_write_events),
                         static_cast<unsigned long long>(core_metrics.rendered_frames),
                         static_cast<unsigned long long>(core_metrics.selection_copies),
                         static_cast<unsigned long long>(core_metrics.max_render_latency_us),
                         static_cast<unsigned long long>(core_metrics.render_latency_samples),
                         static_cast<unsigned long long>(core_metrics.paste_bytes),
                         static_cast<unsigned long long>(core_metrics.mouse_events),
                         static_cast<unsigned long long>(core_metrics.mouse_events_forwarded),
                         static_cast<unsigned long long>(core_metrics.mouse_mode_changes),
                         static_cast<unsigned long long>(impl_->paint_metrics_.full_repaints),
                         static_cast<unsigned long long>(impl_->paint_metrics_.partial_repaints),
                         static_cast<unsigned long long>(impl_->paint_metrics_.painted_cells),
                         static_cast<unsigned long long>(impl_->paint_metrics_.p50_paint_time_us),
                         static_cast<unsigned long long>(impl_->paint_metrics_.p95_paint_time_us),
                         static_cast<unsigned long long>(impl_->paint_metrics_.p99_paint_time_us),
                         static_cast<unsigned long long>(impl_->paint_metrics_.max_paint_time_us),
                         static_cast<unsigned long long>(impl_->paint_metrics_.refresh_requests),
                         static_cast<unsigned long long>(impl_->paint_metrics_.dirty_refresh_requests),
                         static_cast<unsigned long long>(impl_->paint_metrics_.wheel_events),
                         static_cast<unsigned long long>(impl_->paint_metrics_.wheel_partial_events),
                         static_cast<unsigned long long>(impl_->paint_metrics_.wheel_scroll_updates),
                         static_cast<unsigned long long>(impl_->paint_metrics_.wheel_lines_scrolled),
                         static_cast<unsigned long long>(impl_->paint_metrics_.scroll_animation_starts),
                         static_cast<unsigned long long>(impl_->paint_metrics_.scroll_animation_frames),
                         static_cast<unsigned long long>(impl_->paint_metrics_.scroll_animation_paints),
                         static_cast<unsigned long long>(impl_->paint_metrics_.scroll_animation_completions),
                         static_cast<unsigned long long>(impl_->paint_metrics_.scroll_animation_settles),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_hits),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_misses),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_bypasses),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_evictions),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_bytes),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_peak_bytes),
                         static_cast<unsigned long long>(transport_metrics.bytes_read),
                         static_cast<unsigned long long>(transport_metrics.read_events),
                         static_cast<unsigned long long>(transport_metrics.max_queued_bytes),
                         static_cast<unsigned long long>(transport_metrics.resize_events));
            impl_->last_metrics_log_ = now;
        }
    }
}
