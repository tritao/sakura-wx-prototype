#include <sakura/wx/terminal_ctrl.h>

#include <wx/frame.h>

#include <iostream>

namespace {

int RunFontTest()
{
    TerminalConfig system_config;
    if (!system_config.use_system_font || system_config.font_size != 10) {
        std::cerr << "unexpected system font defaults\n";
        return 2;
    }

    wxFrame frame(nullptr, wxID_ANY, "Sakura font test", wxDefaultPosition,
                 wxSize(640, 480));
    WxTerminalCtrl system_terminal(&frame, nullptr, system_config);
    if (system_terminal.GetFontFamily().empty() ||
        system_terminal.GetFontSize() != system_config.font_size) {
        std::cerr << "system font did not resolve: family="
                  << system_terminal.GetFontFamily() << " size="
                  << system_terminal.GetFontSize() << '\n';
        return 3;
    }

    TerminalConfig explicit_config = system_config;
    explicit_config.use_system_font = false;
    explicit_config.font_family = "DejaVu Sans Mono";
    WxTerminalCtrl explicit_terminal(&frame, nullptr, explicit_config);
    if (explicit_terminal.GetFontFamily().empty() ||
        explicit_terminal.GetFontSize() != explicit_config.font_size) {
        std::cerr << "explicit font did not resolve\n";
        return 4;
    }

    std::cout << "system_family=" << system_terminal.GetFontFamily()
              << " explicit_family=" << explicit_terminal.GetFontFamily()
              << " point_size=" << system_terminal.GetFontSize() << '\n';
    return 0;
}

class FontTestApp final : public wxApp {
public:
    bool OnInit() override
    {
        result_ = RunFontTest();
        CallAfter([this]() { ExitMainLoop(); });
        return true;
    }

    int OnExit() override
    {
        return result_;
    }

private:
    int result_ = 1;
};

} // namespace

wxIMPLEMENT_APP(FontTestApp);
