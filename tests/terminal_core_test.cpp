#include "test_terminal.h"

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

const TestCell& CellAt(const TestSnapshot& snapshot,
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
        TestTerminal core([&writes](const char* data, std::size_t length) {
            writes.append(data, length);
        });
        Check(core.IsReady(), "terminal core failed to initialize");
        Check(core.Resize(40, 8), "terminal core resize failed");

        Check(core.HandleKey('a', 'a', 0, 'a'), "lowercase input was not handled");
        Check(writes == "a", "lowercase input was changed before reaching the transport");
        writes.clear();

        Check(core.HandleKey('A', 'A', SAKURA_TERMINAL_SHIFT, 'A'),
              "uppercase input was not handled");
        Check(writes == "A", "uppercase input was changed before reaching the transport");
        writes.clear();

        Check(core.HandleKey('c', 'c', SAKURA_TERMINAL_CONTROL, 'c'),
              "control input was not handled");
        Check(writes == std::string(1, '\x03'), "control-C was encoded incorrectly");
        writes.clear();

        Check(core.HandleKey(XKB_KEY_Up, XKB_KEY_NoSymbol, 0,
                             SAKURA_TERMINAL_INVALID),
              "arrow input was not handled");
        Check(writes == "\x1b[A", "arrow input was encoded incorrectly");

        const std::string screen_text = "\x1b[2J\x1b[Hhello";
        core.FeedOutput(screen_text.data(), screen_text.size());
        TestSnapshot snapshot = core.TakeSnapshot();
        Check(snapshot.columns == 40 && snapshot.rows == 8,
              "snapshot dimensions are incorrect");
        Check(CellAt(snapshot, 0, 0).codepoint == 'h',
              "plain text was not placed in the screen");
        Check(CellAt(snapshot, 4, 0).codepoint == 'o',
              "plain text placement is incorrect");

        TestTerminal frame_core(nullptr);
        Check(frame_core.Resize(10, 2), "dirty frame resize failed");
        uint64_t first_generation = 0;
        {
            const TestFrame first_frame = frame_core.TakeFrame();
            first_generation = first_frame.generation;
            Check(first_frame.generation == 1 && first_frame.changed &&
                      first_frame.full_repaint &&
                      first_frame.dirty.left == 0 && first_frame.dirty.top == 0 &&
                      first_frame.dirty.right == 10 && first_frame.dirty.bottom == 2,
                  "first frame did not request a full repaint");
        }
        Check(frame_core.GetMetrics().frame_cells_decoded == 20,
              "first frame did not decode every cell");
        {
            const TestFrame clean_frame = frame_core.TakeFrame();
            Check(clean_frame.generation == first_generation &&
                      !clean_frame.changed && clean_frame.dirty.IsEmpty(),
                  "unchanged frame was not reported as clean");
        }
        Check(frame_core.GetMetrics().frame_cells_reused >= 20,
              "unchanged frame did not reuse cached cells");
        frame_core.FeedOutput("abc", 3);
        uint64_t changed_generation = 0;
        {
            const TestFrame changed_frame = frame_core.TakeFrame();
            changed_generation = changed_frame.generation;
            Check(changed_frame.generation > first_generation &&
                      changed_frame.changed && !changed_frame.dirty.IsEmpty() &&
                      changed_frame.dirty.left == 0 &&
                      changed_frame.dirty.top == 0 &&
                      changed_frame.dirty.right >= 3,
                  "output did not produce a dirty frame");
        }
        Check(frame_core.GetMetrics().frame_cells_reused > 20,
              "dirty frame did not retain unchanged cells");
        const char* frame_title = "\x1b]2;frame title\x07";
        const uint64_t decoded_before_title =
            frame_core.GetMetrics().frame_cells_decoded;
        frame_core.FeedOutput(frame_title, std::strlen(frame_title));
        const TestFrame title_frame = frame_core.TakeFrame();
        Check(title_frame.generation == changed_generation &&
                  !title_frame.changed && title_frame.dirty.IsEmpty(),
              "title-only output dirtied the screen frame");
        Check(frame_core.GetMetrics().frame_cells_decoded == decoded_before_title,
              "title-only output decoded terminal cells");

        TestTerminal retained_frame_core(nullptr);
        Check(retained_frame_core.Resize(8, 2),
              "retained frame resize failed");
        const TestFrame retained_frame = retained_frame_core.TakeFrame();
        retained_frame_core.FeedOutput("a", 1);
        const TestFrame updated_retained_frame =
            retained_frame_core.TakeFrame();
        Check(retained_frame.snapshot != nullptr &&
                  updated_retained_frame.snapshot != nullptr &&
                  retained_frame.snapshot != updated_retained_frame.snapshot &&
                  CellAt(*retained_frame.snapshot, 0, 0).codepoint == ' ' &&
                  CellAt(*updated_retained_frame.snapshot, 0, 0).codepoint == 'a',
              "retained frame snapshot was mutated in place");

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

        TestTerminal alternate_core(nullptr);
        Check(alternate_core.Resize(20, 4), "alternate screen resize failed");
        alternate_core.FeedOutput("base", 4);
        alternate_core.FeedOutput("\x1b[?1049h", 8);
        alternate_core.FeedOutput("alt", 3);
        const TestSnapshot alternate_snapshot = alternate_core.TakeSnapshot();
        Check(alternate_snapshot.alternate_screen,
              "alternate screen mode was not detected");
        Check(CellAt(alternate_snapshot, 4, 0).codepoint == 'a',
              "alternate screen content was not isolated");
        alternate_core.FeedOutput("\x1b[?1049l", 8);
        const TestSnapshot main_snapshot = alternate_core.TakeSnapshot();
        Check(!main_snapshot.alternate_screen &&
                  CellAt(main_snapshot, 0, 0).codepoint == 'b',
              "main screen was not restored after alternate screen");

        std::string bracketed_writes;
        TestTerminal semantic_core([&bracketed_writes](const char* data,
                                                        std::size_t length) {
            bracketed_writes.append(data, length);
        });
        Check(semantic_core.Resize(20, 3), "semantic core resize failed");
        semantic_core.FeedOutput("\x1b[?2004h", 8);
        semantic_core.Paste("clip");
        Check(bracketed_writes == "\x1b[200~clip\x1b[201~",
              "bracketed paste was not encoded");

        TestTerminal style_core(nullptr);
        Check(style_core.Resize(20, 3), "style core resize failed");
        const char* style = "\x1b[38;2;1;2;3m\x1b[48;2;4;5;6m\x1b[1;3;4mX";
        style_core.FeedOutput(style, std::strlen(style));
        const TestSnapshot style_snapshot = style_core.TakeSnapshot();
        const TestCell& styled_cell = CellAt(style_snapshot, 0, 0);
        Check(styled_cell.foreground == std::array<uint8_t, 3>{1, 2, 3} &&
                  styled_cell.background == std::array<uint8_t, 3>{4, 5, 6},
              "truecolor was not preserved");
        Check((styled_cell.attributes & 0x01) != 0 &&
                  (styled_cell.attributes & 0x02) != 0 &&
                  (styled_cell.attributes & 0x04) != 0,
              "bold italic underline attributes were not preserved");

        TestTerminal cursor_core(nullptr);
        Check(cursor_core.Resize(20, 3), "cursor core resize failed");
        cursor_core.FeedOutput("\x1b[3", 3);
        cursor_core.FeedOutput(" q", 2);
        Check(cursor_core.TakeSnapshot().cursor_style ==
                  SAKURA_TERMINAL_CURSOR_UNDERLINE,
              "underline cursor style was not parsed");
        cursor_core.FeedOutput("\x1b[6 q", 5);
        Check(cursor_core.TakeSnapshot().cursor_style == SAKURA_TERMINAL_CURSOR_BAR,
              "bar cursor style was not parsed");
        Check(cursor_core.GetMetrics().cursor_style_changes == 2,
              "cursor style metric was not recorded");

        TestTerminal glyph_core(nullptr);
        Check(glyph_core.Resize(20, 3), "glyph core resize failed");
        glyph_core.TakeFrame();
        const char* glyphs = "\xe7\x95\x8c" "e\xcc\x81";
        glyph_core.FeedOutput(glyphs, std::strlen(glyphs));
        const TestSnapshot glyph_snapshot = glyph_core.TakeSnapshot();
        Check(CellAt(glyph_snapshot, 0, 0).text == "\xe7\x95\x8c" &&
                  CellAt(glyph_snapshot, 0, 0).width == 2,
              "wide glyph was not preserved");
        Check(CellAt(glyph_snapshot, 2, 0).text == "e\xcc\x81" &&
                  CellAt(glyph_snapshot, 2, 0).width == 1 &&
                  glyph_snapshot.cursor_x == 3,
              "combining glyph was not preserved as one terminal cell");

        TestTerminal combining_selection_core(nullptr);
        Check(combining_selection_core.Resize(20, 3),
              "combining selection core resize failed");
        combining_selection_core.FeedOutput("e\xcc\x81", 3);
        combining_selection_core.StartSelection(0, 0);
        combining_selection_core.UpdateSelection(0, 0);
        Check(combining_selection_core.CopySelection() == "e\xcc\x81",
              "combining mark was lost during selection copy");

        TestTerminal unicode_core(nullptr);
        Check(unicode_core.Resize(20, 4), "unicode core resize failed");
        unicode_core.FeedOutput("caf", 3);
        unicode_core.FeedOutput("\xc3", 1);
        unicode_core.FeedOutput("\xa9", 1);
        const TestSnapshot unicode_snapshot = unicode_core.TakeSnapshot();
        Check(CellAt(unicode_snapshot, 3, 0).codepoint == 0xE9,
              "split UTF-8 input was not reconstructed correctly");

        std::string paste_writes;
        TestTerminal selection_core([&paste_writes](const char* data,
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
        const SakuraTerminalMetrics metrics = selection_core.GetMetrics();
        Check(metrics.output_bytes == 9 && metrics.output_chunks == 1,
              "output metrics were not recorded");
        Check(metrics.render_latency_samples >= 1,
              "render latency was not recorded");
        Check(metrics.selection_copies == 1 && metrics.paste_bytes == 6,
              "selection metrics were not recorded");

        TestTerminal selection_frame_core(nullptr);
        Check(selection_frame_core.Resize(30, 6),
              "selection frame core resize failed");
        const char* selection_frame_text = "selection repaint range";
        selection_frame_core.FeedOutput(selection_frame_text,
                                         std::strlen(selection_frame_text));
        selection_frame_core.TakeFrame();
        selection_frame_core.StartSelection(0, 0);
        selection_frame_core.UpdateSelection(8, 0);
        const TestFrame selected_frame = selection_frame_core.TakeFrame();
        Check(selected_frame.changed && !selected_frame.full_repaint &&
                  selected_frame.dirty.top == 0 &&
                  selected_frame.dirty.bottom == 1 &&
                  selected_frame.dirty.left == 0 &&
                  selected_frame.dirty.right >= 9,
              "selection change requested a full repaint");
        selection_frame_core.ClearSelection();
        const TestFrame cleared_selection_frame =
            selection_frame_core.TakeFrame();
        Check(cleared_selection_frame.changed &&
                  !cleared_selection_frame.full_repaint &&
                  cleared_selection_frame.dirty.top == 0 &&
                  cleared_selection_frame.dirty.bottom == 1,
              "clearing selection requested a full repaint");
        selection_frame_core.StartSelection(0, 0);
        selection_frame_core.UpdateSelection(2, 1);
        const TestFrame multiline_selection_frame =
            selection_frame_core.TakeFrame();
        Check(multiline_selection_frame.changed &&
                  !multiline_selection_frame.full_repaint &&
                  multiline_selection_frame.dirty.top == 0 &&
                  multiline_selection_frame.dirty.bottom == 2,
              "multiline selection requested a full repaint");

        TestTerminal selection_behavior_core(nullptr);
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

        TestTerminal unicode_selection_core(nullptr);
        Check(unicode_selection_core.Resize(20, 3),
              "unicode selection core resize failed");
        unicode_selection_core.FeedOutput("caf\xc3\xa9", 5);
        unicode_selection_core.SelectWord(3, 0);
        Check(unicode_selection_core.CopySelection() == "caf\xC3\xA9",
              "Unicode word selection returned unexpected text");

        std::string mouse_writes;
        TestTerminal mouse_core([&mouse_writes](const char* data,
                                                 std::size_t length) {
            mouse_writes.append(data, length);
        });
        Check(mouse_core.Resize(20, 4), "mouse core resize failed");
        const char* enable_mouse = "\x1b[?1000h\x1b[?1006h";
        mouse_core.FeedOutput(enable_mouse, std::strlen(enable_mouse));
        Check(mouse_core.MouseReportingEnabled(),
              "mouse reporting mode was not detected");
        Check(mouse_core.HandleMouse(2, 3, 20, 30, SAKURA_TERMINAL_MOUSE_LEFT,
                                     SAKURA_TERMINAL_MOUSE_PRESSED,
                                     SAKURA_TERMINAL_MOUSE_SHIFT),
              "mouse press was not forwarded");
        Check(mouse_writes == "\x1b[<4;3;4M",
              "mouse press was encoded incorrectly");
        Check(mouse_core.HandleMouse(2, 3, 20, 30, SAKURA_TERMINAL_MOUSE_LEFT,
                                     SAKURA_TERMINAL_MOUSE_RELEASED, 0),
              "mouse release was not forwarded");
        Check(mouse_writes == "\x1b[<4;3;4M\x1b[<0;3;4m",
              "mouse release was encoded incorrectly");
        const SakuraTerminalMetrics mouse_metrics = mouse_core.GetMetrics();
        Check(mouse_metrics.mouse_events == 2 &&
                  mouse_metrics.mouse_events_forwarded == 2 &&
                  mouse_metrics.mouse_mode_changes >= 2,
              "mouse metrics were not recorded");

        TestTerminal scroll_core(nullptr);
        Check(scroll_core.Resize(20, 3), "scroll core resize failed");
        std::string scroll_output;
        for (unsigned int line = 0; line < 8; ++line) {
            scroll_output += "scroll-" + std::to_string(line) + "\r\n";
        }
        scroll_core.FeedOutput(scroll_output.data(), scroll_output.size());
        const TestSnapshot bottom_snapshot = scroll_core.TakeSnapshot();
        scroll_core.ScrollLines(1);
        const TestSnapshot up_snapshot = scroll_core.TakeSnapshot();
        Check(bottom_snapshot.cells[7].codepoint != up_snapshot.cells[7].codepoint,
              "scrolling one line did not change the visible screen");
        scroll_core.ScrollLines(-1);
        const TestSnapshot restored_snapshot = scroll_core.TakeSnapshot();
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
