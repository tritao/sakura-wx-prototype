#ifndef SAKURA_TSM_SCROLL_OBSERVER_H
#define SAKURA_TSM_SCROLL_OBSERVER_H

#include <tsm/libtsm.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned int sakura_tsm_screen_height(const struct tsm_screen* screen);
const void* sakura_tsm_screen_line(const struct tsm_screen* screen,
                                   unsigned int row);
int sakura_tsm_screen_is_alternate(const struct tsm_screen* screen);
int sakura_tsm_screen_is_scrollback(const struct tsm_screen* screen);

#ifdef __cplusplus
}
#endif

#endif
