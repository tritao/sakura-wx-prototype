#pragma once

#include <array>
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
    void ScrollPageUp(unsigned int pages);
    void ScrollPageDown(unsigned int pages);

    TerminalSnapshot TakeSnapshot();

private:
    static void VteWrite(struct tsm_vte* vte, const char* data,
                         std::size_t length, void* user_data);

    WriteCallback write_callback_;
    struct tsm_screen* screen_ = nullptr;
    struct tsm_vte* vte_ = nullptr;
    std::string error_;
};
