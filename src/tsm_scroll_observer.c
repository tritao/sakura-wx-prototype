#include "tsm_scroll_observer.h"

#include <tsm/libtsm-int.h>

unsigned int sakura_tsm_screen_height(const struct tsm_screen* screen)
{
    return screen == NULL ? 0 : screen->size_y;
}

const void* sakura_tsm_screen_line(const struct tsm_screen* screen,
                                   unsigned int row)
{
    if (screen == NULL || screen->lines == NULL || row >= screen->size_y)
        return NULL;
    return screen->lines[row];
}

int sakura_tsm_screen_is_alternate(const struct tsm_screen* screen)
{
    return screen != NULL && (screen->flags & TSM_SCREEN_ALTERNATE) != 0;
}

int sakura_tsm_screen_is_scrollback(const struct tsm_screen* screen)
{
    return screen != NULL && screen->sb.pos != NULL;
}
