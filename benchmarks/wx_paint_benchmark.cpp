#include <sakura/wx/terminal_ctrl.h>

#include <wx/wx.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

constexpr unsigned int kIterations = 100;

struct MetricDelta {
    std::uint64_t paint_events = 0;
    std::uint64_t full_repaints = 0;
    std::uint64_t partial_repaints = 0;
    std::uint64_t framebuffer_rebuilds = 0;
    std::uint64_t painted_cells = 0;
    std::uint64_t paint_time_us = 0;
    std::uint64_t refresh_requests = 0;
    std::uint64_t full_refresh_requests = 0;
    std::uint64_t dirty_refresh_requests = 0;
};

MetricDelta Difference(const WxPaintMetrics& before,
                       const WxPaintMetrics& after)
{
    return {
        after.paint_events - before.paint_events,
        after.full_repaints - before.full_repaints,
        after.partial_repaints - before.partial_repaints,
        after.framebuffer_rebuilds - before.framebuffer_rebuilds,
        after.painted_cells - before.painted_cells,
        after.paint_time_us - before.paint_time_us,
        after.refresh_requests - before.refresh_requests,
        after.full_refresh_requests - before.full_refresh_requests,
        after.dirty_refresh_requests - before.dirty_refresh_requests,
    };
}

std::string MakeScreen(const std::string& prefix, bool unicode)
{
    std::string result;
    for (unsigned int row = 0; row < 32; ++row) {
        result += prefix;
        result += " row=";
        result += std::to_string(row);
        if (unicode)
            result += " 界é";
        result += " payload=0123456789abcdefghijklmnopqrstuvwxyz\r\n";
    }
    return result;
}

enum class ScenarioKind {
    FullAscii,
    PartialAscii,
    PartialUnicode,
    Cursor,
    Selection,
};

const char* ScenarioName(ScenarioKind kind)
{
    switch (kind) {
    case ScenarioKind::FullAscii: return "full-ascii";
    case ScenarioKind::PartialAscii: return "partial-ascii";
    case ScenarioKind::PartialUnicode: return "partial-unicode";
    case ScenarioKind::Cursor: return "cursor";
    case ScenarioKind::Selection: return "selection";
    }
    return "unknown";
}

class BenchmarkApp final : public wxApp {
public:
    bool OnInit() override
    {
        return true;
    }

    int OnRun() override
    {
        std::cout << "scenario\titerations\telapsed_ms\tpaint_events\t"
                     "full_repaints\tpartial_repaints\tpainted_cells\t"
                     "paint_time_us\tfull_refreshes\tdirty_refreshes\n";
        const ScenarioKind scenarios[] = {
            ScenarioKind::FullAscii,
            ScenarioKind::PartialAscii,
            ScenarioKind::PartialUnicode,
            ScenarioKind::Cursor,
            ScenarioKind::Selection,
        };
        for (ScenarioKind scenario : scenarios)
            RunScenario(scenario);
        return 0;
    }

private:
    static void PumpPaint(WxTerminalCtrl& terminal)
    {
        terminal.RefreshFrame();
        wxYield();
        terminal.Update();
        wxYield();
    }

    void RunScenario(ScenarioKind kind)
    {
        auto* frame = new wxFrame(nullptr, wxID_ANY, "Sakura wx paint benchmark",
                                  wxDefaultPosition, wxSize(1280, 720));
        TerminalConfig config;
        config.start_transport = false;
        config.timer_interval_ms = 1000000;
        auto* terminal = new WxTerminalCtrl(frame, nullptr, config, {});
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(terminal, 1, wxEXPAND);
        frame->SetSizer(sizer);
        frame->Show();
        wxYield();

        SakuraTerminal* core = terminal->Core();
        const bool unicode = kind == ScenarioKind::PartialUnicode;
        const std::string initial = MakeScreen("benchmark", unicode);
        sakura_terminal_feed_output(core, "\033[2J\033[H", 7);
        sakura_terminal_feed_output(core, initial.data(), initial.size());
        PumpPaint(*terminal);

        const WxPaintMetrics before = terminal->GetPaintMetrics();
        const auto start = std::chrono::steady_clock::now();
        for (unsigned int iteration = 0; iteration < kIterations; ++iteration) {
            switch (kind) {
            case ScenarioKind::FullAscii: {
                const std::string update = "\033[2J\033[H" +
                    MakeScreen("full", false);
                sakura_terminal_feed_output(core, update.data(), update.size());
                break;
            }
            case ScenarioKind::PartialAscii: {
                const std::string update = "\033[1;1Hdelta-0123456789";
                sakura_terminal_feed_output(core, update.data(), update.size());
                break;
            }
            case ScenarioKind::PartialUnicode: {
                const std::string update = "\033[2;1H界é";
                sakura_terminal_feed_output(core, update.data(), update.size());
                break;
            }
            case ScenarioKind::Cursor: {
                const unsigned int row = iteration % 20 + 1;
                const std::string cursor = "\033[" + std::to_string(row) + ";1H";
                sakura_terminal_feed_output(core, cursor.data(), cursor.size());
                break;
            }
            case ScenarioKind::Selection:
                sakura_terminal_start_selection(core, 0, 0);
                sakura_terminal_update_selection(core, 3 + iteration % 20, 0);
                break;
            }
            PumpPaint(*terminal);
        }
        const auto finish = std::chrono::steady_clock::now();
        const MetricDelta metrics = Difference(before, terminal->GetPaintMetrics());
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(finish - start).count();
        std::cout << ScenarioName(kind) << '\t'
                  << kIterations << '\t'
                  << elapsed_ms << '\t'
                  << metrics.paint_events << '\t'
                  << metrics.full_repaints << '\t'
                  << metrics.partial_repaints << '\t'
                  << metrics.painted_cells << '\t'
                  << metrics.paint_time_us << '\t'
                  << metrics.full_refresh_requests << '\t'
                  << metrics.dirty_refresh_requests << '\n';

        frame->Destroy();
        wxYield();
    }
};

} // namespace

wxIMPLEMENT_APP(BenchmarkApp);
