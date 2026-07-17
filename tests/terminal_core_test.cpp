#include <sakura/terminal/core.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

        Check(core.HandleKey('A', 'A', TerminalShift, 'A'),
              "uppercase input was not handled");
        Check(writes == "A", "uppercase input was changed before reaching the transport");
        writes.clear();

        Check(core.HandleKey('c', 'c', TerminalControl, 'c'),
              "control input was not handled");
        Check(writes == std::string(1, '\x03'), "control-C was encoded incorrectly");
        writes.clear();

        Check(core.HandleKey(XKB_KEY_Up, XKB_KEY_NoSymbol, 0, TerminalInvalid),
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

        core.FeedOutput("\x1b]2;semantic title\x07", 19);
        Check(core.Title() == "semantic title", "OSC title was not captured");
        Check(core.GetMetrics().title_changes == 1,
              "OSC title metric was not recorded");
        Check(core.Resize(12, 3), "semantic resize failed");
        snapshot = core.TakeSnapshot();
        Check(snapshot.columns == 12 && snapshot.rows == 3,
              "semantic resize dimensions are incorrect");

        TerminalCore alternate_core(nullptr);
        Check(alternate_core.Resize(20, 4), "alternate screen resize failed");
        alternate_core.FeedOutput("base", 4);
        alternate_core.FeedOutput("\x1b[?1049h", 8);
        alternate_core.FeedOutput("alt", 3);
        const TerminalSnapshot alternate_snapshot = alternate_core.TakeSnapshot();
        Check(alternate_snapshot.alternate_screen,
              "alternate screen mode was not detected");
        Check(CellAt(alternate_snapshot, 4, 0).codepoint == 'a',
              "alternate screen content was not isolated");
        alternate_core.FeedOutput("\x1b[?1049l", 8);
        const TerminalSnapshot main_snapshot = alternate_core.TakeSnapshot();
        Check(!main_snapshot.alternate_screen &&
                  CellAt(main_snapshot, 0, 0).codepoint == 'b',
              "main screen was not restored after alternate screen");

        std::string bracketed_writes;
        TerminalCore semantic_core([&bracketed_writes](const char* data,
                                                        std::size_t length) {
            bracketed_writes.append(data, length);
        });
        Check(semantic_core.Resize(20, 3), "semantic core resize failed");
        semantic_core.FeedOutput("\x1b[?2004h", 8);
        semantic_core.Paste("clip");
        Check(bracketed_writes == "\x1b[200~clip\x1b[201~",
              "bracketed paste was not encoded");

        TerminalCore style_core(nullptr);
        Check(style_core.Resize(20, 3), "style core resize failed");
        const char* style = "\x1b[38;2;1;2;3m\x1b[48;2;4;5;6m\x1b[1;3;4mX";
        style_core.FeedOutput(style, std::strlen(style));
        const TerminalSnapshot style_snapshot = style_core.TakeSnapshot();
        const TerminalCell& styled_cell = CellAt(style_snapshot, 0, 0);
        Check(styled_cell.foreground == std::array<uint8_t, 3>{1, 2, 3} &&
                  styled_cell.background == std::array<uint8_t, 3>{4, 5, 6},
              "truecolor was not preserved");
        Check((styled_cell.attributes & 0x01) != 0 &&
                  (styled_cell.attributes & 0x02) != 0 &&
                  (styled_cell.attributes & 0x04) != 0,
              "bold italic underline attributes were not preserved");

        TerminalCore cursor_core(nullptr);
        Check(cursor_core.Resize(20, 3), "cursor core resize failed");
        cursor_core.FeedOutput("\x1b[3", 3);
        cursor_core.FeedOutput(" q", 2);
        Check(cursor_core.TakeSnapshot().cursor_style ==
                  TerminalCursorStyle::Underline,
              "underline cursor style was not parsed");
        cursor_core.FeedOutput("\x1b[6 q", 5);
        Check(cursor_core.TakeSnapshot().cursor_style == TerminalCursorStyle::Bar,
              "bar cursor style was not parsed");
        Check(cursor_core.GetMetrics().cursor_style_changes == 2,
              "cursor style metric was not recorded");

        TerminalCore glyph_core(nullptr);
        Check(glyph_core.Resize(20, 3), "glyph core resize failed");
        const char* glyphs = "\xe7\x95\x8c" "e\xcc\x81";
        glyph_core.FeedOutput(glyphs, std::strlen(glyphs));
        const TerminalSnapshot glyph_snapshot = glyph_core.TakeSnapshot();
        Check(CellAt(glyph_snapshot, 0, 0).text == "\xe7\x95\x8c" &&
                  CellAt(glyph_snapshot, 0, 0).width == 2,
              "wide glyph was not preserved");
        Check(CellAt(glyph_snapshot, 2, 0).text == "e\xcc\x81" &&
                  CellAt(glyph_snapshot, 2, 0).width == 1 &&
                  glyph_snapshot.cursor_x == 3,
              "combining glyph was not preserved as one terminal cell");

        TerminalCore combining_selection_core(nullptr);
        Check(combining_selection_core.Resize(20, 3),
              "combining selection core resize failed");
        combining_selection_core.FeedOutput("e\xcc\x81", 3);
        combining_selection_core.StartSelection(0, 0);
        combining_selection_core.UpdateSelection(0, 0);
        Check(combining_selection_core.CopySelection() == "e\xcc\x81",
              "combining mark was lost during selection copy");

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

        TerminalCore selection_behavior_core(nullptr);
        Check(selection_behavior_core.Resize(30, 6),
              "selection behavior core resize failed");
        selection_behavior_core.FeedOutput("one\r\ntwo", 8);
        selection_behavior_core.StartSelection(0, 0);
        selection_behavior_core.UpdateSelection(2, 1);
        Check(selection_behavior_core.CopySelection() == "one\ntwo",
              "multiline selection returned unexpected text");
        selection_behavior_core.SelectWord(1, 0);
        Check(selection_behavior_core.CopySelection() == "one",
              "word selection returned unexpected text");
        selection_behavior_core.SelectLine(1);
        Check(selection_behavior_core.CopySelection() == "two",
              "line selection returned unexpected text");

        TerminalCore unicode_selection_core(nullptr);
        Check(unicode_selection_core.Resize(20, 3),
              "unicode selection core resize failed");
        unicode_selection_core.FeedOutput("caf\xc3\xa9", 5);
        unicode_selection_core.SelectWord(3, 0);
        Check(unicode_selection_core.CopySelection() == "caf\xC3\xA9",
              "Unicode word selection returned unexpected text");

        std::string mouse_writes;
        TerminalCore mouse_core([&mouse_writes](const char* data,
                                                 std::size_t length) {
            mouse_writes.append(data, length);
        });
        Check(mouse_core.Resize(20, 4), "mouse core resize failed");
        const char* enable_mouse = "\x1b[?1000h\x1b[?1006h";
        mouse_core.FeedOutput(enable_mouse, std::strlen(enable_mouse));
        Check(mouse_core.MouseReportingEnabled(),
              "mouse reporting mode was not detected");
        Check(mouse_core.HandleMouse(2, 3, 20, 30, TerminalMouseLeft,
                                     TerminalMousePressed,
                                     TerminalMouseShift),
              "mouse press was not forwarded");
        Check(mouse_writes == "\x1b[<4;3;4M",
              "mouse press was encoded incorrectly");
        Check(mouse_core.HandleMouse(2, 3, 20, 30, TerminalMouseLeft,
                                     TerminalMouseReleased, 0),
              "mouse release was not forwarded");
        Check(mouse_writes == "\x1b[<4;3;4M\x1b[<0;3;4m",
              "mouse release was encoded incorrectly");
        const TerminalMetrics mouse_metrics = mouse_core.GetMetrics();
        Check(mouse_metrics.mouse_events == 2 &&
                  mouse_metrics.mouse_events_forwarded == 2 &&
                  mouse_metrics.mouse_mode_changes >= 2,
              "mouse metrics were not recorded");

        TerminalCore scroll_core(nullptr);
        Check(scroll_core.Resize(20, 3), "scroll core resize failed");
        std::string scroll_output;
        for (unsigned int line = 0; line < 8; ++line) {
            scroll_output += "scroll-" + std::to_string(line) + "\r\n";
        }
        scroll_core.FeedOutput(scroll_output.data(), scroll_output.size());
        const TerminalSnapshot bottom_snapshot = scroll_core.TakeSnapshot();
        scroll_core.ScrollLines(1);
        const TerminalSnapshot up_snapshot = scroll_core.TakeSnapshot();
        Check(bottom_snapshot.cells[7].codepoint != up_snapshot.cells[7].codepoint,
              "scrolling one line did not change the visible screen");
        scroll_core.ScrollLines(-1);
        const TerminalSnapshot restored_snapshot = scroll_core.TakeSnapshot();
        Check(restored_snapshot.cells[7].codepoint ==
                  bottom_snapshot.cells[7].codepoint,
              "scrolling back down did not restore the visible screen");

        std::cout << "terminal_core: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terminal_core: FAIL: " << error.what() << '\n';
        return 1;
    }
}
