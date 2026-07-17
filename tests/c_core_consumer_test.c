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

    SakuraTerminalFrame *frame = sakura_terminal_take_frame(terminal);
    SakuraTerminalFrameInfo info;
    if (!check(frame != NULL && sakura_terminal_frame_info(frame, &info),
               "C API did not return frame information")) {
        sakura_terminal_frame_free(frame);
        sakura_terminal_free(terminal);
        return 1;
    }
    if (!check(info.columns == 4 && info.rows == 1 && info.full_repaint,
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
