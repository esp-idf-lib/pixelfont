/*
 * Draws on an ILI9341 TFT through espressif/esp_lcd_ili9341.
 *
 * The panel takes 16-bit colour pixels, so the one-bit blocks that pixelfont
 * produces are expanded here: a set pixel becomes the foreground colour, a
 * clear one the background. The expansion is what any colour panel needs, and
 * it costs one line buffer of the block width.
 */

#include <string.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_ili9341.h>
#include <pixelfont.h>
#include <pixelfont_fonts.h>

static const char *TAG = "example";

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 320
#define LCD_V_RES 240

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xffff
#define COLOR_BLUE 0x001f
#define COLOR_YELLOW 0xffe0

typedef struct
{
    esp_lcd_panel_handle_t panel;
    uint16_t *line;
    uint16_t fg;
    uint16_t bg;
} tft_t;

/*
 * Expands one block of one-bit pixels into 16-bit colour and sends it to the
 * panel. The block is in the row packing, so the pixels of a row are packed
 * into bytes from the most significant bit.
 */
static esp_err_t flush(void *ctx, int x, int y, int w, int h, const uint8_t *bits)
{
    tft_t *tft = ctx;
    const int stride = (w + 7) / 8;

    for (int row = 0; row < h; row++)
    {
        const uint8_t *src = bits + row * stride;
        uint16_t *dst = tft->line;

        for (int col = 0; col < w; col++)
            *dst++ = src[col >> 3] & (0x80 >> (col & 7)) ? tft->fg : tft->bg;

        esp_err_t err = esp_lcd_panel_draw_bitmap(tft->panel, x, y + row, x + w, y + row + 1,
                                                  tft->line);
        if (err != ESP_OK)
            return err;
    }

    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus");
    const spi_bus_config_t bus_config = ILI9341_PANEL_BUS_SPI_CONFIG(
                                            CONFIG_EXAMPLE_LCD_PIN_PCLK, CONFIG_EXAMPLE_LCD_PIN_MOSI, LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = ILI9341_PANEL_IO_SPI_CONFIG(
                                                        CONFIG_EXAMPLE_LCD_PIN_CS, CONFIG_EXAMPLE_LCD_PIN_DC, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));

    ESP_LOGI(TAG, "Install the panel driver");
    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_config =
    {
        .reset_gpio_num = CONFIG_EXAMPLE_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    tft_t tft = { .panel = panel, .fg = COLOR_WHITE, .bg = COLOR_BLACK, };
    tft.line = malloc(LCD_H_RES * sizeof(uint16_t));
    if (!tft.line)
    {
        ESP_LOGE(TAG, "Out of memory");
        return;
    }

    // clear the screen once
    for (int i = 0; i < LCD_H_RES; i++)
        tft.line[i] = COLOR_BLACK;
    for (int y = 0; y < LCD_V_RES; y++)
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_H_RES, y + 1, tft.line));

    pixelfont_canvas_t canvas =
    {
        .width = LCD_H_RES,
        .height = LCD_V_RES,
        .pack = PIXELFONT_PACK_ROW,
        .flush = flush,
        .ctx = &tft,
    };

    pixelfont_style_t style = PIXELFONT_STYLE_DEFAULT;

    while (true)
    {
        ESP_LOGI(TAG, "Draw the frame");
        style.scale = 3;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 10, 10, "Pixel", &style));

        tft.fg = COLOR_YELLOW;
        style.scale = 2;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 10, 70, "on ILI9341", &style));

        tft.fg = COLOR_WHITE;
        style.scale = 1;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 10, 120,
                                            "One bit per pixel, expanded here", &style));

        // inverse video: the cell is filled, the glyph leaves holes in it
        tft.fg = COLOR_BLUE;
        style.opaque = true;
        style.reversed = true;
        style.scale = 2;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 10, 150, "inverse", &style));
        style = (pixelfont_style_t)PIXELFONT_STYLE_DEFAULT;

        // text that leaves the screen is cut by the edge
        tft.fg = COLOR_WHITE;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, LCD_H_RES - 60, 200,
                                            "and the right edge", &style));


        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "Done");
}
