#include <sakura/terminal/factory.h>
#include <sakura/terminal/core.h>
#include <sakura/wx/terminal_ctrl.h>

#include <stdexcept>

int main()
{
    TerminalConfig config;
    TerminalCallbacks callbacks;
    callbacks.on_error = [](const std::string&) {};
    if (config.font_size <= 0 || !callbacks.on_error)
        return 1;

    TerminalCore core(nullptr);
    if (!core.IsReady())
        throw std::runtime_error("installed-style terminal core was not ready");
    return CreateTerminalTransport() == nullptr ? 1 : 0;
}
