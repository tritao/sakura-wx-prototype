#include <tsm/libtsm.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr unsigned int kColumns = 120;
constexpr unsigned int kRows = 40;
constexpr unsigned int kIterations = 2000;

void IgnoreVteWrite(struct tsm_vte*, const char*, std::size_t, void*) {}

struct DrawStats {
    std::uint64_t cells = 0;
    std::uint64_t codepoints = 0;
    std::uint64_t checksum = 0;
};

int CountDrawnCell(struct tsm_screen*,
                   std::uint64_t id,
                   const std::uint32_t* codepoints,
                   std::size_t length,
                   unsigned int width,
                   unsigned int column,
                   unsigned int row,
                   const struct tsm_screen_attr*,
                   tsm_age_t age,
                   void* data)
{
    auto* stats = static_cast<DrawStats*>(data);
    ++stats->cells;
    stats->codepoints += length;
    stats->checksum ^= id + (static_cast<std::uint64_t>(width) << 8) +
                       (static_cast<std::uint64_t>(column) << 16) +
                       (static_cast<std::uint64_t>(row) << 32) +
                       (static_cast<std::uint64_t>(age) << 40);
    if (length != 0 && codepoints != nullptr)
        stats->checksum ^= codepoints[0];
    return 0;
}

class BenchmarkScreen final {
public:
    BenchmarkScreen(unsigned int columns, unsigned int rows)
    {
        if (tsm_screen_new(&screen_, nullptr, nullptr) != 0)
            throw std::runtime_error("tsm_screen_new failed");
        tsm_screen_set_max_sb(screen_, 100);
        if (tsm_screen_resize(screen_, columns, rows) != 0) {
            tsm_screen_unref(screen_);
            screen_ = nullptr;
            throw std::runtime_error("tsm_screen_resize failed");
        }
        if (tsm_vte_new(&vte_, screen_, &IgnoreVteWrite, nullptr,
                        nullptr, nullptr) != 0) {
            tsm_screen_unref(screen_);
            screen_ = nullptr;
            throw std::runtime_error("tsm_vte_new failed");
        }
    }

    ~BenchmarkScreen()
    {
        if (vte_ != nullptr)
            tsm_vte_unref(vte_);
        if (screen_ != nullptr)
            tsm_screen_unref(screen_);
    }

    BenchmarkScreen(const BenchmarkScreen&) = delete;
    BenchmarkScreen& operator=(const BenchmarkScreen&) = delete;

    void Feed(const std::string& input)
    {
        tsm_vte_input(vte_, input.data(), input.size());
    }

    DrawStats DrawFull()
    {
        DrawStats stats;
        age_ = tsm_screen_draw(screen_, &CountDrawnCell, &stats);
        return stats;
    }

    DrawStats DrawSince()
    {
        DrawStats stats;
        age_ = tsm_screen_draw_since(screen_, age_, &CountDrawnCell, &stats);
        return stats;
    }

    DrawStats Draw2()
    {
        DrawStats stats;
        const struct tsm_screen_cell* cells = tsm_screen_draw2(screen_);
        if (cells == nullptr)
            throw std::runtime_error("tsm_screen_draw2 failed");

        const std::uint64_t cell_count =
            static_cast<std::uint64_t>(tsm_screen_get_width(screen_)) *
            tsm_screen_get_height(screen_);
        stats.cells = cell_count;
        for (std::uint64_t i = 0; i < cell_count; ++i) {
            stats.codepoints += cells[i].ch != 0;
            stats.checksum ^= cells[i].ch +
                              (static_cast<std::uint64_t>(cells[i].fg.r) << 8) +
                              (static_cast<std::uint64_t>(cells[i].bg.b) << 16) +
                              (i << 24);
        }
        return stats;
    }

private:
    struct tsm_screen* screen_ = nullptr;
    struct tsm_vte* vte_ = nullptr;
    tsm_age_t age_ = 0;
};

enum class DrawMode {
    Full,
    Since,
    Draw2,
};

const char* ModeName(DrawMode mode)
{
    switch (mode) {
    case DrawMode::Full:
        return "draw";
    case DrawMode::Since:
        return "draw_since";
    case DrawMode::Draw2:
        return "draw2";
    }
    return "unknown";
}

DrawStats Draw(BenchmarkScreen& screen, DrawMode mode)
{
    switch (mode) {
    case DrawMode::Full:
        return screen.DrawFull();
    case DrawMode::Since:
        return screen.DrawSince();
    case DrawMode::Draw2:
        return screen.Draw2();
    }
    throw std::runtime_error("unknown draw mode");
}

struct Scenario {
    const char* name;
    std::string initial;
    std::string change;
};

std::string MakeInitial(const std::string& prefix)
{
    std::string result;
    for (unsigned int row = 0; row < kRows; ++row) {
        result += prefix;
        result += " row=";
        result += std::to_string(row);
        result += " payload=0123456789abcdefghijklmnopqrstuvwxyz\r\n";
    }
    return result;
}

void RunScenario(const Scenario& scenario, DrawMode mode)
{
    BenchmarkScreen screen(kColumns, kRows);
    screen.Feed(scenario.initial);
    Draw(screen, DrawMode::Full);

    const auto start = std::chrono::steady_clock::now();
    DrawStats last;
    for (unsigned int iteration = 0; iteration < kIterations; ++iteration) {
        if (!scenario.change.empty())
            screen.Feed(scenario.change);
        last = Draw(screen, mode);
    }
    const auto finish = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(finish - start).count();

    std::cout << scenario.name << '\t'
              << ModeName(mode) << '\t'
              << kIterations << '\t'
              << elapsed_ms << '\t'
              << last.cells << '\t'
              << last.codepoints << '\t'
              << last.checksum << '\n';
}

} // namespace

int main()
{
    try {
        const Scenario scenarios[] = {
            {"ascii-idle", MakeInitial("ascii"), {}},
            {"unicode-idle", MakeInitial("unicode-\xe7\x95\x8c-e\xcc\x81"), {}},
            {"ascii-changing", MakeInitial("changing"),
             "\033[1;1Hstatus-change-0123456789"},
        };
        const DrawMode modes[] = {
            DrawMode::Full,
            DrawMode::Since,
            DrawMode::Draw2,
        };

        std::cout << "scenario\tmode\titerations\telapsed_ms\tlast_cells\t"
                     "last_codepoints\tchecksum\n";
        for (const Scenario& scenario : scenarios) {
            for (DrawMode mode : modes)
                RunScenario(scenario, mode);
        }
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
