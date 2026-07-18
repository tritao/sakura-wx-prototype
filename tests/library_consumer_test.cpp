#include <sakura/terminal/factory.h>
#include <sakura/terminal/core_c.h>
#include <sakura/wx/terminal_ctrl.h>

#include <stdexcept>

int main()
{
    TerminalConfig config;
    TerminalCallbacks callbacks;
    callbacks.on_error = [](const std::string&) {};
    if (config.font_size <= 0 || !callbacks.on_error)
        return 1;

    SakuraTerminal* core = sakura_terminal_new(nullptr, nullptr);
    if (!sakura_terminal_is_ready(core)) {
        sakura_terminal_free(core);
        throw std::runtime_error("installed-style terminal core was not ready");
    }
    const int result = CreateTerminalTransport() == nullptr ? 1 : 0;
    sakura_terminal_free(core);
    return result;
}
