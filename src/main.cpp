#include "pty_session.h"

#include <wx/dcbuffer.h>
#include <wx/font.h>
#include <wx/wx.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <tsm/libtsm.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

class TerminalPanel final : public wxPanel {
public:
    explicit TerminalPanel(wxWindow* parent)
        : wxPanel(parent), output_timer_(this)
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

        if (tsm_screen_new(&screen_, nullptr, nullptr) != 0) {
            error_ = "libtsm could not create a screen";
            return;
        }
        tsm_screen_set_max_sb(screen_, 2000);

        if (tsm_vte_new(&vte_, screen_, &TerminalPanel::VteWrite, this,
                        nullptr, nullptr) != 0) {
            error_ = "libtsm could not create a VTE";
            return;
        }
        tsm_vte_set_palette(vte_, "base16-dark");

        UpdateGeometry();
        const char* shell = std::getenv("SHELL");
        if (!pty_.Start(columns_, rows_, shell == nullptr ? "" : shell)) {
            error_ = "No POSIX PTY backend is available on this platform";
            const char* notice =
                "\x1b[1;33mSakura wx prototype\x1b[0m\r\n"
                "This build has libtsm and wxWidgets, but its first process "
                "backend is POSIX-only.\r\n"
                "Windows ConPTY support is the next platform milestone.\r\n";
            tsm_vte_input(vte_, notice, std::strlen(notice));
        }
        output_timer_.Start(16);
    }

    ~TerminalPanel() override
    {
        output_timer_.Stop();
        pty_.Stop();
        if (vte_ != nullptr)
            tsm_vte_unref(vte_);
        if (screen_ != nullptr)
            tsm_screen_unref(screen_);
    }

private:
    static void VteWrite(struct tsm_vte*, const char* data, std::size_t length, void* user_data)
    {
        auto* self = static_cast<TerminalPanel*>(user_data);
        self->pty_.Write(data, length);
    }

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
        if (screen_ != nullptr)
            tsm_screen_resize(screen_, columns_, rows_);
        pty_.Resize(columns_, rows_);
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

        if (screen_ == nullptr) {
            dc.SetTextForeground(wxColour(240, 180, 90));
            dc.DrawText(error_, 12, 12);
            return;
        }

        const unsigned int columns = tsm_screen_get_width(screen_);
        const unsigned int rows = tsm_screen_get_height(screen_);
        const struct tsm_screen_cell* cells = tsm_screen_draw2(screen_);
        dc.SetPen(*wxTRANSPARENT_PEN);

        for (unsigned int row = 0; row < rows; ++row) {
            for (unsigned int column = 0; column < columns; ++column) {
                const auto& cell = cells[row * columns + column];
                dc.SetBrush(wxBrush(wxColour(cell.bg.r, cell.bg.g, cell.bg.b)));
                dc.DrawRectangle(column * cell_width_, row * cell_height_,
                                 cell_width_, cell_height_);

                if (cell.ch == 0)
                    continue;
                dc.SetTextForeground(wxColour(cell.fg.r, cell.fg.g, cell.fg.b));
                const wxString glyph = CellText(cell.ch);
                if (!glyph.empty())
                    dc.DrawText(glyph, column * cell_width_, row * cell_height_);
            }
        }

        const unsigned int cursor_x = tsm_screen_get_cursor_x(screen_);
        const unsigned int cursor_y = tsm_screen_get_cursor_y(screen_);
        if (vte_ != nullptr &&
            (tsm_vte_get_flags(vte_) & TSM_VTE_FLAG_TEXT_CURSOR_MODE) != 0 &&
            cursor_x < columns && cursor_y < rows) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(wxColour(230, 230, 230), 1));
            dc.DrawRectangle(cursor_x * cell_width_, cursor_y * cell_height_,
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
        if (vte_ == nullptr) {
            event.Skip();
            return;
        }

        unsigned int modifiers = 0;
        if (event.ShiftDown()) modifiers |= TSM_SHIFT_MASK;
        if (event.ControlDown()) modifiers |= TSM_CONTROL_MASK;
        if (event.AltDown()) modifiers |= TSM_ALT_MASK;
        if (event.MetaDown()) modifiers |= TSM_LOGO_MASK;

        const uint32_t unicode = event.GetUnicodeKey();
        const uint32_t ascii = unicode <= 0x7f ? unicode : TSM_VTE_INVALID;
        if (tsm_vte_handle_keyboard(vte_, KeySymFor(event), ascii, modifiers, unicode)) {
            Refresh(false);
            return;
        }
        event.Skip();
    }

    void OnMouseWheel(wxMouseEvent& event)
    {
        if (screen_ != nullptr) {
            const unsigned int pages = 3;
            if (event.GetWheelRotation() > 0)
                tsm_screen_sb_page_up(screen_, pages);
            else if (event.GetWheelRotation() < 0)
                tsm_screen_sb_page_down(screen_, pages);
            Refresh(false);
        }
    }

    void OnTimer(wxTimerEvent&)
    {
        if (vte_ == nullptr)
            return;
        const auto output = pty_.TakeOutput();
        for (const auto& chunk : output)
            tsm_vte_input(vte_, chunk.data(), chunk.size());
        if (!output.empty())
            Refresh(false);
    }

    wxFont font_;
    wxTimer output_timer_;
    PtySession pty_;
    struct tsm_screen* screen_ = nullptr;
    struct tsm_vte* vte_ = nullptr;
    wxString error_;
    int cell_width_ = 8;
    int cell_height_ = 16;
    unsigned int columns_ = 80;
    unsigned int rows_ = 24;
};

class SakuraWxApp final : public wxApp {
public:
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
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(SakuraWxApp);
