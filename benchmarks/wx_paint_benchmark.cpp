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
    std::uint64_t max_paint_time_us = 0;
    std::uint64_t refresh_requests = 0;
    std::uint64_t full_refresh_requests = 0;
    std::uint64_t dirty_refresh_requests = 0;
    std::uint64_t glyph_run_cache_hits = 0;
    std::uint64_t glyph_run_cache_misses = 0;
    std::uint64_t glyph_run_cache_evictions = 0;
    std::uint64_t glyph_run_cache_entries = 0;
    std::uint64_t glyph_run_cache_bytes = 0;
    std::uint64_t glyph_run_cache_peak_bytes = 0;
    std::uint64_t background_rectangles = 0;
    std::uint64_t glyph_bitmap_draws = 0;
    std::uint64_t glyph_text_draws = 0;
    std::uint64_t dc_state_changes = 0;
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
        after.max_paint_time_us,
        after.refresh_requests - before.refresh_requests,
        after.full_refresh_requests - before.full_refresh_requests,
        after.dirty_refresh_requests - before.dirty_refresh_requests,
        after.glyph_run_cache_hits - before.glyph_run_cache_hits,
        after.glyph_run_cache_misses - before.glyph_run_cache_misses,
        after.glyph_run_cache_evictions - before.glyph_run_cache_evictions,
        after.glyph_run_cache_entries,
        after.glyph_run_cache_bytes,
        after.glyph_run_cache_peak_bytes,
        after.background_rectangles - before.background_rectangles,
        after.glyph_bitmap_draws - before.glyph_bitmap_draws,
        after.glyph_text_draws - before.glyph_text_draws,
        after.dc_state_changes - before.dc_state_changes,
    };
}

std::string MakeScreen(const std::string& prefix, bool unicode,
                       unsigned int rows = 32)
{
    std::string result;
    for (unsigned int row = 0; row < rows; ++row) {
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
    PartialUnicodeUncached,
    GlyphCacheChurn,
    BurstOutput,
    LargeScreen,
    Resize,
    Scroll,
    Cursor,
    Selection,
};

const char* ScenarioName(ScenarioKind kind)
{
    switch (kind) {
    case ScenarioKind::FullAscii: return "full-ascii";
    case ScenarioKind::PartialAscii: return "partial-ascii";
    case ScenarioKind::PartialUnicode: return "partial-unicode";
    case ScenarioKind::PartialUnicodeUncached:
        return "partial-unicode-uncached";
    case ScenarioKind::GlyphCacheChurn: return "glyph-cache-churn";
    case ScenarioKind::BurstOutput: return "burst-output";
    case ScenarioKind::LargeScreen: return "large-screen";
    case ScenarioKind::Resize: return "resize";
    case ScenarioKind::Scroll: return "scroll";
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
                     "paint_time_us\tmax_paint_us\tfull_refreshes\t"
                     "dirty_refreshes\t"
                     "glyph_cache_hits\tglyph_cache_misses\t"
                     "glyph_cache_evictions\tglyph_cache_entries\t"
                     "glyph_cache_bytes\tglyph_cache_peak_bytes\t"
                     "background_rectangles\tglyph_bitmap_draws\t"
                     "glyph_text_draws\tdc_state_changes\n";
        const ScenarioKind scenarios[] = {
            ScenarioKind::FullAscii,
            ScenarioKind::PartialAscii,
            ScenarioKind::PartialUnicode,
            ScenarioKind::PartialUnicodeUncached,
            ScenarioKind::GlyphCacheChurn,
            ScenarioKind::BurstOutput,
            ScenarioKind::LargeScreen,
            ScenarioKind::Resize,
            ScenarioKind::Scroll,
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
        config.glyph_cache_enabled = kind !=
            ScenarioKind::PartialUnicodeUncached;
        if (kind == ScenarioKind::GlyphCacheChurn) {
            config.glyph_cache_max_bytes = 16 * 1024;
            config.glyph_cache_max_entries = 128;
        }
        auto* terminal = new WxTerminalCtrl(frame, nullptr, config, {});
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(terminal, 1, wxEXPAND);
        frame->SetSizer(sizer);
        frame->Show();
        if (kind == ScenarioKind::LargeScreen) {
            frame->SetClientSize(wxSize(1600, 900));
            frame->Layout();
        }
        wxYield();

        SakuraTerminal* core = terminal->Core();
        const bool unicode = kind == ScenarioKind::PartialUnicode ||
            kind == ScenarioKind::PartialUnicodeUncached ||
            kind == ScenarioKind::GlyphCacheChurn;
        const unsigned int initial_rows = kind == ScenarioKind::LargeScreen
            ? 64 : 32;
        const std::string initial = MakeScreen(
            kind == ScenarioKind::Scroll ? "scroll" : "benchmark",
            unicode, initial_rows);
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
            case ScenarioKind::PartialUnicodeUncached: {
                const std::string update = "\033[2;1H界é";
                sakura_terminal_feed_output(core, update.data(), update.size());
                break;
            }
            case ScenarioKind::GlyphCacheChurn:
                for (unsigned int chunk = 0; chunk < 8; ++chunk) {
                    const unsigned int column = 1 + chunk * 9;
                    const unsigned int foreground =
                        16 + (iteration * 11 + chunk * 17) % 216;
                    const unsigned int background =
                        16 + (iteration * 7 + chunk * 23 + 37) % 216;
                    const std::string update = "\033[1;" +
                        std::to_string(column) + "H\033[38;5;" +
                        std::to_string(foreground) + "m\033[48;5;" +
                        std::to_string(background) + "mchurn-" +
                        std::to_string(iteration) + '-' +
                        std::to_string(chunk) + " 界é\033[0m";
                    sakura_terminal_feed_output(core, update.data(),
                                                update.size());
                }
                break;
            case ScenarioKind::BurstOutput:
                for (unsigned int chunk = 0; chunk < 8; ++chunk) {
                    const std::string update = "\033[1;1Hburst-" +
                        std::to_string(iteration) + '-' +
                        std::to_string(chunk);
                    sakura_terminal_feed_output(core, update.data(),
                                                update.size());
                }
                break;
            case ScenarioKind::LargeScreen: {
                const std::string update = "\033[10;1Hlarge-" +
                    std::to_string(iteration) + " 界é";
                sakura_terminal_feed_output(core, update.data(), update.size());
                break;
            }
            case ScenarioKind::Resize:
                frame->SetClientSize(iteration % 2 == 0
                    ? wxSize(1280, 720) : wxSize(1024, 600));
                frame->Layout();
                break;
            case ScenarioKind::Scroll: {
                const std::string update = "scroll-" +
                    std::to_string(iteration) + " payload=0123456789\r\n";
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
                  << metrics.max_paint_time_us << '\t'
                  << metrics.full_refresh_requests << '\t'
                  << metrics.dirty_refresh_requests << '\t'
                  << metrics.glyph_run_cache_hits << '\t'
                  << metrics.glyph_run_cache_misses << '\t'
                  << metrics.glyph_run_cache_evictions << '\t'
                  << metrics.glyph_run_cache_entries << '\t'
                  << metrics.glyph_run_cache_bytes << '\t'
                  << metrics.glyph_run_cache_peak_bytes << '\t'
                  << metrics.background_rectangles << '\t'
                  << metrics.glyph_bitmap_draws << '\t'
                  << metrics.glyph_text_draws << '\t'
                  << metrics.dc_state_changes << '\n';

        frame->Destroy();
        wxYield();
    }
};

} // namespace

wxIMPLEMENT_APP(BenchmarkApp);
