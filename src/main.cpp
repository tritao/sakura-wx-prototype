#include <sakura/terminal/factory.h>

#include <sakura/wx/terminal_ctrl.h>

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/wx.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace {

bool RunScenario(WxTerminalCtrl& terminal)
{
    const auto fail = [](const char* reason) {
        std::fprintf(stderr, "wx scenario failed: %s\n", reason);
        return false;
    };
    SakuraTerminal* core = terminal.Core();
    if (!sakura_terminal_is_ready(core))
        return fail("core not ready");

    const char* text = "\x1b[2J\x1b[Hscenario";
    sakura_terminal_feed_output(core, text, std::strlen(text));
    terminal.RefreshFrame();
    wxYield();
    terminal.Update();
    const WxPaintMetrics initial_paint = terminal.GetPaintMetrics();
    if (initial_paint.full_repaints == 0 ||
        initial_paint.painted_cells == 0 ||
        initial_paint.full_refresh_requests == 0)
        return fail("initial full paint metrics");

    const char* update = "\x1b[2;1Hdelta";
    sakura_terminal_feed_output(core, update, std::strlen(update));
    terminal.RefreshFrame();
    wxYield();
    terminal.Update();
    const WxPaintMetrics incremental_paint = terminal.GetPaintMetrics();
    if (incremental_paint.partial_repaints == 0 ||
        incremental_paint.dirty_refresh_requests == 0 ||
        incremental_paint.painted_cells - initial_paint.painted_cells >=
            initial_paint.painted_cells)
        return fail("incremental paint metrics");

    const char* repeated_glyphs = "\x1b[3;1H界é";
    const WxPaintMetrics glyph_before = terminal.GetPaintMetrics();
    sakura_terminal_feed_output(core, repeated_glyphs,
                                std::strlen(repeated_glyphs));
    terminal.RefreshFrame();
    wxYield();
    terminal.Update();
    const WxPaintMetrics glyph_first = terminal.GetPaintMetrics();
    sakura_terminal_feed_output(core, repeated_glyphs,
                                std::strlen(repeated_glyphs));
    terminal.RefreshFrame();
    wxYield();
    terminal.Update();
    const WxPaintMetrics glyph_second = terminal.GetPaintMetrics();
    if (glyph_first.glyph_run_cache_misses <=
            glyph_before.glyph_run_cache_misses ||
        glyph_second.glyph_run_cache_hits <= glyph_first.glyph_run_cache_hits)
        return fail("glyph run cache metrics");

    sakura_terminal_start_selection(core, 0, 0);
    sakura_terminal_update_selection(core, 7, 0);
    terminal.RefreshFrame();
    wxYield();
    terminal.Update();
    char* initial_copy = sakura_terminal_copy_selection(core);
    const std::string initial_text = initial_copy == nullptr ? "" : initial_copy;
    sakura_terminal_free_string(initial_copy);
    if (initial_text.find("scenario") == std::string::npos)
        return fail("initial selection copy");
    sakura_terminal_update_selection(core, 9, 0);
    char* copied_value = sakura_terminal_copy_selection(core);
    const std::string copied = copied_value == nullptr ? "" : copied_value;
    sakura_terminal_free_string(copied_value);
    if (copied.find("scenario") == std::string::npos)
        return fail("extended selection copy");

    if (wxTheClipboard == nullptr || !wxTheClipboard->Open())
        return fail("clipboard open for write");
    const bool copied_to_clipboard = wxTheClipboard->SetData(
        new wxTextDataObject(wxString::FromUTF8(copied)));
    wxTheClipboard->Close();
    if (!copied_to_clipboard)
        return false;

    if (wxTheClipboard == nullptr || !wxTheClipboard->Open())
        return fail("clipboard open for read");
    wxTextDataObject data;
    const bool available = wxTheClipboard->GetData(data);
    wxTheClipboard->Close();
    if (!available)
        return fail("clipboard data unavailable");
    const wxString pasted_text = data.GetText();
    const auto utf8 = pasted_text.ToUTF8();
    if (utf8.data() == nullptr)
        return fail("clipboard UTF-8 conversion");
    sakura_terminal_paste(core, utf8.data(), utf8.length());

    SakuraTerminalMetrics metrics {};
    sakura_terminal_get_metrics(core, &metrics);
    return metrics.selection_copies >= 2 && metrics.paste_bytes >= 8;
}

class SakuraWxApp final : public wxApp {
public:
    SakuraWxApp()
        : smoke_timer_(this)
    {
        Bind(wxEVT_TIMER, &SakuraWxApp::OnSmokeTimer, this,
             smoke_timer_.GetId());
    }

    bool OnInit() override
    {
        auto* frame = new wxFrame(nullptr, wxID_ANY,
                                  "Sakura wx prototype — libtsm",
                                  wxDefaultPosition, wxSize(960, 540));
        TerminalCallbacks callbacks;
        callbacks.on_title_changed = [frame](const std::string& title) {
            wxString caption = "Sakura wx prototype — libtsm";
            if (!title.empty()) {
                wxString terminal_title = wxString::FromUTF8(title.c_str());
                terminal_title.Replace("\r", " ");
                terminal_title.Replace("\n", " ");
                if (terminal_title.length() > 120)
                    terminal_title = terminal_title.Left(117) + "...";
                caption += " — " + terminal_title;
            }
            frame->SetTitle(caption);
        };
        TerminalConfig config;
        if (std::getenv("SAKURA_WX_SCENARIO_TEST") != nullptr)
            config.start_transport = false;
        auto* terminal = new WxTerminalCtrl(frame, CreateTerminalTransport(),
                                            std::move(config),
                                            std::move(callbacks));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(terminal, 1, wxEXPAND);
        frame->SetSizer(sizer);
        frame->Show();
        terminal->SetFocus();

        if (std::getenv("SAKURA_WX_SCENARIO_TEST") != nullptr &&
            !RunScenario(*terminal))
            return false;
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
