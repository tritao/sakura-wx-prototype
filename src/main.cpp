#include "pty_session.h"
#include "terminal_core.h"

#include <wx/dcbuffer.h>
#include <wx/font.h>
#include <wx/wx.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <tsm/libtsm.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

class TerminalPanel final : public wxPanel {
public:
    explicit TerminalPanel(wxWindow* parent)
        : wxPanel(parent),
          output_timer_(this),
          transport_(std::make_unique<PosixPtySession>()),
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
            error_ = "No POSIX PTY backend is available on this platform";
            const char* notice =
                "\x1b[1;33mSakura wx prototype\x1b[0m\r\n"
                "This build has libtsm and wxWidgets, but its first process "
                "backend is POSIX-only.\r\n"
                "Windows ConPTY support is the next platform milestone.\r\n";
            core_.FeedOutput(notice, std::strlen(notice));
        }
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

    static wxString CellText(uint32_t codepoint)
    {
        char utf8[8] {};
        const std::size_t length = tsm_ucs4_to_utf8(codepoint, utf8);
        if (length == 0)
            return {};
        return wxString::FromUTF8(utf8, length);
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
                dc.SetBrush(wxBrush(wxColour(cell.background[0], cell.background[1],
                                             cell.background[2])));
                dc.DrawRectangle(column * cell_width_, row * cell_height_,
                                 cell_width_, cell_height_);

                if (cell.codepoint == 0)
                    continue;
                dc.SetTextForeground(wxColour(cell.foreground[0], cell.foreground[1],
                                              cell.foreground[2]));
                const wxString glyph = CellText(cell.codepoint);
                if (!glyph.empty())
                    dc.DrawText(glyph, column * cell_width_, row * cell_height_);
            }
        }

        if (snapshot.cursor_visible && snapshot.cursor_x < snapshot.columns &&
            snapshot.cursor_y < snapshot.rows) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(wxColour(230, 230, 230), 1));
            dc.DrawRectangle(snapshot.cursor_x * cell_width_,
                             snapshot.cursor_y * cell_height_,
                             cell_width_, cell_height_);
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
        unsigned int modifiers = 0;
        if (event.ShiftDown()) modifiers |= TSM_SHIFT_MASK;
        if (event.ControlDown()) modifiers |= TSM_CONTROL_MASK;
        if (event.AltDown()) modifiers |= TSM_ALT_MASK;
        if (event.MetaDown()) modifiers |= TSM_LOGO_MASK;

        const uint32_t unicode = event.GetUnicodeKey();
        const uint32_t ascii = unicode <= 0x7f ? unicode : TSM_VTE_INVALID;
        if (core_.HandleKey(KeySymFor(event), ascii, modifiers, unicode)) {
            Refresh(false);
            return;
        }
        event.Skip();
    }

    void OnMouseWheel(wxMouseEvent& event)
    {
        if (event.GetWheelRotation() > 0)
            core_.ScrollPageUp(3);
        else if (event.GetWheelRotation() < 0)
            core_.ScrollPageDown(3);
        Refresh(false);
    }

    void OnTimer(wxTimerEvent&)
    {
        const auto output = transport_->TakeOutput();
        for (const auto& chunk : output)
            core_.FeedOutput(chunk.data(), chunk.size());
        if (!output.empty())
            Refresh(false);
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
