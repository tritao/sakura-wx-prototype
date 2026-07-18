#include <sakura/terminal/core_c.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr unsigned int kColumns = 120;
constexpr unsigned int kRows = 40;
constexpr unsigned int kScrollbackLines = 240;
constexpr unsigned int kIterations = 500;

using Clock = std::chrono::steady_clock;

void IgnoreWrite(void*, const char*, std::size_t) {}

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string MakeScreen(const std::string& prefix, bool unicode)
{
    std::string result;
    result.reserve(static_cast<std::size_t>(kColumns) * kRows);
    for (unsigned int row = 0; row < kRows; ++row) {
        result += "\x1b[" + std::to_string(row + 1) + ";1H";
        result += prefix;
        result += " row=";
        result += std::to_string(row);
        if (unicode)
            result += " 界é";
        result += " payload=0123456789abcdefghijklmnopqrstuvwxyz";
    }
    return result;
}

std::string MakeScrollback()
{
    std::string result;
    for (unsigned int line = 0; line < kScrollbackLines; ++line) {
        result += "scrollback line=";
        result += std::to_string(line);
        result += " payload=0123456789abcdefghijklmnopqrstuvwxyz\r\n";
    }
    return result;
}

std::string MakePartialUpdate(unsigned int iteration, bool unicode)
{
    const unsigned int row = iteration % kRows;
    std::string result = "\x1b[" + std::to_string(row + 1) + ";1H";
    result += unicode ? "界delta-é-" : "delta-";
    result += std::to_string(iteration);
    result += "-0123456789";
    return result;
}

struct Counters {
    std::uint64_t rendered_frames = 0;
    std::uint64_t decoded_cells = 0;
    std::uint64_t reused_cells = 0;
};

Counters GetCounters(const SakuraTerminal* terminal)
{
    SakuraTerminalMetrics metrics {};
    sakura_terminal_get_metrics(terminal, &metrics);
    return {
        metrics.rendered_frames,
        metrics.frame_cells_decoded,
        metrics.frame_cells_reused,
    };
}

struct RunStats {
    std::uint64_t count = 0;
    std::uint64_t cells = 0;
    std::uint64_t text_bytes = 0;
    std::uint64_t checksum = 0;
};

RunStats EnumerateRuns(const SakuraTerminalFrame* frame,
                       const SakuraTerminalFrameInfo& info)
{
    RunStats stats;
    for (unsigned int row = 0; row < info.rows; ++row) {
        const std::size_t run_count =
            sakura_terminal_frame_row_run_count(frame, row);
        for (std::size_t index = 0; index < run_count; ++index) {
            SakuraTerminalRunView run {};
            Check(sakura_terminal_frame_row_run(frame, row, index, &run),
                  "row-run lookup failed");
            Check(run.cell_count <= SAKURA_TERMINAL_RUN_SPAN_MAX_CELLS,
                  "row-run span exceeded the packed bound");
            ++stats.count;
            stats.cells += run.cell_count;
            stats.text_bytes += run.text_length;
            stats.checksum ^= run.style_id +
                (static_cast<std::uint64_t>(run.row) << 8) +
                (static_cast<std::uint64_t>(run.left) << 24);
            for (std::size_t text_index = 0; text_index < run.text_length;
                 ++text_index) {
                stats.checksum = (stats.checksum * 131) ^
                    static_cast<unsigned char>(run.text[text_index]);
            }
        }
    }
    return stats;
}

enum class ScenarioKind {
    FullRepaint,
    PartialRepaint,
    Unicode,
    Scrollback,
    RowRuns,
};

const char* ScenarioName(ScenarioKind scenario)
{
    switch (scenario) {
    case ScenarioKind::FullRepaint: return "full-repaint";
    case ScenarioKind::PartialRepaint: return "partial-repaint";
    case ScenarioKind::Unicode: return "unicode-wide-combining";
    case ScenarioKind::Scrollback: return "scrollback";
    case ScenarioKind::RowRuns: return "row-runs";
    }
    return "unknown";
}

struct BenchmarkResult {
    double feed_us = 0;
    double frame_us = 0;
    double runs_us = 0;
    double total_us = 0;
    Counters counters;
    RunStats runs;
    std::uint64_t changed_frames = 0;
    std::uint64_t full_repaints = 0;
};

BenchmarkResult RunScenario(ScenarioKind scenario)
{
    SakuraTerminal* terminal = sakura_terminal_new(&IgnoreWrite, nullptr);
    Check(terminal != nullptr && sakura_terminal_is_ready(terminal),
          "terminal creation failed");
    Check(sakura_terminal_resize(terminal, kColumns, kRows),
          "terminal resize failed");

    const bool unicode = scenario == ScenarioKind::Unicode;
    const std::string initial = scenario == ScenarioKind::Scrollback
        ? MakeScrollback() : MakeScreen("benchmark", unicode);
    const std::string full_update = "\x1b[2J\x1b[H" +
        MakeScreen("full", false);

    sakura_terminal_feed_output(terminal, initial.data(), initial.size());
    SakuraTerminalFrame* initial_frame = sakura_terminal_take_frame(terminal);
    Check(initial_frame != nullptr, "initial frame creation failed");
    sakura_terminal_frame_free(initial_frame);
    const Counters before = GetCounters(terminal);

    BenchmarkResult result;
    const auto total_start = Clock::now();
    for (unsigned int iteration = 0; iteration < kIterations; ++iteration) {
        std::string update;
        if (scenario == ScenarioKind::FullRepaint)
            update = full_update;
        else if (scenario == ScenarioKind::PartialRepaint)
            update = MakePartialUpdate(iteration, false);
        else if (scenario == ScenarioKind::Unicode)
            update = MakePartialUpdate(iteration, true);
        else if (scenario == ScenarioKind::Scrollback)
            update = "scroll line=" + std::to_string(iteration) +
                " payload=0123456789abcdefghijklmnopqrstuvwxyz\r\n";

        const auto feed_start = Clock::now();
        if (!update.empty())
            sakura_terminal_feed_output(terminal, update.data(), update.size());
        const auto feed_finish = Clock::now();
        result.feed_us += std::chrono::duration<double, std::micro>(
            feed_finish - feed_start).count();

        const auto frame_start = Clock::now();
        SakuraTerminalFrame* frame = sakura_terminal_take_frame(terminal);
        const auto frame_finish = Clock::now();
        Check(frame != nullptr, "frame creation failed");
        result.frame_us += std::chrono::duration<double, std::micro>(
            frame_finish - frame_start).count();

        SakuraTerminalFrameInfo info {};
        Check(sakura_terminal_frame_info(frame, &info),
              "frame information lookup failed");
        result.changed_frames += info.changed != 0;
        result.full_repaints += info.full_repaint != 0;

        const auto runs_start = Clock::now();
        const RunStats runs = EnumerateRuns(frame, info);
        const auto runs_finish = Clock::now();
        result.runs_us += std::chrono::duration<double, std::micro>(
            runs_finish - runs_start).count();
        result.runs = runs;
        sakura_terminal_frame_free(frame);
    }
    const auto total_finish = Clock::now();
    result.total_us = std::chrono::duration<double, std::micro>(
        total_finish - total_start).count();

    const Counters after = GetCounters(terminal);
    result.counters.rendered_frames = after.rendered_frames -
        before.rendered_frames;
    result.counters.decoded_cells = after.decoded_cells - before.decoded_cells;
    result.counters.reused_cells = after.reused_cells - before.reused_cells;
    sakura_terminal_free(terminal);
    return result;
}

} // namespace

int main()
{
    try {
        std::cout << "scenario\titerations\tfeed_us/frame\tframe_us/frame\t"
                     "runs_us/frame\ttotal_us/frame\tchanged\tfull\t"
                     "decoded_cells\treused_cells\truns\trun_cells\t"
                     "text_bytes\tchecksum\n";
        const ScenarioKind scenarios[] = {
            ScenarioKind::FullRepaint,
            ScenarioKind::PartialRepaint,
            ScenarioKind::Unicode,
            ScenarioKind::Scrollback,
            ScenarioKind::RowRuns,
        };
        std::cout << std::fixed << std::setprecision(3);
        for (const ScenarioKind scenario : scenarios) {
            const BenchmarkResult result = RunScenario(scenario);
            const double divisor = static_cast<double>(kIterations);
            std::cout << ScenarioName(scenario) << '\t'
                      << kIterations << '\t'
                      << result.feed_us / divisor << '\t'
                      << result.frame_us / divisor << '\t'
                      << result.runs_us / divisor << '\t'
                      << result.total_us / divisor << '\t'
                      << result.changed_frames << '\t'
                      << result.full_repaints << '\t'
                      << result.counters.decoded_cells << '\t'
                      << result.counters.reused_cells << '\t'
                      << result.runs.count << '\t'
                      << result.runs.cells << '\t'
                      << result.runs.text_bytes << '\t'
                      << result.runs.checksum << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
