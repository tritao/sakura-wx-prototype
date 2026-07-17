#include "terminal_core.h"
#include "transport_factory.h"

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/font.h>
#include <wx/wx.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include <tsm/libtsm.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

class TerminalPanel final : public wxPanel {
public:
    explicit TerminalPanel(wxWindow* parent)
        : wxPanel(parent),
          output_timer_(this),
          transport_(CreateTerminalTransport()),
          core_([this](const char* data, std::size_t length) {
              transport_->Write(data, length);
          })
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFocus();

        Bind(wxEVT_PAINT, &TerminalPanel::OnPaint, this);
        Bind(wxEVT_SIZE, &TerminalPanel::OnSize, this);
        Bind(wxEVT_CHAR, &TerminalPanel::OnChar, this);
        Bind(wxEVT_MOUSEWHEEL, &TerminalPanel::OnMouseWheel, this);
        Bind(wxEVT_LEFT_DOWN, &TerminalPanel::OnLeftDown, this);
        Bind(wxEVT_LEFT_DCLICK, &TerminalPanel::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP, &TerminalPanel::OnLeftUp, this);
        Bind(wxEVT_RIGHT_DOWN, &TerminalPanel::OnMouseButtonDown, this);
        Bind(wxEVT_RIGHT_UP, &TerminalPanel::OnMouseButtonUp, this);
        Bind(wxEVT_MIDDLE_DOWN, &TerminalPanel::OnMouseButtonDown, this);
        Bind(wxEVT_MIDDLE_UP, &TerminalPanel::OnMouseButtonUp, this);
        Bind(wxEVT_MOTION, &TerminalPanel::OnMotion, this);
        Bind(wxEVT_TIMER, &TerminalPanel::OnTimer, this);

        font_ = wxFont(12, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                       wxFONTWEIGHT_NORMAL, false, "DejaVu Sans Mono");

        if (!core_.IsReady()) {
            error_ = wxString::FromUTF8(core_.Error().c_str());
            return;
        }

        UpdateGeometry();
        const char* shell = std::getenv("SHELL");
        if (!transport_->Start(columns_, rows_, shell == nullptr ? "" : shell)) {
            error_ = "No platform terminal transport is available";
            const char* notice =
                "\x1b[1;33mSakura wx prototype\x1b[0m\r\n"
                "This build has libtsm and wxWidgets, but could not start "
                "the platform process backend.\r\n";
            core_.FeedOutput(notice, std::strlen(notice));
        }
        const TransportStatus initial_status = transport_->GetStatus();
        last_transport_state_ = initial_status.state == TransportState::Failed
            ? TransportState::Failed
            : TransportState::Stopped;
        UpdateTransportTitle(initial_status);
        trace_metrics_ = std::getenv("SAKURA_TRACE_METRICS") != nullptr;
        last_metrics_log_ = std::chrono::steady_clock::now();
        output_timer_.Start(16);
    }

    ~TerminalPanel() override
    {
        output_timer_.Stop();
        transport_->Stop();
    }

private:
    void UpdateGeometry()
    {
        wxClientDC dc(this);
        dc.SetFont(font_);
        dc.GetTextExtent("M", &cell_width_, &cell_height_);
        cell_width_ = std::max(cell_width_, 1);
        cell_height_ = std::max(cell_height_, 1);

        const wxSize size = GetClientSize();
        const unsigned int columns = static_cast<unsigned int>(
            std::max(1, size.GetWidth() / cell_width_));
        const unsigned int rows = static_cast<unsigned int>(
            std::max(1, size.GetHeight() / cell_height_));

        if (columns == columns_ && rows == rows_)
            return;
        columns_ = columns;
        rows_ = rows;
        core_.Resize(columns_, rows_);
        transport_->Resize(columns_, rows_);
    }

    static uint32_t KeySymFor(const wxKeyEvent& event)
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

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(wxColour(16, 18, 20)));
        dc.Clear();
        dc.SetFont(font_);

        if (!core_.IsReady()) {
            dc.SetTextForeground(wxColour(240, 180, 90));
            dc.DrawText(error_, 12, 12);
            return;
        }

        const TerminalSnapshot snapshot = core_.TakeSnapshot();
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (unsigned int row = 0; row < snapshot.rows; ++row) {
            for (unsigned int column = 0; column < snapshot.columns; ++column) {
                const auto& cell = snapshot.cells[row * snapshot.columns + column];
                if (cell.width == 0)
                    continue;
                const unsigned int span = std::min(
                    std::max(1u, cell.width), snapshot.columns - column);
                dc.SetBrush(wxBrush(wxColour(cell.background[0], cell.background[1],
                                             cell.background[2])));
                dc.DrawRectangle(column * cell_width_, row * cell_height_,
                                 span * cell_width_, cell_height_);

                dc.SetTextForeground(wxColour(cell.foreground[0], cell.foreground[1],
                                              cell.foreground[2]));
                wxFont glyph_font = font_;
                glyph_font.SetWeight((cell.attributes & 0x01) != 0
                    ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
                glyph_font.SetStyle((cell.attributes & 0x02) != 0
                    ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);
                glyph_font.SetUnderlined((cell.attributes & 0x04) != 0);
                dc.SetFont(glyph_font);
                const wxString glyph = wxString::FromUTF8(cell.text);
                if (!glyph.empty() && cell.text != " ")
                    dc.DrawText(glyph, column * cell_width_, row * cell_height_);
                if ((cell.attributes & 0x04) != 0) {
                    dc.SetPen(wxPen(wxColour(cell.foreground[0], cell.foreground[1],
                                             cell.foreground[2]), 1));
                    const int underline_y = (row + 1) * cell_height_ - 2;
                    dc.DrawLine(column * cell_width_, underline_y,
                                (column + span) * cell_width_ - 1, underline_y);
                    dc.SetPen(*wxTRANSPARENT_PEN);
                }
            }
        }

        if (snapshot.cursor_visible && snapshot.cursor_x < snapshot.columns &&
            snapshot.cursor_y < snapshot.rows) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(wxColour(230, 230, 230), 1));
            const int cursor_x = snapshot.cursor_x * cell_width_;
            const int cursor_y = snapshot.cursor_y * cell_height_;
            switch (snapshot.cursor_style) {
            case TerminalCursorStyle::Underline:
                dc.DrawLine(cursor_x, cursor_y + cell_height_ - 2,
                            cursor_x + cell_width_ - 1,
                            cursor_y + cell_height_ - 2);
                break;
            case TerminalCursorStyle::Bar:
                dc.DrawLine(cursor_x + 1, cursor_y + 1,
                            cursor_x + 1, cursor_y + cell_height_ - 2);
                break;
            case TerminalCursorStyle::Block:
                dc.DrawRectangle(cursor_x, cursor_y, cell_width_, cell_height_);
                break;
            }
        }
    }

    void OnSize(wxSizeEvent& event)
    {
        UpdateGeometry();
        Refresh(false);
        event.Skip();
    }

    void OnChar(wxKeyEvent& event)
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
        if (event.ShiftDown()) modifiers |= TSM_SHIFT_MASK;
        if (event.ControlDown()) modifiers |= TSM_CONTROL_MASK;
        if (event.AltDown()) modifiers |= TSM_ALT_MASK;
        if (event.MetaDown()) modifiers |= TSM_LOGO_MASK;

        const uint32_t ascii = unicode <= 0x7f ? unicode : TSM_VTE_INVALID;
        if (core_.HandleKey(KeySymFor(event), ascii, modifiers, unicode)) {
            Refresh(false);
            return;
        }
        event.Skip();
    }

    void OnMouseWheel(wxMouseEvent& event)
    {
        if (!event.ShiftDown() && core_.MouseReportingEnabled()) {
            const unsigned int button = event.GetWheelRotation() > 0
                ? TSM_MOUSE_BUTTON_WHEEL_UP
                : TSM_MOUSE_BUTTON_WHEEL_DOWN;
            core_.HandleMouse(CellAt(event.GetPosition()).first,
                              CellAt(event.GetPosition()).second,
                              static_cast<unsigned int>(std::max(0, event.GetX())),
                              static_cast<unsigned int>(std::max(0, event.GetY())),
                              button, TSM_MOUSE_EVENT_PRESSED,
                              MouseModifiers(event));
            return;
        }
        if (event.GetWheelRotation() > 0)
            core_.ScrollPageUp(3);
        else if (event.GetWheelRotation() < 0)
            core_.ScrollPageDown(3);
        Refresh(false);
    }

    std::pair<unsigned int, unsigned int> CellAt(const wxPoint& point) const
    {
        const int column = std::max(0, point.x) / std::max(1, cell_width_);
        const int row = std::max(0, point.y) / std::max(1, cell_height_);
        return {
            std::min(static_cast<unsigned int>(column), columns_ - 1),
            std::min(static_cast<unsigned int>(row), rows_ - 1),
        };
    }

    static unsigned char MouseModifiers(const wxMouseEvent& event)
    {
        unsigned char modifiers = 0;
        if (event.ShiftDown()) modifiers |= TSM_MOUSE_MODIFIER_SHIFT;
        if (event.MetaDown()) modifiers |= TSM_MOUSE_MODIFIER_META;
        if (event.ControlDown()) modifiers |= TSM_MOUSE_MODIFIER_CTRL;
        return modifiers;
    }

    bool ForwardMouse(const wxMouseEvent& event, unsigned int button,
                      unsigned int mouse_event)
    {
        if (event.ShiftDown() || !core_.MouseReportingEnabled())
            return false;
        const auto [column, row] = CellAt(event.GetPosition());
        return core_.HandleMouse(
            column, row,
            static_cast<unsigned int>(std::max(0, event.GetX())),
            static_cast<unsigned int>(std::max(0, event.GetY())),
            button, mouse_event, MouseModifiers(event));
    }

    void BeginMouseReporting(unsigned int button)
    {
        mouse_reporting_gesture_ = true;
        mouse_reporting_button_ = button;
        CaptureMouse();
    }

    void EndMouseReporting(const wxMouseEvent& event)
    {
        if (!mouse_reporting_gesture_)
            return;
        const auto [column, row] = CellAt(event.GetPosition());
        core_.HandleMouse(
            column, row,
            static_cast<unsigned int>(std::max(0, event.GetX())),
            static_cast<unsigned int>(std::max(0, event.GetY())),
            mouse_reporting_button_, TSM_MOUSE_EVENT_RELEASED,
            MouseModifiers(event));
        mouse_reporting_gesture_ = false;
        if (HasCapture())
            ReleaseMouse();
    }

    void OnLeftDown(wxMouseEvent& event)
    {
        SetFocus();
        const auto [column, row] = CellAt(event.GetPosition());
        if (ForwardMouse(event, TSM_MOUSE_BUTTON_LEFT,
                         TSM_MOUSE_EVENT_PRESSED)) {
            BeginMouseReporting(TSM_MOUSE_BUTTON_LEFT);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool same_cell = column == last_click_column_ &&
                               row == last_click_row_;
        const bool quick_click = now - last_click_time_ <
                                 std::chrono::milliseconds(500);
        if (event.GetClickCount() >= 2 || (same_cell && quick_click))
            click_count_ = click_count_ % 3 + 1;
        else
            click_count_ = 1;
        last_click_time_ = now;
        last_click_column_ = column;
        last_click_row_ = row;

        const bool extend_selection = event.ShiftDown() && core_.HasSelection();
        if (extend_selection) {
            core_.UpdateSelection(column, row);
        } else {
            core_.ClearSelection();
            if (click_count_ == 2)
                core_.SelectWord(column, row);
            else if (click_count_ == 3)
                core_.SelectLine(row);
        }

        pointer_down_ = true;
        selection_dragging_ = extend_selection;
        pointer_down_position_ = event.GetPosition();
        selection_anchor_column_ = column;
        selection_anchor_row_ = row;
        CaptureMouse();
        Refresh(false);
    }

    void OnLeftUp(wxMouseEvent& event)
    {
        if (mouse_reporting_gesture_) {
            EndMouseReporting(event);
            return;
        }
        if (!pointer_down_)
            return;
        const auto [column, row] = CellAt(event.GetPosition());
        const bool should_copy = selection_dragging_ || click_count_ >= 2;
        if (selection_dragging_)
            core_.UpdateSelection(column, row);
        selection_dragging_ = false;
        pointer_down_ = false;
        auto_scroll_direction_ = 0;
        if (HasCapture())
            ReleaseMouse();
        if (should_copy)
            CopySelectionToClipboard();
        Refresh(false);
    }

    void OnMouseButtonDown(wxMouseEvent& event)
    {
        const unsigned int button = event.RightDown()
            ? TSM_MOUSE_BUTTON_RIGHT
            : TSM_MOUSE_BUTTON_MIDDLE;
        if (ForwardMouse(event, button, TSM_MOUSE_EVENT_PRESSED))
            BeginMouseReporting(button);
    }

    void OnMouseButtonUp(wxMouseEvent& event)
    {
        if (mouse_reporting_gesture_)
            EndMouseReporting(event);
    }

    void UpdateSelectionAt(const wxPoint& position)
    {
        auto_scroll_direction_ = 0;
        if (position.y < 0)
            auto_scroll_direction_ = -1;
        else if (position.y >= GetClientSize().GetHeight())
            auto_scroll_direction_ = 1;

        if (auto_scroll_direction_ != 0)
            core_.ScrollLines(auto_scroll_direction_ < 0 ? 1 : -1);

        const auto [column, row] = CellAt(position);
        core_.UpdateSelection(column, row);
    }

    void OnMotion(wxMouseEvent& event)
    {
        if (mouse_reporting_gesture_) {
            if (event.Dragging()) {
                const auto [column, row] = CellAt(event.GetPosition());
                core_.HandleMouse(
                    column, row,
                    static_cast<unsigned int>(std::max(0, event.GetX())),
                    static_cast<unsigned int>(std::max(0, event.GetY())),
                    32 + mouse_reporting_button_, TSM_MOUSE_EVENT_MOVED,
                    MouseModifiers(event));
            }
            return;
        }
        if (!pointer_down_ || !event.Dragging()) {
            if (!event.ShiftDown() && core_.MouseReportingEnabled()) {
                const auto [column, row] = CellAt(event.GetPosition());
                core_.HandleMouse(
                    column, row,
                    static_cast<unsigned int>(std::max(0, event.GetX())),
                    static_cast<unsigned int>(std::max(0, event.GetY())),
                    TSM_MOUSE_BUTTON_LEFT, TSM_MOUSE_EVENT_MOVED,
                    MouseModifiers(event));
            }
            return;
        }
        const wxPoint position = event.GetPosition();
        const int distance_x = std::abs(position.x - pointer_down_position_.x);
        const int distance_y = std::abs(position.y - pointer_down_position_.y);
        if (!selection_dragging_ && (distance_x >= 4 || distance_y >= 4)) {
            core_.StartSelection(selection_anchor_column_, selection_anchor_row_);
            selection_dragging_ = true;
        }
        if (!selection_dragging_)
            return;
        last_pointer_position_ = position;
        UpdateSelectionAt(position);
        Refresh(false);
    }

    bool CopySelectionToClipboard()
    {
        const std::string text = core_.CopySelection();
        if (text.empty() || wxTheClipboard == nullptr || !wxTheClipboard->Open())
            return false;
        const bool copied = wxTheClipboard->SetData(
            new wxTextDataObject(wxString::FromUTF8(text)));
        wxTheClipboard->Close();
        return copied;
    }

    bool PasteFromClipboard()
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
        core_.Paste(std::string(utf8.data(), utf8.length()));
        Refresh(false);
        return true;
    }

    wxString TransportTitle(const TransportStatus& status) const
    {
        wxString base = "Sakura wx prototype — libtsm";
        if (!core_.Title().empty()) {
            wxString terminal_title = wxString::FromUTF8(core_.Title());
            terminal_title.Replace("\r", " ");
            terminal_title.Replace("\n", " ");
            if (terminal_title.length() > 120)
                terminal_title = terminal_title.Left(117) + "...";
            if (!terminal_title.empty())
                base += " — " + terminal_title;
        }
        switch (status.state) {
        case TransportState::Running:
            return base;
        case TransportState::Exited:
            if (status.exit_code_valid && status.signal == 0)
                return base + wxString::Format(" [process exited %d]", status.exit_code);
            if (status.exit_code_valid && status.signal != 0)
                return base + wxString::Format(" [process terminated by signal %d]",
                                               status.signal);
            return base + " [process exited]";
        case TransportState::Failed:
            return base + " [transport failed]";
        case TransportState::Starting:
            return base + " [starting]";
        case TransportState::Stopped:
            return base + " [stopped]";
        }
        return base;
    }

    void UpdateTransportTitle(const TransportStatus& status)
    {
        auto* frame = wxDynamicCast(GetParent(), wxFrame);
        if (frame != nullptr)
            frame->SetTitle(TransportTitle(status));
    }

    void ShowTransportNotice(const TransportStatus& status)
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
        core_.FeedOutput(notice.data(), notice.size());
    }

    void RestartTransport()
    {
        transport_->Stop();
        core_.ClearSelection();
        const char* shell = std::getenv("SHELL");
        transport_->Start(columns_, rows_, shell == nullptr ? "" : shell);
        const char* clear = "\x1b[2J\x1b[H";
        core_.FeedOutput(clear, std::strlen(clear));
        const TransportStatus status = transport_->GetStatus();
        ShowTransportNotice(status);
        last_transport_state_ = status.state == TransportState::Failed
            ? TransportState::Failed
            : TransportState::Stopped;
        UpdateTransportTitle(status);
        Refresh(false);
    }

    void UpdateTransportStatus()
    {
        const TransportStatus status = transport_->GetStatus();
        UpdateTransportTitle(status);
        if (status.state == last_transport_state_)
            return;
        last_transport_state_ = status.state;
        if (status.state == TransportState::Exited ||
            status.state == TransportState::Failed)
            ShowTransportNotice(status);
        Refresh(false);
    }

public:
    bool RunScenario()
    {
        if (!core_.IsReady())
            return false;

        const char* text = "\x1b[2J\x1b[Hscenario";
        core_.FeedOutput(text, std::strlen(text));
        wxMouseEvent down(wxEVT_LEFT_DOWN);
        down.SetPosition(wxPoint(1, 1));
        OnLeftDown(down);
        wxMouseEvent up(wxEVT_LEFT_UP);
        up.SetPosition(wxPoint(7 * cell_width_ + 1, 1));
        wxMouseEvent motion(wxEVT_MOTION);
        motion.SetLeftDown(true);
        motion.SetPosition(up.GetPosition());
        OnMotion(motion);
        OnLeftUp(up);
        if (core_.CopySelection().find("scenario") == std::string::npos)
            return false;

        wxMouseEvent shift_down(wxEVT_LEFT_DOWN);
        shift_down.SetShiftDown(true);
        shift_down.SetPosition(wxPoint(9 * cell_width_ + 1, 1));
        OnLeftDown(shift_down);
        wxMouseEvent shift_up(wxEVT_LEFT_UP);
        shift_up.SetPosition(shift_down.GetPosition());
        OnLeftUp(shift_up);
        if (core_.CopySelection().find("scenario") == std::string::npos)
            return false;
        if (!CopySelectionToClipboard())
            return false;
        if (!PasteFromClipboard())
            return false;

        const TerminalMetrics metrics = core_.GetMetrics();
        return metrics.selection_copies >= 2 && metrics.paste_bytes >= 8;
    }

private:

    void OnTimer(wxTimerEvent&)
    {
        if (selection_dragging_ && auto_scroll_direction_ != 0) {
            core_.ScrollLines(auto_scroll_direction_ < 0 ? 1 : -1);
            const auto [column, row] = CellAt(last_pointer_position_);
            core_.UpdateSelection(column, row);
            Refresh(false);
        }
        const auto output = transport_->TakeOutput();
        for (const auto& chunk : output)
            core_.FeedOutput(chunk.data(), chunk.size());
        if (!output.empty())
            Refresh(false);
        UpdateTransportStatus();

        if (trace_metrics_) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_metrics_log_ >= std::chrono::seconds(1)) {
                const TerminalMetrics core_metrics = core_.GetMetrics();
                const TransportMetrics transport_metrics = transport_->GetMetrics();
                std::fprintf(stderr,
                             "[metrics] output=%lluB/%llu chunks input=%llu "
                             "writes=%lluB/%llu renders=%llu selection=%llu "
                             "latency-max=%lluus/%llu paste=%lluB "
                             "mouse=%llu/%llu modes=%llu "
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
                             static_cast<unsigned long long>(transport_metrics.bytes_read),
                             static_cast<unsigned long long>(transport_metrics.read_events),
                             static_cast<unsigned long long>(transport_metrics.max_queued_bytes),
                             static_cast<unsigned long long>(transport_metrics.resize_events));
                last_metrics_log_ = now;
            }
        }
    }

    wxFont font_;
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
    unsigned int mouse_reporting_button_ = TSM_MOUSE_BUTTON_LEFT;
    int auto_scroll_direction_ = 0;
    wxPoint last_pointer_position_;
    std::chrono::steady_clock::time_point last_click_time_;
    TransportState last_transport_state_ = TransportState::Stopped;
    std::chrono::steady_clock::time_point last_metrics_log_;
};

class SakuraWxApp final : public wxApp {
public:
    SakuraWxApp()
        : smoke_timer_(this)
    {
        Bind(wxEVT_TIMER, &SakuraWxApp::OnSmokeTimer, this, smoke_timer_.GetId());
    }

    bool OnInit() override
    {
        auto* frame = new wxFrame(nullptr, wxID_ANY, "Sakura wx prototype — libtsm",
                                  wxDefaultPosition, wxSize(960, 540));
        auto* panel = new TerminalPanel(frame);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(panel, 1, wxEXPAND);
        frame->SetSizer(sizer);
        frame->Show();
        panel->SetFocus();

        if (std::getenv("SAKURA_WX_SCENARIO_TEST") != nullptr &&
            !panel->RunScenario()) {
            return false;
        }
        if (std::getenv("SAKURA_WX_SMOKE_TEST") != nullptr)
            smoke_timer_.StartOnce(300);
        return true;
    }

private:
    void OnSmokeTimer(wxTimerEvent&)
    {
        ExitMainLoop();
    }

    wxTimer smoke_timer_;
};

} // namespace

wxIMPLEMENT_APP(SakuraWxApp);
