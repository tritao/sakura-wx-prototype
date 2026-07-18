#include <tsm/libtsm.h>

#include <stdint.h>
#include <stdio.h>

static int fail(const char* message)
{
    fprintf(stderr, "tsm_public_api: FAIL: %s\n", message);
    return 1;
}

int main(void)
{
    struct tsm_screen* screen = NULL;
    uint64_t before[3];
    uint64_t after_scroll[3];

    if (tsm_screen_new(&screen, NULL, NULL) != 0 || screen == NULL)
        return fail("could not create screen");
    tsm_screen_set_max_sb(screen, 16);
    if (tsm_screen_resize(screen, 4, 3) != 0)
        return fail("could not resize screen");

    for (unsigned int row = 0; row < 3; ++row) {
        before[row] = tsm_screen_get_row_id(screen, row);
        if (before[row] == 0) {
            tsm_screen_unref(screen);
            return fail("initial row identity was zero");
        }
    }

    tsm_screen_scroll_up(screen, 1);
    for (unsigned int row = 0; row < 3; ++row)
        after_scroll[row] = tsm_screen_get_row_id(screen, row);
    if (after_scroll[0] != before[1] || after_scroll[1] != before[2] ||
        after_scroll[2] == before[0]) {
        tsm_screen_unref(screen);
        return fail("row identities did not describe content scrolling");
    }

    tsm_screen_sb_up(screen, 1);
    if (tsm_screen_sb_get_line_pos(screen) >=
        tsm_screen_sb_get_line_count(screen) ||
        tsm_screen_get_row_id(screen, 0) != before[0]) {
        tsm_screen_unref(screen);
        return fail("row identity did not follow the scrollback viewport");
    }

    tsm_screen_sb_reset(screen);
    if (tsm_screen_sb_get_line_pos(screen) !=
        tsm_screen_sb_get_line_count(screen) ||
        tsm_screen_get_row_id(screen, 0) != after_scroll[0]) {
        tsm_screen_unref(screen);
        return fail("scrollback reset did not restore the live viewport");
    }

    tsm_screen_unref(screen);
    puts("tsm_public_api: PASS");
    return 0;
}
