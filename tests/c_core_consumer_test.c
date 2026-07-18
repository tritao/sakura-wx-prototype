#include <sakura/terminal/core_c.h>

#include <stdio.h>
#include <string.h>

static void fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
}

static int check(int condition, const char *message)
{
    if (!condition)
        fail(message);
    return condition;
}

int main(void)
{
    SakuraTerminal *terminal = sakura_terminal_new(NULL, NULL);
    if (!check(terminal != NULL, "C API did not create a terminal"))
        return 1;
    if (!check(sakura_terminal_is_ready(terminal),
               "C API terminal was not ready")) {
        sakura_terminal_free(terminal);
        return 1;
    }
    if (!check(sakura_terminal_resize(terminal, 4, 1),
               "C API resize failed")) {
        sakura_terminal_free(terminal);
        return 1;
    }

    SakuraTerminalTheme theme;
    sakura_terminal_theme_default(&theme);
    theme.background[0] = 11;
    theme.background[1] = 22;
    theme.background[2] = 33;
    SakuraTerminal *themed = sakura_terminal_new_with_theme(
        NULL, NULL, &theme);
    if (!check(themed != NULL && sakura_terminal_resize(themed, 4, 1),
               "themed C API terminal setup failed")) {
        sakura_terminal_free(themed);
        sakura_terminal_free(terminal);
        return 1;
    }
    SakuraTerminalFrame *themed_frame = sakura_terminal_take_frame(themed);
    SakuraTerminalCellView themed_cell;
    if (!check(themed_frame != NULL &&
                   sakura_terminal_frame_cell(themed_frame, 1, 0,
                                              &themed_cell) &&
                   themed_cell.background[0] == 11 &&
                   themed_cell.background[1] == 22 &&
                   themed_cell.background[2] == 33,
               "themed C API background was not applied to blank cells")) {
        sakura_terminal_frame_free(themed_frame);
        sakura_terminal_free(themed);
        sakura_terminal_free(terminal);
        return 1;
    }
    sakura_terminal_frame_free(themed_frame);

    const char explicit_background[] = "\033[48;2;24;24;24mX";
    sakura_terminal_feed_output(themed, explicit_background,
                                sizeof(explicit_background) - 1);
    themed_frame = sakura_terminal_take_frame(themed);
    if (!check(themed_frame != NULL &&
                   sakura_terminal_frame_cell(themed_frame, 0, 0,
                                              &themed_cell) &&
                   themed_cell.background[0] == 24 &&
                   themed_cell.background[1] == 24 &&
                   themed_cell.background[2] == 24,
               "explicit #181818 background was confused with the default")) {
        sakura_terminal_frame_free(themed_frame);
        sakura_terminal_free(themed);
        sakura_terminal_free(terminal);
        return 1;
    }
    sakura_terminal_frame_free(themed_frame);
    sakura_terminal_free(themed);

    SakuraTerminalFrame *frame = sakura_terminal_take_frame(terminal);
    SakuraTerminalFrameInfo info;
    if (!check(frame != NULL && sakura_terminal_frame_info(frame, &info),
               "C API did not return frame information")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }
    if (!check(info.columns == 4 && info.rows == 1 && info.full_repaint &&
                   info.scroll_delta == 0 &&
                   info.scroll_kind == SAKURA_TERMINAL_SCROLL_NONE,
               "C API frame dimensions or repaint state were wrong")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }
    if (!check(sakura_terminal_frame_dirty_span_count(frame) == 1,
               "C API did not expose one full-row dirty span")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }

    SakuraTerminalDirtySpan span;
    if (!check(sakura_terminal_frame_dirty_span(frame, 0, &span) &&
                   span.row == 0 && span.left == 0 && span.right == 4,
               "C API dirty span was wrong")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }
    sakura_terminal_frame_free(frame);

    sakura_terminal_feed_output(terminal, "A", 1);
    frame = sakura_terminal_take_frame(terminal);
    if (!check(frame != NULL && sakura_terminal_frame_info(frame, &info) &&
                   info.changed,
               "C API did not expose changed output")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }

    SakuraTerminalCellView cell;
    if (!check(sakura_terminal_frame_cell(frame, 0, 0, &cell) &&
                   cell.codepoint == 'A' && cell.text_length == 1 &&
                   cell.text != NULL && cell.text[0] == 'A',
               "C API cell view was wrong")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }
    const size_t run_count = sakura_terminal_frame_row_run_count(frame, 0);
    if (!check(run_count == 3,
               "C API did not split the row at style boundaries")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }
    SakuraTerminalRunView run;
    if (!check(sakura_terminal_frame_row_run(frame, 0, 0, &run) &&
                   run.row == 0 && run.left == 0 && run.cell_count == 1 &&
                   run.text_length == 1 && run.text != NULL && run.text[0] == 'A',
               "C API first row run was wrong")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }
    if (!check(sakura_terminal_frame_row_run(frame, 0, 2, &run) &&
                   run.left == 2 && run.cell_count == 2 &&
                   run.text_length == 2 && run.text[0] == ' ' &&
                   run.text[1] == ' ',
               "C API coalesced row run was wrong")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }
    sakura_terminal_frame_free(frame);

    SakuraTerminal *span_terminal = sakura_terminal_new(NULL, NULL);
    if (!check(span_terminal != NULL &&
                   sakura_terminal_resize(span_terminal, 100, 1),
               "C API span terminal setup failed")) {
        sakura_terminal_free(span_terminal);
        sakura_terminal_free(terminal);
        return 1;
    }
    char span_text[128];
    size_t span_text_length = 0;
    for (unsigned int index = 0; index < 31; ++index)
        span_text[span_text_length++] = 'a';
    span_text[span_text_length++] = (char)0xe7;
    span_text[span_text_length++] = (char)0x95;
    span_text[span_text_length++] = (char)0x8c;
    for (unsigned int index = 0; index < 67; ++index)
        span_text[span_text_length++] = 'b';
    sakura_terminal_feed_output(span_terminal, span_text, span_text_length);
    SakuraTerminalFrame *span_frame = sakura_terminal_take_frame(span_terminal);
    if (!check(span_frame != NULL,
               "C API did not return bounded span frame")) {
        sakura_terminal_frame_free(span_frame);
        sakura_terminal_free(span_terminal);
        sakura_terminal_free(terminal);
        return 1;
    }
    const size_t logical_run_count =
        sakura_terminal_frame_row_run_count(span_frame, 0);
    unsigned int logical_next_left = 0;
    for (size_t index = 0; index < logical_run_count; ++index) {
        SakuraTerminalRunView logical_run;
        if (!check(sakura_terminal_frame_row_run(
                       span_frame, 0, index, &logical_run) &&
                       logical_run.left == logical_next_left &&
                       logical_run.cell_count > 0,
                   "C API logical run was not preserved")) {
            sakura_terminal_frame_free(span_frame);
            sakura_terminal_free(span_terminal);
            sakura_terminal_free(terminal);
            return 1;
        }
        logical_next_left += logical_run.cell_count;
    }
    if (!check(logical_run_count > 0 && logical_next_left == 100,
               "C API logical runs did not cover the row")) {
        sakura_terminal_frame_free(span_frame);
        sakura_terminal_free(span_terminal);
        sakura_terminal_free(terminal);
        return 1;
    }
    const size_t span_count = sakura_terminal_frame_row_span_count(
        span_frame, 0, SAKURA_TERMINAL_DEFAULT_RUN_SPAN_MAX_CELLS);
    unsigned int next_left = 0;
    for (size_t index = 0; index < span_count; ++index) {
        SakuraTerminalRunView span;
        if (!check(sakura_terminal_frame_row_span(
                       span_frame, 0, index,
                       SAKURA_TERMINAL_DEFAULT_RUN_SPAN_MAX_CELLS, &span) &&
                       span.left == next_left && span.cell_count > 0 &&
                       span.cell_count <=
                           SAKURA_TERMINAL_DEFAULT_RUN_SPAN_MAX_CELLS,
                   "C API emitted an invalid bounded run span")) {
            sakura_terminal_frame_free(span_frame);
            sakura_terminal_free(span_terminal);
            sakura_terminal_free(terminal);
            return 1;
        }
        SakuraTerminalCellView first_cell;
        SakuraTerminalCellView last_cell;
        if (!check(sakura_terminal_frame_cell(
                       span_frame, span.left, 0, &first_cell) &&
                       first_cell.width != 0 &&
                       sakura_terminal_frame_cell(
                           span_frame, span.left + span.cell_count - 1, 0,
                           &last_cell) && last_cell.width != 2,
                   "C API split a wide glyph at a run boundary")) {
            sakura_terminal_frame_free(span_frame);
            sakura_terminal_free(span_terminal);
            sakura_terminal_free(terminal);
            return 1;
        }
        next_left += span.cell_count;
    }
    if (!check(span_count >= 4 && next_left == 100,
               "C API bounded run spans did not cover the row")) {
        sakura_terminal_frame_free(span_frame);
        sakura_terminal_free(span_terminal);
        sakura_terminal_free(terminal);
        return 1;
    }
    const size_t narrow_span_count = sakura_terminal_frame_row_span_count(
        span_frame, 0, 16);
    if (!check(narrow_span_count > span_count,
               "C API span bound was not caller-configurable")) {
        sakura_terminal_frame_free(span_frame);
        sakura_terminal_free(span_terminal);
        sakura_terminal_free(terminal);
        return 1;
    }
    sakura_terminal_frame_free(span_frame);
    sakura_terminal_free(span_terminal);

    sakura_terminal_feed_output(terminal, "\033]2;c-api title\007", 16);
    frame = sakura_terminal_take_frame(terminal);
    const int title_frame_info_ok =
        frame != NULL && sakura_terminal_frame_info(frame, &info);
    if (!check(title_frame_info_ok && !info.changed,
               "title-only output unexpectedly changed the screen")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }
    if (!check(strcmp(sakura_terminal_title(terminal), "c-api title") == 0,
               "C API title was wrong")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }

    sakura_terminal_frame_free(frame);
    sakura_terminal_free(terminal);
    return 0;
}
