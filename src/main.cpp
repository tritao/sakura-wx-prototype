#include <sakura/terminal/factory.h>

#include <sakura/wx/terminal_ctrl.h>

#include <wx/wx.h>

#include <cstdlib>

namespace {

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
        auto* terminal = new WxTerminalCtrl(frame, CreateTerminalTransport());
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(terminal, 1, wxEXPAND);
        frame->SetSizer(sizer);
        frame->Show();
        terminal->SetFocus();

        if (std::getenv("SAKURA_WX_SCENARIO_TEST") != nullptr &&
            !terminal->RunScenario())
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
