#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class TerminalCursorStyle {
    Block,
    Underline,
    Bar,
};

enum TerminalKeyModifier : unsigned int {
    TerminalShift = 1u << 0,
    TerminalControl = 1u << 2,
    TerminalAlt = 1u << 3,
    TerminalLogo = 1u << 4,
};

constexpr uint32_t TerminalInvalid = 0xffffffffu;

enum TerminalMouseButton : unsigned int {
    TerminalMouseLeft = 0,
    TerminalMouseMiddle = 1,
    TerminalMouseRight = 2,
    TerminalMouseWheelUp = 4,
    TerminalMouseWheelDown = 5,
};

enum TerminalMouseModifier : unsigned char {
    TerminalMouseShift = 4,
    TerminalMouseMeta = 8,
    TerminalMouseControl = 16,
};

enum TerminalMouseEvent : unsigned int {
    TerminalMousePressed = 1,
    TerminalMouseReleased = 2,
    TerminalMouseMoved = 4,
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

    // TerminalCore is single-thread-affine: construct it and call all methods
    // from the same thread. The write callback runs synchronously on that
    // thread and must not re-enter TerminalCore.
    explicit TerminalCore(WriteCallback write_callback);
    ~TerminalCore();

    TerminalCore(const TerminalCore&) = delete;
    TerminalCore& operator=(const TerminalCore&) = delete;
    TerminalCore(TerminalCore&&) noexcept;
    TerminalCore& operator=(TerminalCore&&) noexcept;

    bool IsReady() const;
    const std::string& Error() const;
    const std::string& Title() const;

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
    bool HasSelection() const;
    std::string CopySelection();

    TerminalSnapshot TakeSnapshot();
    TerminalMetrics GetMetrics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
