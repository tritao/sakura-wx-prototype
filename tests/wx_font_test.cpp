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
    if (system_config.theme.background[0] != 16 ||
        system_config.theme.background[1] != 18 ||
        system_config.theme.background[2] != 20) {
        std::cerr << "unexpected wx theme background defaults\n";
        return 3;
    }

    wxFrame frame(nullptr, wxID_ANY, "Sakura font test", wxDefaultPosition,
                 wxSize(640, 480));
    WxTerminalCtrl system_terminal(&frame, nullptr, system_config);
    if (system_terminal.GetFontFamily().empty() ||
        system_terminal.GetFontSize() != system_config.font_size) {
        std::cerr << "system font did not resolve: family="
                  << system_terminal.GetFontFamily() << " size="
                  << system_terminal.GetFontSize() << '\n';
        return 4;
    }

    TerminalConfig explicit_config = system_config;
    explicit_config.use_system_font = false;
    explicit_config.font_family = "DejaVu Sans Mono";
    WxTerminalCtrl explicit_terminal(&frame, nullptr, explicit_config);
    if (explicit_terminal.GetFontFamily().empty() ||
        explicit_terminal.GetFontSize() != explicit_config.font_size) {
        std::cerr << "explicit font did not resolve\n";
        return 5;
    }

    TerminalConfig themed_config = system_config;
    themed_config.theme.background[0] = 11;
    themed_config.theme.background[1] = 22;
    themed_config.theme.background[2] = 33;
    themed_config.theme.ansi[1][0] = 44;
    themed_config.theme.ansi[1][1] = 55;
    themed_config.theme.ansi[1][2] = 66;
    WxTerminalCtrl themed_terminal(&frame, nullptr, themed_config);
    const char styled_text[] = "\033[31mX";
    sakura_terminal_feed_output(themed_terminal.Core(), styled_text,
                                sizeof(styled_text) - 1);
    SakuraTerminalFrame* themed_frame =
        sakura_terminal_take_frame(themed_terminal.Core());
    SakuraTerminalCellView themed_cell {};
    if (themed_frame == nullptr ||
        !sakura_terminal_frame_cell(themed_frame, 0, 0, &themed_cell) ||
        themed_cell.foreground[0] != 44 || themed_cell.foreground[1] != 55 ||
        themed_cell.foreground[2] != 66 || themed_cell.background[0] != 11 ||
        themed_cell.background[1] != 22 || themed_cell.background[2] != 33) {
        sakura_terminal_frame_free(themed_frame);
        std::cerr << "custom wx theme was not propagated to the terminal core\n";
        return 6;
    }
    sakura_terminal_frame_free(themed_frame);

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
