#include <sakura/wx/terminal_ctrl.h>

#include <tsm/libtsm.h>

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

uint32_t ColorKey(const std::array<uint8_t, 3>& color)
{
    return (static_cast<uint32_t>(color[0]) << 16) |
           (static_cast<uint32_t>(color[1]) << 8) |
           static_cast<uint32_t>(color[2]);
}

} // namespace

class WxTerminalCtrl::Impl {
public:
    Impl(WxTerminalCtrl& owner, std::unique_ptr<TerminalTransport> transport,
         TerminalConfig config, TerminalCallbacks callbacks)
        : owner_(owner),
          config_(std::move(config)),
          callbacks_(std::move(callbacks)),
          output_timer_(&owner_),
          transport_(std::move(transport)),
          core_([this](const char* data, std::size_t length) {
              if (transport_ != nullptr)
                  transport_->Write(data, length);
          })
    {
        const auto& background = config_.background;
        background_brush_ = wxBrush(wxColour(background[0], background[1],
                                              background[2]));
    }

    void AssertOwnerThread() const
    {
        assert(std::this_thread::get_id() == owner_thread_);
    }

    struct ColorResources {
        wxColour color;
        wxBrush brush;
        wxPen pen;
    };

    const ColorResources& ColorResourcesFor(
        const std::array<uint8_t, 3>& color)
    {
        const uint32_t key = ColorKey(color);
        const auto existing = color_resources_.find(key);
        if (existing != color_resources_.end())
            return existing->second;
        if (color_resources_.size() >= 4096)
            color_resources_.clear();

        const wxColour wx_color(color[0], color[1], color[2]);
        const auto inserted = color_resources_.emplace(
            key, ColorResources {wx_color, wxBrush(wx_color), wxPen(wx_color)});
        return inserted.first->second;
    }

    WxTerminalCtrl& owner_;
    std::thread::id owner_thread_ = std::this_thread::get_id();
    TerminalConfig config_;
    TerminalCallbacks callbacks_;
    wxFont font_;
    std::array<wxFont, 8> glyph_fonts_;
    std::array<bool, 8> glyph_fonts_valid_ {};
    std::unordered_map<std::string, wxString> glyph_texts_;
    std::unordered_map<uint32_t, ColorResources> color_resources_;
    wxBrush background_brush_;
    wxBitmap framebuffer_;
    bool framebuffer_valid_ = false;
    TerminalFrame pending_frame_;
    WxPaintMetrics paint_metrics_;
    wxTimer output_timer_;
    std::unique_ptr<TerminalTransport> transport_;
    TerminalCore core_;
    wxString error_;
    int cell_width_ = 8;
    int cell_height_ = 16;
    unsigned int columns_ = 80;
    unsigned int rows_ = 24;
    bool selection_dragging_ = false;
    bool pointer_down_ = false;
    bool mouse_reporting_gesture_ = false;
    bool trace_metrics_ = false;
    int click_count_ = 0;
    wxPoint pointer_down_position_;
    unsigned int selection_anchor_column_ = 0;
    unsigned int selection_anchor_row_ = 0;
    unsigned int last_click_column_ = 0;
    unsigned int last_click_row_ = 0;
    unsigned int mouse_reporting_button_ = TerminalMouseLeft;
    int auto_scroll_direction_ = 0;
    wxPoint last_pointer_position_;
    std::chrono::steady_clock::time_point last_click_time_;
    TransportState last_transport_state_ = TransportState::Stopped;
    std::chrono::steady_clock::time_point last_metrics_log_;
    std::chrono::steady_clock::time_point last_metrics_callback_;
    std::string last_title_;
    std::string last_error_;
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

    impl_->font_ = wxFont(std::max(1, impl_->config_.font_size),
                          wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                          wxFONTWEIGHT_NORMAL, false,
                          wxString::FromUTF8(impl_->config_.font_family.c_str()));

    if (!impl_->core_.IsReady()) {
        impl_->error_ = wxString::FromUTF8(impl_->core_.Error().c_str());
        ReportError(impl_->core_.Error());
        return;
    }

    UpdateGeometry();
    const auto metrics_start = std::chrono::steady_clock::now();
    impl_->last_metrics_log_ = metrics_start;
    impl_->last_metrics_callback_ = metrics_start;
    if (impl_->transport_ != nullptr && impl_->config_.start_transport) {
        const char* shell = std::getenv("SHELL");
        if (!impl_->transport_->Start(impl_->columns_, impl_->rows_,
                                      shell == nullptr ? "" : shell)) {
            impl_->error_ = "No platform terminal transport is available";
            ReportError("No platform terminal transport is available");
            const char* notice =
                "\x1b[1;33mSakura wx terminal\x1b[0m\r\n"
                "This build has libtsm and wxWidgets, but could not start "
                "the platform process backend.\r\n";
            impl_->core_.FeedOutput(notice, std::strlen(notice));
        }
        const TransportStatus initial_status = impl_->transport_->GetStatus();
        impl_->last_transport_state_ = initial_status.state == TransportState::Failed
            ? TransportState::Failed
            : TransportState::Stopped;
        NotifyTitleChanged();
        NotifyTransportStatus(initial_status);
        impl_->trace_metrics_ = std::getenv("SAKURA_TRACE_METRICS") != nullptr;
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

TerminalCore& WxTerminalCtrl::Core()
{
    impl_->AssertOwnerThread();
    return impl_->core_;
}

const TerminalCore& WxTerminalCtrl::Core() const
{
    impl_->AssertOwnerThread();
    return impl_->core_;
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
    const TerminalFrame frame = impl_->core_.TakeFrame();
    if (frame.snapshot == nullptr || !frame.changed)
        return;

    if (impl_->pending_frame_.snapshot == nullptr) {
        impl_->pending_frame_ = frame;
    } else {
        const TerminalSnapshot& snapshot = *frame.snapshot;
        if (frame.full_repaint) {
            impl_->pending_frame_.dirty = {
                0, 0, snapshot.columns, snapshot.rows
            };
        } else if (!frame.dirty.IsEmpty()) {
            TerminalDirtyRegion& dirty = impl_->pending_frame_.dirty;
            if (dirty.IsEmpty()) {
                dirty = frame.dirty;
            } else {
                dirty.left = std::min(dirty.left, frame.dirty.left);
                dirty.top = std::min(dirty.top, frame.dirty.top);
                dirty.right = std::max(dirty.right, frame.dirty.right);
                dirty.bottom = std::max(dirty.bottom, frame.dirty.bottom);
            }
        }
        impl_->pending_frame_.generation = frame.generation;
        impl_->pending_frame_.changed = true;
        impl_->pending_frame_.full_repaint =
            impl_->pending_frame_.full_repaint || frame.full_repaint;
        impl_->pending_frame_.snapshot = frame.snapshot;
    }

    ++impl_->paint_metrics_.refresh_requests;
    const TerminalFrame& pending = impl_->pending_frame_;
    if (pending.full_repaint || pending.dirty.IsEmpty()) {
        ++impl_->paint_metrics_.full_refresh_requests;
        Refresh(false);
        return;
    }

    const TerminalSnapshot& snapshot = *pending.snapshot;
    const unsigned int left = std::min(pending.dirty.left, snapshot.columns);
    const unsigned int top = std::min(pending.dirty.top, snapshot.rows);
    const unsigned int right = std::min(pending.dirty.right, snapshot.columns);
    const unsigned int bottom = std::min(pending.dirty.bottom, snapshot.rows);
    if (left >= right || top >= bottom) {
        ++impl_->paint_metrics_.full_refresh_requests;
        Refresh(false);
        return;
    }

    ++impl_->paint_metrics_.dirty_refresh_requests;
    RefreshRect(wxRect(left * impl_->cell_width_,
                       top * impl_->cell_height_,
                       (right - left) * impl_->cell_width_,
                       (bottom - top) * impl_->cell_height_), false);
}

void WxTerminalCtrl::UpdateGeometry()
{
    wxClientDC dc(this);
    dc.SetFont(impl_->font_);
    dc.GetTextExtent("M", &impl_->cell_width_, &impl_->cell_height_);
    impl_->cell_width_ = std::max(impl_->cell_width_, 1);
    impl_->cell_height_ = std::max(impl_->cell_height_, 1);

    const wxSize size = GetClientSize();
    const unsigned int columns = static_cast<unsigned int>(
        std::max(1, size.GetWidth() / impl_->cell_width_));
    const unsigned int rows = static_cast<unsigned int>(
        std::max(1, size.GetHeight() / impl_->cell_height_));

    if (columns == impl_->columns_ && rows == impl_->rows_)
        return;
    impl_->columns_ = columns;
    impl_->rows_ = rows;
    impl_->core_.Resize(impl_->columns_, impl_->rows_);
    if (impl_->transport_ != nullptr)
        impl_->transport_->Resize(impl_->columns_, impl_->rows_);
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

const wxFont& WxTerminalCtrl::GlyphFont(uint8_t attributes)
{
    const std::size_t index = attributes & 0x07;
    if (!impl_->glyph_fonts_valid_[index]) {
        wxFont& glyph_font = impl_->glyph_fonts_[index];
        glyph_font = impl_->font_;
        glyph_font.SetWeight((attributes & 0x01) != 0
            ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        glyph_font.SetStyle((attributes & 0x02) != 0
            ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);
        glyph_font.SetUnderlined((attributes & 0x04) != 0);
        impl_->glyph_fonts_valid_[index] = true;
    }
    return impl_->glyph_fonts_[index];
}

const wxString& WxTerminalCtrl::GlyphText(const std::string& text)
{
    constexpr std::size_t max_glyph_texts = 4096;
    const auto existing = impl_->glyph_texts_.find(text);
    if (existing != impl_->glyph_texts_.end())
        return existing->second;
    if (impl_->glyph_texts_.size() >= max_glyph_texts)
        impl_->glyph_texts_.clear();
    const auto inserted = impl_->glyph_texts_.emplace(
        text, wxString::FromUTF8(text));
    return inserted.first->second;
}

void WxTerminalCtrl::RenderSnapshot(wxDC& dc,
                                    const TerminalSnapshot& snapshot,
                                    const TerminalDirtyRegion& dirty,
                                    uint64_t* painted_cells)
{
    if (dirty.IsEmpty() || snapshot.columns == 0 || snapshot.rows == 0)
        return;

    const unsigned int left = std::min(dirty.left, snapshot.columns);
    const unsigned int top = std::min(dirty.top, snapshot.rows);
    const unsigned int right = std::min(dirty.right, snapshot.columns);
    const unsigned int bottom = std::min(dirty.bottom, snapshot.rows);
    if (left >= right || top >= bottom)
        return;

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(impl_->background_brush_);
    dc.DrawRectangle(left * impl_->cell_width_, top * impl_->cell_height_,
                     (right - left) * impl_->cell_width_,
                     (bottom - top) * impl_->cell_height_);
    dc.SetFont(impl_->font_);

    uint32_t current_background = 0xffffffffu;
    uint32_t current_foreground = 0xffffffffu;
    uint8_t current_font_attributes = 0xffu;

    for (unsigned int row = top; row < bottom; ++row) {
        wxString glyph_run;
        unsigned int glyph_run_start = 0;
        unsigned int glyph_run_end = 0;
        uint32_t glyph_run_foreground = 0xffffffffu;
        uint8_t glyph_run_font_attributes = 0xffu;
        const auto flush_glyph_run = [&]() {
            if (glyph_run.empty())
                return;
            dc.DrawText(glyph_run,
                        glyph_run_start * impl_->cell_width_,
                        row * impl_->cell_height_);
            glyph_run.clear();
        };

        for (unsigned int column = left; column < right; ++column) {
            if (painted_cells != nullptr)
                ++*painted_cells;
            const auto& cell = snapshot.cells[row * snapshot.columns + column];
            if (cell.width == 0) {
                flush_glyph_run();
                continue;
            }
            const unsigned int span = std::min(
                std::max(1u, cell.width), snapshot.columns - column);
            const uint32_t background_key = ColorKey(cell.background);
            if (background_key != current_background) {
                dc.SetBrush(impl_->ColorResourcesFor(cell.background).brush);
                current_background = background_key;
            }
            dc.DrawRectangle(column * impl_->cell_width_, row * impl_->cell_height_,
                             span * impl_->cell_width_, impl_->cell_height_);

            const uint32_t foreground_key = ColorKey(cell.foreground);
            if (foreground_key != current_foreground) {
                flush_glyph_run();
                dc.SetTextForeground(
                    impl_->ColorResourcesFor(cell.foreground).color);
                current_foreground = foreground_key;
            }
            const uint8_t font_attributes = cell.attributes & 0x07;
            if (font_attributes != current_font_attributes) {
                flush_glyph_run();
                dc.SetFont(GlyphFont(cell.attributes));
                current_font_attributes = font_attributes;
            }
            const wxString& glyph = GlyphText(cell.text);
            if (!glyph.empty() && cell.text != " " && cell.width == 1) {
                if (glyph_run.empty() || glyph_run_end != column ||
                    glyph_run_foreground != foreground_key ||
                    glyph_run_font_attributes != font_attributes) {
                    flush_glyph_run();
                    glyph_run_start = column;
                    glyph_run_foreground = foreground_key;
                    glyph_run_font_attributes = font_attributes;
                }
                glyph_run += glyph;
                glyph_run_end = column + 1;
            } else {
                flush_glyph_run();
                if (!glyph.empty() && cell.text != " ")
                    dc.DrawText(glyph, column * impl_->cell_width_,
                                row * impl_->cell_height_);
            }
            if ((cell.attributes & 0x04) != 0) {
                dc.SetPen(impl_->ColorResourcesFor(cell.foreground).pen);
                const int underline_y = (row + 1) * impl_->cell_height_ - 2;
                dc.DrawLine(column * impl_->cell_width_, underline_y,
                            (column + span) * impl_->cell_width_ - 1, underline_y);
                dc.SetPen(*wxTRANSPARENT_PEN);
            }
        }
        flush_glyph_run();
    }

    const bool cursor_in_dirty_region =
        snapshot.cursor_x >= left && snapshot.cursor_x < right &&
        snapshot.cursor_y >= top && snapshot.cursor_y < bottom;
    if (cursor_in_dirty_region && snapshot.cursor_visible &&
        snapshot.cursor_x < snapshot.columns && snapshot.cursor_y < snapshot.rows) {
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(wxColour(230, 230, 230), 1));
        const int cursor_x = snapshot.cursor_x * impl_->cell_width_;
        const int cursor_y = snapshot.cursor_y * impl_->cell_height_;
        switch (snapshot.cursor_style) {
        case TerminalCursorStyle::Underline:
            dc.DrawLine(cursor_x, cursor_y + impl_->cell_height_ - 2,
                        cursor_x + impl_->cell_width_ - 1,
                        cursor_y + impl_->cell_height_ - 2);
            break;
        case TerminalCursorStyle::Bar:
            dc.DrawLine(cursor_x + 1, cursor_y + 1,
                        cursor_x + 1, cursor_y + impl_->cell_height_ - 2);
            break;
        case TerminalCursorStyle::Block:
            dc.DrawRectangle(cursor_x, cursor_y, impl_->cell_width_, impl_->cell_height_);
            break;
        }
    }
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
        impl_->paint_metrics_.paint_time_us += elapsed_us;
        impl_->paint_metrics_.max_paint_time_us = std::max(
            impl_->paint_metrics_.max_paint_time_us, elapsed_us);
    };
    wxAutoBufferedPaintDC dc(this);
    const auto& background = impl_->config_.background;
    dc.SetBackground(wxBrush(wxColour(background[0], background[1],
                                       background[2])));

    if (!impl_->core_.IsReady()) {
        dc.Clear();
        const auto& error_foreground = impl_->config_.error_foreground;
        dc.SetTextForeground(wxColour(error_foreground[0], error_foreground[1],
                                      error_foreground[2]));
        dc.DrawText(impl_->error_, 12, 12);
        record_paint();
        return;
    }

    TerminalFrame frame;
    if (impl_->pending_frame_.snapshot != nullptr) {
        frame = std::move(impl_->pending_frame_);
        impl_->pending_frame_ = {};
    } else {
        frame = impl_->core_.TakeFrame();
    }
    if (frame.snapshot == nullptr) {
        record_paint();
        return;
    }
    const TerminalSnapshot& snapshot = *frame.snapshot;
    const wxSize client_size = GetClientSize();
    const int bitmap_width = std::max(1, client_size.GetWidth());
    const int bitmap_height = std::max(1, client_size.GetHeight());
    if (!impl_->framebuffer_.IsOk() ||
        impl_->framebuffer_.GetWidth() != bitmap_width ||
        impl_->framebuffer_.GetHeight() != bitmap_height) {
        impl_->framebuffer_ = wxBitmap(bitmap_width, bitmap_height, -1);
        impl_->framebuffer_valid_ = false;
        ++impl_->paint_metrics_.framebuffer_rebuilds;
    }

    wxMemoryDC framebuffer_dc;
    framebuffer_dc.SelectObject(impl_->framebuffer_);
    uint64_t painted_cells = 0;
    if (!impl_->framebuffer_valid_ || frame.full_repaint) {
        ++impl_->paint_metrics_.full_repaints;
        framebuffer_dc.SetBackground(wxBrush(wxColour(background[0],
                                                       background[1],
                                                       background[2])));
        framebuffer_dc.Clear();
        const TerminalDirtyRegion full_region {
            0, 0, snapshot.columns, snapshot.rows
        };
        RenderSnapshot(framebuffer_dc, snapshot, full_region, &painted_cells);
        impl_->framebuffer_valid_ = true;
    } else if (frame.changed) {
        ++impl_->paint_metrics_.partial_repaints;
        RenderSnapshot(framebuffer_dc, snapshot, frame.dirty, &painted_cells);
    }
    framebuffer_dc.SelectObject(wxNullBitmap);
    impl_->paint_metrics_.painted_cells += painted_cells;

    dc.DrawBitmap(impl_->framebuffer_, 0, 0, false);
    record_paint();
}

void WxTerminalCtrl::OnSize(wxSizeEvent& event)
{
    UpdateGeometry();
    ++impl_->paint_metrics_.refresh_requests;
    ++impl_->paint_metrics_.full_refresh_requests;
    Refresh(false);
    event.Skip();
}

void WxTerminalCtrl::OnChar(wxKeyEvent& event)
{
    const uint32_t key = static_cast<uint32_t>(event.GetKeyCode());
    const uint32_t unicode = event.GetUnicodeKey();
    const bool copy_key = key == 'c' || key == 'C' || unicode == 'c' ||
                          unicode == 'C' || key == 3;
    const bool paste_key = key == 'v' || key == 'V' || unicode == 'v' ||
                           unicode == 'V' || key == 22;
    const bool restart_key = key == 'r' || key == 'R' || unicode == 'r' ||
                             unicode == 'R' || key == 18;
    if ((event.ControlDown() || event.MetaDown()) && event.ShiftDown()) {
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
    if (event.ShiftDown()) modifiers |= TerminalShift;
    if (event.ControlDown()) modifiers |= TerminalControl;
    if (event.AltDown()) modifiers |= TerminalAlt;
    if (event.MetaDown()) modifiers |= TerminalLogo;

    const uint32_t ascii = unicode <= 0x7f ? unicode : TerminalInvalid;
    if (impl_->core_.HandleKey(KeySymFor(event), ascii, modifiers, unicode)) {
        RequestFrameRefresh();
        return;
    }
    event.Skip();
}

void WxTerminalCtrl::OnMouseWheel(wxMouseEvent& event)
{
    if (!event.ShiftDown() && impl_->core_.MouseReportingEnabled()) {
        const unsigned int button = event.GetWheelRotation() > 0
            ? TerminalMouseWheelUp
            : TerminalMouseWheelDown;
        const auto [column, row] = CellAt(event.GetPosition());
        impl_->core_.HandleMouse(column, row,
                          static_cast<unsigned int>(std::max(0, event.GetX())),
                          static_cast<unsigned int>(std::max(0, event.GetY())),
                          button, TerminalMousePressed,
                          MouseModifiers(event));
        return;
    }
    if (event.GetWheelRotation() > 0)
        impl_->core_.ScrollPageUp(3);
    else if (event.GetWheelRotation() < 0)
        impl_->core_.ScrollPageDown(3);
    RequestFrameRefresh();
}

std::pair<unsigned int, unsigned int>
WxTerminalCtrl::CellAt(const wxPoint& point) const
{
    const int column = std::max(0, point.x) / std::max(1, impl_->cell_width_);
    const int row = std::max(0, point.y) / std::max(1, impl_->cell_height_);
    return {
        std::min(static_cast<unsigned int>(column), impl_->columns_ - 1),
        std::min(static_cast<unsigned int>(row), impl_->rows_ - 1),
    };
}

unsigned char WxTerminalCtrl::MouseModifiers(const wxMouseEvent& event)
{
    unsigned char modifiers = 0;
    if (event.ShiftDown()) modifiers |= TerminalMouseShift;
    if (event.MetaDown()) modifiers |= TerminalMouseMeta;
    if (event.ControlDown()) modifiers |= TerminalMouseControl;
    return modifiers;
}

bool WxTerminalCtrl::ForwardMouse(const wxMouseEvent& event,
                                  unsigned int button,
                                  unsigned int mouse_event)
{
    if (event.ShiftDown() || !impl_->core_.MouseReportingEnabled())
        return false;
    const auto [column, row] = CellAt(event.GetPosition());
    return impl_->core_.HandleMouse(
        column, row,
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
    impl_->core_.HandleMouse(
        column, row,
        static_cast<unsigned int>(std::max(0, event.GetX())),
        static_cast<unsigned int>(std::max(0, event.GetY())),
        impl_->mouse_reporting_button_, TerminalMouseReleased,
        MouseModifiers(event));
    impl_->mouse_reporting_gesture_ = false;
    if (HasCapture())
        ReleaseMouse();
}

void WxTerminalCtrl::OnLeftDown(wxMouseEvent& event)
{
    SetFocus();
    const auto [column, row] = CellAt(event.GetPosition());
    if (ForwardMouse(event, TerminalMouseLeft,
                     TerminalMousePressed)) {
        BeginMouseReporting(TerminalMouseLeft);
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

    const bool extend_selection = event.ShiftDown() && impl_->core_.HasSelection();
    if (extend_selection) {
        impl_->core_.UpdateSelection(column, row);
    } else {
        impl_->core_.ClearSelection();
        if (impl_->click_count_ == 2)
            impl_->core_.SelectWord(column, row);
        else if (impl_->click_count_ == 3)
            impl_->core_.SelectLine(row);
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
        impl_->core_.UpdateSelection(column, row);
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
        ? TerminalMouseRight
        : TerminalMouseMiddle;
    if (ForwardMouse(event, button, TerminalMousePressed))
        BeginMouseReporting(button);
}

void WxTerminalCtrl::OnMouseButtonUp(wxMouseEvent& event)
{
    if (impl_->mouse_reporting_gesture_)
        EndMouseReporting(event);
}

void WxTerminalCtrl::UpdateSelectionAt(const wxPoint& position)
{
    impl_->auto_scroll_direction_ = 0;
    if (position.y < 0)
        impl_->auto_scroll_direction_ = -1;
    else if (position.y >= GetClientSize().GetHeight())
        impl_->auto_scroll_direction_ = 1;

    if (impl_->auto_scroll_direction_ != 0)
        impl_->core_.ScrollLines(impl_->auto_scroll_direction_ < 0 ? 1 : -1);

    const auto [column, row] = CellAt(position);
    impl_->core_.UpdateSelection(column, row);
}

void WxTerminalCtrl::OnMotion(wxMouseEvent& event)
{
    if (impl_->mouse_reporting_gesture_) {
        if (event.Dragging()) {
            const auto [column, row] = CellAt(event.GetPosition());
            impl_->core_.HandleMouse(
                column, row,
                static_cast<unsigned int>(std::max(0, event.GetX())),
                static_cast<unsigned int>(std::max(0, event.GetY())),
                32 + impl_->mouse_reporting_button_, TerminalMouseMoved,
                MouseModifiers(event));
        }
        return;
    }
    if (!impl_->pointer_down_ || !event.Dragging()) {
        if (!event.ShiftDown() && impl_->core_.MouseReportingEnabled()) {
            const auto [column, row] = CellAt(event.GetPosition());
            impl_->core_.HandleMouse(
                column, row,
                static_cast<unsigned int>(std::max(0, event.GetX())),
                static_cast<unsigned int>(std::max(0, event.GetY())),
                TerminalMouseLeft, TerminalMouseMoved,
                MouseModifiers(event));
        }
        return;
    }
    const wxPoint position = event.GetPosition();
    const int distance_x = std::abs(position.x - impl_->pointer_down_position_.x);
    const int distance_y = std::abs(position.y - impl_->pointer_down_position_.y);
    if (!impl_->selection_dragging_ && (distance_x >= 4 || distance_y >= 4)) {
        impl_->core_.StartSelection(impl_->selection_anchor_column_, impl_->selection_anchor_row_);
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
    const std::string text = impl_->core_.CopySelection();
    if (text.empty() || wxTheClipboard == nullptr || !wxTheClipboard->Open())
        return false;
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
    impl_->core_.Paste(std::string(utf8.data(), utf8.length()));
    RequestFrameRefresh();
    return true;
}

void WxTerminalCtrl::NotifyTitleChanged()
{
    impl_->AssertOwnerThread();
    const std::string title = impl_->core_.Title();
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
    impl_->core_.FeedOutput(notice.data(), notice.size());
}

void WxTerminalCtrl::RestartTransport()
{
    if (impl_->transport_ == nullptr)
        return;
    impl_->transport_->Stop();
    impl_->core_.ClearSelection();
    const char* shell = std::getenv("SHELL");
    impl_->transport_->Start(impl_->columns_, impl_->rows_, shell == nullptr ? "" : shell);
    const char* clear = "\x1b[2J\x1b[H";
    impl_->core_.FeedOutput(clear, std::strlen(clear));
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
    if (impl_->selection_dragging_ && impl_->auto_scroll_direction_ != 0) {
        impl_->core_.ScrollLines(impl_->auto_scroll_direction_ < 0 ? 1 : -1);
        const auto [column, row] = CellAt(impl_->last_pointer_position_);
        impl_->core_.UpdateSelection(column, row);
        RequestFrameRefresh();
    }
    const auto output = impl_->transport_ != nullptr
        ? impl_->transport_->TakeOutput() : std::vector<std::string> {};
    for (const auto& chunk : output)
        impl_->core_.FeedOutput(chunk.data(), chunk.size());
    if (!output.empty())
        RequestFrameRefresh();
    NotifyTitleChanged();
    UpdateTransportStatus();

    const auto now = std::chrono::steady_clock::now();
    if (impl_->callbacks_.on_metrics && impl_->config_.metrics_interval_ms > 0 &&
        now - impl_->last_metrics_callback_ >= std::chrono::milliseconds(
            impl_->config_.metrics_interval_ms)) {
        const TerminalMetrics core_metrics = impl_->core_.GetMetrics();
        const TransportMetrics transport_metrics = impl_->transport_ != nullptr
            ? impl_->transport_->GetMetrics() : TransportMetrics {};
        impl_->callbacks_.on_metrics(core_metrics, transport_metrics);
        impl_->last_metrics_callback_ = now;
    }

    if (impl_->trace_metrics_) {
        if (now - impl_->last_metrics_log_ >= std::chrono::seconds(1)) {
            const TerminalMetrics core_metrics = impl_->core_.GetMetrics();
            const TransportMetrics transport_metrics = impl_->transport_ != nullptr
                ? impl_->transport_->GetMetrics() : TransportMetrics {};
            std::fprintf(stderr,
                         "[metrics] output=%lluB/%llu chunks input=%llu "
                         "writes=%lluB/%llu renders=%llu selection=%llu "
                         "latency-max=%lluus/%llu paste=%lluB "
                         "mouse=%llu/%llu modes=%llu "
                         "paint=%llu full/%llu partial cells=%llu "
                         "paint-max=%lluus refresh=%llu/%llu dirty "
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
                         static_cast<unsigned long long>(impl_->paint_metrics_.max_paint_time_us),
                         static_cast<unsigned long long>(impl_->paint_metrics_.refresh_requests),
                         static_cast<unsigned long long>(impl_->paint_metrics_.dirty_refresh_requests),
                         static_cast<unsigned long long>(transport_metrics.bytes_read),
                         static_cast<unsigned long long>(transport_metrics.read_events),
                         static_cast<unsigned long long>(transport_metrics.max_queued_bytes),
                         static_cast<unsigned long long>(transport_metrics.resize_events));
            impl_->last_metrics_log_ = now;
        }
    }
}
