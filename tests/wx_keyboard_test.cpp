#include <sakura/wx/terminal_ctrl.h>

#include <wx/frame.h>

#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class RecordingTransport final : public TerminalTransport {
public:
    explicit RecordingTransport(std::string& writes)
        : writes_(writes)
    {
    }

    bool Start(unsigned int, unsigned int, const std::string&) override
    {
        status_.state = TransportState::Running;
        return true;
    }

    void Stop() override
    {
        status_.state = TransportState::Stopped;
    }

    bool Write(const char* data, std::size_t length) override
    {
        writes_.append(data, length);
        metrics_.bytes_written += length;
        ++metrics_.write_events;
        return true;
    }

    bool Resize(unsigned int, unsigned int) override
    {
        ++metrics_.resize_events;
        return true;
    }

    std::vector<std::string> TakeOutput(std::size_t) override
    {
        return {};
    }

    void DiscardOutput() override
    {
    }

    bool IsRunning() const override
    {
        return status_.state == TransportState::Running;
    }

    TransportStatus GetStatus() const override
    {
        return status_;
    }

    TransportMetrics GetMetrics() const override
    {
        return metrics_;
    }

private:
    std::string& writes_;
    TransportStatus status_;
    TransportMetrics metrics_;
};

void SendChar(WxTerminalCtrl& terminal, wxKeyEvent& event)
{
    event.SetEventObject(&terminal);
    Check(terminal.GetEventHandler()->ProcessEvent(event),
          "wx terminal did not handle the character event");
}

int RunKeyboardTest()
{
    wxFrame frame(nullptr, wxID_ANY, "Sakura keyboard test",
                 wxDefaultPosition, wxSize(640, 480));
    std::string writes;
    auto transport = std::make_unique<RecordingTransport>(writes);
    TerminalConfig config;
    config.start_transport = false;
    config.timer_interval_ms = 60000;
    WxTerminalCtrl terminal(&frame, std::move(transport), config);

    // This is the representation produced by wxGTK and by wx's other
    // backends for a physical Ctrl-C: a control-code character plus the raw
    // Ctrl modifier. RawControlDown() is the portable wx API for this.
    wxKeyEvent physical_control_c(wxEVT_CHAR);
    physical_control_c.m_keyCode = WXK_CONTROL_C;
    physical_control_c.m_uniChar = WXK_CONTROL_C;
    physical_control_c.SetRawControlDown(true);
    SendChar(terminal, physical_control_c);
    Check(writes == std::string(1, '\x03'),
          "physical Ctrl-C was not encoded as ETX");
    writes.clear();

    // On macOS ControlDown() is the primary Command modifier, while on
    // other platforms it is physical Ctrl. The terminal must preserve that
    // distinction: Command-A is text input, Ctrl-A is SOH.
    wxKeyEvent primary_a(wxEVT_CHAR);
    primary_a.m_keyCode = 'a';
    primary_a.m_uniChar = 'a';
    primary_a.SetControlDown(true);
    SendChar(terminal, primary_a);
#if defined(__WXOSX__)
    Check(writes == "a", "Command-A was incorrectly encoded as Ctrl-A");
#else
    Check(writes == std::string(1, '\x01'),
          "Ctrl-A was not encoded as SOH");
#endif

    return 0;
}

class KeyboardTestApp final : public wxApp {
public:
    bool OnInit() override
    {
        try {
            result_ = RunKeyboardTest();
        } catch (const std::exception& error) {
            std::cerr << "wx_keyboard: FAIL: " << error.what() << '\n';
            result_ = 1;
        }
        CallAfter([this]() { ExitMainLoop(); });
        return true;
    }

    int OnExit() override
    {
        if (result_ == 0)
            std::cout << "wx_keyboard: PASS\n";
        return result_;
    }

private:
    int result_ = 1;
};

} // namespace

wxIMPLEMENT_APP(KeyboardTestApp);
