/**
 * @file pixelfont_fonts.h
 *
 * The fonts bundled with the component.
 *
 * A font is available only when its name is in the Kconfig list of the
 * bundled fonts, so every declaration here is guarded by a definition that
 * the build sets.
 */

#pragma once

#include <pixelfont.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PIXELFONT_FONT_TERMINUS_8X16_ENABLED
/**
 * @brief Terminus, 8x16, Latin, Greek, Cyrillic, box drawing
 */
extern const pixelfont_t pixelfont_font_terminus_8x16;
#endif

#ifdef PIXELFONT_FONT_TERMINUS_12X24_ENABLED
/**
 * @brief Terminus, 12x24, Latin, Greek, Cyrillic, box drawing
 */
extern const pixelfont_t pixelfont_font_terminus_12x24;
#endif

#if defined(CONFIG_PIXELFONT_DEFAULT_TERMINUS_8X16) \
        && defined(PIXELFONT_FONT_TERMINUS_8X16_ENABLED)
#define PIXELFONT_FONT_DEFAULT (&pixelfont_font_terminus_8x16)
#elif defined(CONFIG_PIXELFONT_DEFAULT_TERMINUS_12X24) \
        && defined(PIXELFONT_FONT_TERMINUS_12X24_ENABLED)
#define PIXELFONT_FONT_DEFAULT (&pixelfont_font_terminus_12x24)
#else
#error "The default font is not in the list of the bundled fonts"
#endif

#ifdef __cplusplus
}
#endif
