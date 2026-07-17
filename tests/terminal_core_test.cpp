#include "terminal_core.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const TerminalCell& CellAt(const TerminalSnapshot& snapshot,
                           unsigned int column, unsigned int row)
{
    Check(column < snapshot.columns && row < snapshot.rows,
          "cell coordinate is outside the snapshot");
    return snapshot.cells[row * snapshot.columns + column];
}

} // namespace

int main()
{
    try {
        std::string writes;
        TerminalCore core([&writes](const char* data, std::size_t length) {
            writes.append(data, length);
        });
        Check(core.IsReady(), "terminal core failed to initialize");
        Check(core.Resize(40, 8), "terminal core resize failed");

        Check(core.HandleKey('a', 'a', 0, 'a'), "lowercase input was not handled");
        Check(writes == "a", "lowercase input was changed before reaching the transport");
        writes.clear();

        Check(core.HandleKey('A', 'A', TSM_SHIFT_MASK, 'A'),
              "uppercase input was not handled");
        Check(writes == "A", "uppercase input was changed before reaching the transport");
        writes.clear();

        Check(core.HandleKey('c', 'c', TSM_CONTROL_MASK, 'c'),
              "control input was not handled");
        Check(writes == std::string(1, '\x03'), "control-C was encoded incorrectly");
        writes.clear();

        Check(core.HandleKey(XKB_KEY_Up, XKB_KEY_NoSymbol, 0, TSM_VTE_INVALID),
              "arrow input was not handled");
        Check(writes == "\x1b[A", "arrow input was encoded incorrectly");

        const std::string screen_text = "\x1b[2J\x1b[Hhello";
        core.FeedOutput(screen_text.data(), screen_text.size());
        TerminalSnapshot snapshot = core.TakeSnapshot();
        Check(snapshot.columns == 40 && snapshot.rows == 8,
              "snapshot dimensions are incorrect");
        Check(CellAt(snapshot, 0, 0).codepoint == 'h',
              "plain text was not placed in the screen");
        Check(CellAt(snapshot, 4, 0).codepoint == 'o',
              "plain text placement is incorrect");

        const std::string styled_text = "\r\n\x1b[4munder\x1b[0m";
        core.FeedOutput(styled_text.data(), styled_text.size());
        snapshot = core.TakeSnapshot();
        Check(CellAt(snapshot, 0, 1).codepoint == 'u',
              "line movement was not parsed correctly");
        Check((CellAt(snapshot, 0, 1).attributes & 0x04) != 0,
              "underline attribute was not preserved");

        TerminalCore unicode_core(nullptr);
        Check(unicode_core.Resize(20, 4), "unicode core resize failed");
        unicode_core.FeedOutput("caf", 3);
        unicode_core.FeedOutput("\xc3", 1);
        unicode_core.FeedOutput("\xa9", 1);
        const TerminalSnapshot unicode_snapshot = unicode_core.TakeSnapshot();
        Check(CellAt(unicode_snapshot, 3, 0).codepoint == 0xE9,
              "split UTF-8 input was not reconstructed correctly");

        std::string paste_writes;
        TerminalCore selection_core([&paste_writes](const char* data,
                                                     std::size_t length) {
            paste_writes.append(data, length);
        });
        Check(selection_core.Resize(20, 4), "selection core resize failed");
        selection_core.FeedOutput("select me", 9);
        selection_core.TakeSnapshot();
        selection_core.StartSelection(0, 0);
        selection_core.UpdateSelection(8, 0);
        const std::string copied = selection_core.CopySelection();
        Check(copied == "select me", "selection copy returned unexpected text");
        selection_core.Paste("pasted");
        Check(paste_writes == "pasted", "paste was not sent to the transport");
        const TerminalMetrics metrics = selection_core.GetMetrics();
        Check(metrics.output_bytes == 9 && metrics.output_chunks == 1,
              "output metrics were not recorded");
        Check(metrics.render_latency_samples >= 1,
              "render latency was not recorded");
        Check(metrics.selection_copies == 1 && metrics.paste_bytes == 6,
              "selection metrics were not recorded");

        std::cout << "terminal_core: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terminal_core: FAIL: " << error.what() << '\n';
        return 1;
    }
}
