/*
 * The example draws a frame on a buffered surface and prints it as text.
 *
 * The surface imitates the GDDRAM of a SSD1306, so the drawing code is the
 * same as it would be with a real display: the pixels go into the buffer of
 * the driver, and the driver sends the buffer when it likes.
 */

#include <stdio.h>
#include <string.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pixelfont.h>
#include <pixelfont_fonts.h>

static const char *TAG = "example";

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

// One page of eight rows is a byte per column, bit 0 is the topmost pixel:
// the format of the SSD1306 GDDRAM.
static uint8_t framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8];

// An 8x8 bitmap: a smiley, one byte per row
static const uint8_t smiley[] =
{
    0x3c, 0x42, 0xa5, 0x81, 0xa5, 0x99, 0x42, 0x3c,
};

static unsigned count_pixels(void)
{
    unsigned count = 0;

    for (size_t i = 0; i < sizeof(framebuffer); i++)
        for (int bit = 0; bit < 8; bit++)
            if (framebuffer[i] & (1 << bit))
                count++;

    return count;
}

/*
 * Prints the buffer as text, two pixels per character, so the whole 128x64
 * frame fits into 64 lines of 64 columns.
 */
static void dump(void)
{
    char line[DISPLAY_WIDTH / 2 + 1];

    for (int y = 0; y < DISPLAY_HEIGHT; y++)
    {
        for (int x = 0; x < DISPLAY_WIDTH / 2; x++)
        {
            const int left = framebuffer[(y / 8) * DISPLAY_WIDTH + x * 2] & (1 << (y & 7));
            const int right = framebuffer[(y / 8) * DISPLAY_WIDTH + x * 2 + 1] & (1 << (y & 7));
            line[x] = left && right ? '#' : left || right ? '+' : '.';
        }
        line[DISPLAY_WIDTH / 2] = 0;
        printf("%s\n", line);
    }
}

void app_main(void)
{
    pixelfont_canvas_t canvas =
    {
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .pack = PIXELFONT_PACK_PAGE,
        .data = framebuffer,
        .size = sizeof(framebuffer),
    };

    while (true)
    {
        pixelfont_style_t style = PIXELFONT_STYLE_DEFAULT;

        // the frame
        ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, 0, 0, DISPLAY_WIDTH, 1, true));
        ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, 0, DISPLAY_HEIGHT - 1, DISPLAY_WIDTH, 1, true));
        ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, 0, 0, 1, DISPLAY_HEIGHT, true));
        ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, DISPLAY_WIDTH - 1, 0, 1, DISPLAY_HEIGHT, true));

        // the title, twice the size
        style.scale = 2;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 8, 4, "Pixel", &style));

        // plain text, the default font
        style.scale = 1;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 8, 38,
                                            "Fonts as data", &style));

        // a bitmap, twice the size, over a filled spot
        ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, 8, 44, 32, 16, true));
        ESP_ERROR_CHECK(pixelfont_draw_bitmap(&canvas, 104, 44, 8, 8, smiley, 2, true, false));

        // inverse video: the cell is filled, the glyph leaves holes in it
        style.opaque = true;
        style.reversed = true;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 48, 44, "INV", &style));
        style = (pixelfont_style_t)PIXELFONT_STYLE_DEFAULT;

        printf("\n");
        dump();

        // the same frame with every pixel inverted
        ESP_LOGI(TAG, "The same frame, inverted");
        ESP_ERROR_CHECK(pixelfont_invert(&canvas));

        printf("\n");
        dump();

        ESP_ERROR_CHECK(pixelfont_invert(&canvas));

        // text that leaves the frame is cut by the edges, not dropped
        ESP_LOGI(TAG, "Text that leaves the frame");
        ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, 1, 1, DISPLAY_WIDTH - 2, DISPLAY_HEIGHT - 2, false));
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, -6, 20,
                                            "Off the left edge", &style));
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, DISPLAY_WIDTH - 22, 38,
                                            "and the right", &style));

        style.scale = 2;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 16, DISPLAY_HEIGHT - 20,
                                            "bottom", &style));
        style = (pixelfont_style_t)PIXELFONT_STYLE_DEFAULT;

        printf("\n");
        dump();

        // the same frame with the second font, when it is bundled
#ifdef PIXELFONT_FONT_TERMINUS_12X24_ENABLED
        ESP_LOGI(TAG, "The same frame with Terminus 12x24");

        ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, 1, 1, DISPLAY_WIDTH - 2, DISPLAY_HEIGHT - 2, false));
        ESP_ERROR_CHECK(pixelfont_draw_text(&pixelfont_font_terminus_12x24, &canvas, 6, 6, "Pixelfont", &style));

        printf("\n");
        dump();
#else
        ESP_LOGI(TAG, "Add terminus_12x24 to CONFIG_PIXELFONT_FONTS to see the second font");
#endif


        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    printf("\nDone. %u pixels set in %u bytes.\n", count_pixels(), (unsigned)canvas.size);
}
