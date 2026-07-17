#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <tsm/libtsm.h>

struct TerminalCell {
    uint32_t codepoint = ' ';
    std::array<uint8_t, 3> foreground {};
    std::array<uint8_t, 3> background {};
    uint8_t attributes = 0;
};

struct TerminalSnapshot {
    unsigned int columns = 0;
    unsigned int rows = 0;
    unsigned int cursor_x = 0;
    unsigned int cursor_y = 0;
    bool cursor_visible = false;
    std::vector<TerminalCell> cells;
};

struct TerminalMetrics {
    uint64_t output_bytes = 0;
    uint64_t output_chunks = 0;
    uint64_t input_events = 0;
    uint64_t transport_write_bytes = 0;
    uint64_t transport_write_events = 0;
    uint64_t rendered_frames = 0;
    uint64_t render_latency_samples = 0;
    uint64_t max_render_latency_us = 0;
    uint64_t selection_copies = 0;
    uint64_t paste_bytes = 0;
};

class TerminalCore final {
public:
    using WriteCallback = std::function<void(const char*, std::size_t)>;

    explicit TerminalCore(WriteCallback write_callback);
    ~TerminalCore();

    TerminalCore(const TerminalCore&) = delete;
    TerminalCore& operator=(const TerminalCore&) = delete;

    bool IsReady() const { return screen_ != nullptr && vte_ != nullptr; }
    const std::string& Error() const { return error_; }

    bool Resize(unsigned int columns, unsigned int rows);
    void FeedOutput(const char* data, std::size_t length);
    bool HandleKey(uint32_t keysym, uint32_t ascii, unsigned int modifiers,
                   uint32_t unicode);
    void Paste(const std::string& text);
    void ScrollPageUp(unsigned int pages);
    void ScrollPageDown(unsigned int pages);

    void StartSelection(unsigned int column, unsigned int row);
    void UpdateSelection(unsigned int column, unsigned int row);
    void ClearSelection();
    bool HasSelection() const { return selection_active_; }
    std::string CopySelection();

    TerminalSnapshot TakeSnapshot();
    TerminalMetrics GetMetrics() const { return metrics_; }

private:
    static void VteWrite(struct tsm_vte* vte, const char* data,
                         std::size_t length, void* user_data);

    WriteCallback write_callback_;
    struct tsm_screen* screen_ = nullptr;
    struct tsm_vte* vte_ = nullptr;
    std::string error_;
    bool selection_active_ = false;
    TerminalMetrics metrics_;
    std::chrono::steady_clock::time_point last_output_time_;
    bool output_waiting_for_render_ = false;
};
