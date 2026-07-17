#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <tsm/libtsm.h>

enum class TerminalCursorStyle {
    Block,
    Underline,
    Bar,
};

struct TerminalCell {
    uint32_t codepoint = ' ';
    std::string text;
    unsigned int width = 1;
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
    TerminalCursorStyle cursor_style = TerminalCursorStyle::Block;
    bool alternate_screen = false;
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
    uint64_t mouse_mode_changes = 0;
    uint64_t mouse_events = 0;
    uint64_t mouse_events_forwarded = 0;
    uint64_t title_changes = 0;
    uint64_t cursor_style_changes = 0;
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
    const std::string& Title() const { return title_; }

    bool Resize(unsigned int columns, unsigned int rows);
    void FeedOutput(const char* data, std::size_t length);
    bool HandleKey(uint32_t keysym, uint32_t ascii, unsigned int modifiers,
                   uint32_t unicode);
    void Paste(const std::string& text);
    bool HandleMouse(unsigned int cell_x, unsigned int cell_y,
                     unsigned int pixel_x, unsigned int pixel_y,
                     unsigned int button, unsigned int event,
                     unsigned char modifiers);
    bool MouseReportingEnabled() const;
    void ScrollPageUp(unsigned int pages);
    void ScrollPageDown(unsigned int pages);
    void ScrollLines(int lines);

    void StartSelection(unsigned int column, unsigned int row);
    void UpdateSelection(unsigned int column, unsigned int row);
    void SelectWord(unsigned int column, unsigned int row);
    void SelectLine(unsigned int row);
    void ClearSelection();
    bool HasSelection() const { return selection_active_; }
    std::string CopySelection();

    TerminalSnapshot TakeSnapshot();
    TerminalMetrics GetMetrics() const { return metrics_; }

private:
    static void VteWrite(struct tsm_vte* vte, const char* data,
                         std::size_t length, void* user_data);
    static void VteMouse(struct tsm_vte* vte,
                         enum tsm_mouse_track_mode track_mode,
                         bool track_pixels, void* user_data);
    static int DrawCell(struct tsm_screen* screen, uint64_t id,
                        const uint32_t* codepoints, std::size_t length,
                        unsigned int width, unsigned int column,
                        unsigned int row, const struct tsm_screen_attr* attr,
                        tsm_age_t age, void* user_data);
    static void VteOsc(struct tsm_vte* vte, const char* data,
                       std::size_t length, void* user_data);
    void TrackCursorStyle(const char* data, std::size_t length);

    enum class CursorSequenceState {
        Ground,
        Escape,
        Csi,
        CsiIntermediate,
    } cursor_sequence_state_ = CursorSequenceState::Ground;
    std::string cursor_sequence_parameters_;

    WriteCallback write_callback_;
    struct tsm_screen* screen_ = nullptr;
    struct tsm_vte* vte_ = nullptr;
    std::string error_;
    std::string title_;
    TerminalCursorStyle cursor_style_ = TerminalCursorStyle::Block;
    bool selection_active_ = false;
    TerminalMetrics metrics_;
    std::chrono::steady_clock::time_point last_output_time_;
    bool output_waiting_for_render_ = false;
};
