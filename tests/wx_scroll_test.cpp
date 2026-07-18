#include <sakura/wx/terminal_ctrl.h>

#include <wx/frame.h>

#include <iostream>

namespace {

void SendWheel(WxTerminalCtrl& terminal, int rotation, int delta = 120,
               int lines_per_action = 3)
{
    wxMouseEvent event(wxEVT_MOUSEWHEEL);
    event.SetEventObject(&terminal);
    event.m_wheelAxis = wxMOUSE_WHEEL_VERTICAL;
    event.m_wheelRotation = rotation;
    event.m_wheelDelta = delta;
    event.m_wheelInverted = false;
    event.m_linesPerAction = lines_per_action;
    event.m_columnsPerAction = 3;
    terminal.GetEventHandler()->ProcessEvent(event);
}

void Tick(WxTerminalCtrl& terminal)
{
    wxTimer timer(&terminal);
    wxTimerEvent event(timer);
    terminal.GetEventHandler()->ProcessEvent(event);
}

int RunScrollTest()
{
    wxFrame frame(nullptr, wxID_ANY, "Sakura scroll test", wxDefaultPosition,
                 wxSize(640, 480));
    TerminalConfig config;
    config.start_transport = false;
    config.timer_interval_ms = 60000;
    WxTerminalCtrl terminal(&frame, nullptr, config);

    const WxPaintMetrics initial = terminal.GetPaintMetrics();
    SendWheel(terminal, 60);
    const WxPaintMetrics partial = terminal.GetPaintMetrics();
    if (partial.wheel_events != initial.wheel_events + 1 ||
        partial.wheel_partial_events != initial.wheel_partial_events + 1 ||
        partial.wheel_scroll_updates != initial.wheel_scroll_updates) {
        std::cerr << "partial wheel rotation was applied too early\n";
        return 1;
    }

    Tick(terminal);
    const WxPaintMetrics still_partial = terminal.GetPaintMetrics();
    if (still_partial.wheel_scroll_updates != initial.wheel_scroll_updates) {
        std::cerr << "partial wheel rotation changed viewport\n";
        return 2;
    }

    SendWheel(terminal, 60);
    Tick(terminal);
    const WxPaintMetrics first_line = terminal.GetPaintMetrics();
    if (first_line.wheel_scroll_updates != initial.wheel_scroll_updates + 1 ||
        first_line.wheel_lines_scrolled != initial.wheel_lines_scrolled + 1) {
        std::cerr << "one accumulated wheel action did not scroll one line\n";
        return 3;
    }

    Tick(terminal);
    Tick(terminal);
    const WxPaintMetrics completed_action = terminal.GetPaintMetrics();
    if (completed_action.wheel_scroll_updates != initial.wheel_scroll_updates + 3 ||
        completed_action.wheel_lines_scrolled != initial.wheel_lines_scrolled + 3) {
        std::cerr << "wheel action was not drained at one line per tick\n";
        return 4;
    }

    SendWheel(terminal, 120);
    SendWheel(terminal, 120);
    const WxPaintMetrics queued = terminal.GetPaintMetrics();
    if (queued.wheel_scroll_updates != completed_action.wheel_scroll_updates) {
        std::cerr << "wheel events were applied before the UI tick\n";
        return 5;
    }
    Tick(terminal);
    const WxPaintMetrics coalesced = terminal.GetPaintMetrics();
    if (coalesced.wheel_scroll_updates != queued.wheel_scroll_updates + 1 ||
        coalesced.wheel_lines_scrolled != queued.wheel_lines_scrolled + 1) {
        std::cerr << "wheel burst was not coalesced and smoothed\n";
        return 6;
    }

    return 0;
}

class ScrollTestApp final : public wxApp {
public:
    bool OnInit() override
    {
        result_ = RunScrollTest();
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

wxIMPLEMENT_APP(ScrollTestApp);
