/*
 * Loads a font at run time and draws a big clock with it.
 *
 * The font is not a part of the application: it lives in the file system of
 * the storage partition as a BDF, made from the Spleen font by
 * tools/pixelfont.py with the digits only. The application mounts the file
 * system, loads the font, and can drop it when the screen shows something
 * else, so the memory is spent only while the digits are on the screen.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_ssd1306.h>
#include <pixelfont.h>

static const char *TAG = "example";

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define FONT_PATH "/storage/digits.bdf"

static uint8_t framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8];
static esp_lcd_panel_handle_t panel = NULL;

static esp_err_t flush(void)
{
    for (int y = 0; y < DISPLAY_HEIGHT; y += 8)
    {
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, DISPLAY_WIDTH, y + 8,
                                                  framebuffer + (y / 8) * DISPLAY_WIDTH);
        if (err != ESP_OK)
            return err;
    }

    return ESP_OK;
}

static void mount_storage(void)
{
    esp_vfs_spiffs_conf_t conf =
    {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 2,
        .format_if_mount_failed = false,
    };

    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    size_t total = 0, used = 0;
    ESP_ERROR_CHECK(esp_spiffs_info("storage", &total, &used));
    ESP_LOGI(TAG, "Storage: %u of %u bytes used", (unsigned)used, (unsigned)total);
}

// the ink of the digits starts below the top edge of the cell, so the wheel
// has to move by the height of the ink and not by the height of the cell:
// otherwise two digits that slide next to each other look far apart
static bool glyph_bit(const pixelfont_t *font, const pixelfont_glyph_t *glyph, int x, int y)
{
    const int stride = (font->width + 7) / 8;
    return (font->bitmaps[glyph->offset + y * stride + (x >> 3)] & (0x80 >> (x & 7))) != 0;
}

static void digit_ink_box(const pixelfont_t *font, int *top, int *bottom)
{
    *top = font->height;
    *bottom = -1;

    for (char c = '0'; c <= '9'; c++)
    {
        const pixelfont_glyph_t *glyph = NULL;
        if (pixelfont_find_glyph(font, (uint32_t)c, &glyph) != ESP_OK)
            continue;

        for (int y = 0; y < font->height; y++)
            for (int x = 0; x < font->width; x++)
                if (glyph_bit(font, glyph, x, y))
                {
                    if (y < *top)
                        *top = y;
                    if (y > *bottom)
                        *bottom = y;
                }
    }
}

void app_main(void)
{
    // the I2C bus and the panel
    i2c_master_bus_config_t bus_config =
    {
        .i2c_port = -1,
        .sda_io_num = CONFIG_EXAMPLE_I2C_SDA,
        .scl_io_num = CONFIG_EXAMPLE_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_config =
    {
        .dev_addr = CONFIG_EXAMPLE_I2C_ADDR,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.dc_low_on_data = false,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_config, &io));

    esp_lcd_panel_ssd1306_config_t ssd1306_config =
    {
        .height = DISPLAY_HEIGHT,
    };
    esp_lcd_panel_dev_config_t panel_config =
    {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd1306_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // the font from the file system
    mount_storage();

    pixelfont_t *big = NULL;
    esp_err_t err = pixelfont_load(FONT_PATH, &big);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not load %s: %s", FONT_PATH, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Loaded %s: %ux%u, %u glyphs", FONT_PATH, big->width, big->height, big->count);

    pixelfont_canvas_t canvas =
    {
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .pack = PIXELFONT_PACK_PAGE,
        .data = framebuffer,
        .size = sizeof(framebuffer),
    };

    // the four digits sit in cells of the size of the glyph, so a digit that
    // changes can slide out of its cell and the new one can slide in
    const int cell_w = big->width;
    const int cell_h = big->height;
    const int cell_y = DISPLAY_HEIGHT - cell_h;
    const int cell_x = (DISPLAY_WIDTH - 4 * cell_w) / 2;

    int ink_top, ink_bottom;
    digit_ink_box(big, &ink_top, &ink_bottom);
    // the gap between two digits of the wheel, in pixels
    const int travel = ink_bottom - ink_top + 1 + 3;

    // the window is the ink of the digits and not the whole cell: when the
    // wheel has moved by its pitch, the tail of the old digit must leave the
    // window, otherwise its remains stay on the screen
    const int window_y = cell_y + ink_top;
    const int window_h = ink_bottom - ink_top + 1;
    ESP_LOGI(TAG, "Digits use rows %d..%d of the cell, the wheel moves %d pixels",
             ink_top, ink_bottom, travel);

    ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, false));
    ESP_ERROR_CHECK(pixelfont_draw_text(big, &canvas, cell_x, cell_y, "0000", NULL));
    ESP_ERROR_CHECK(flush());
    vTaskDelay(pdMS_TO_TICKS(1000));

    char previous[4] = { '0', '0', '0', '0' };

    for (uint32_t counter = 1; ; counter++)
    {
        char text[8];
        snprintf(text, sizeof(text), "%04" PRIu32, counter % 10000);

        // every digit slides to its new place like a wheel of a counter: the
        // one that grows goes up, the one that shrinks goes down, and the clip
        // rectangle keeps the movement inside the cell
        for (int step = 0; step <= travel; step++)
        {
            for (int i = 0; i < 4; i++)
            {
                const int x = cell_x + i * cell_w;
                char old_digit[2] = { previous[i], 0 };
                char new_digit[2] = { text[i], 0 };

                ESP_ERROR_CHECK(pixelfont_set_clip(&canvas, x, window_y, cell_w, window_h));
                ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, x, window_y, cell_w, window_h, false));

                // the counter always grows, so a digit that passes 9 to 0
                // keeps going up, like a wheel of a counter
                const int delta = (text[i] - previous[i] + 10) % 10;

                if (previous[i] == text[i])
                {
                    ESP_ERROR_CHECK(pixelfont_draw_text(big, &canvas, x, cell_y, new_digit, NULL));
                }
                else if (delta <= 5)
                {
                    // the new digit enters from below
                    ESP_ERROR_CHECK(pixelfont_draw_text(big, &canvas, x, cell_y - step, old_digit, NULL));
                    ESP_ERROR_CHECK(pixelfont_draw_text(big, &canvas, x, cell_y + travel - step,
                                                        new_digit, NULL));
                }
                else
                {
                    // and from above, when the digit goes down
                    ESP_ERROR_CHECK(pixelfont_draw_text(big, &canvas, x, cell_y + step, old_digit, NULL));
                    ESP_ERROR_CHECK(pixelfont_draw_text(big, &canvas, x, cell_y - travel + step,
                                                        new_digit, NULL));
                }

                ESP_ERROR_CHECK(pixelfont_reset_clip(&canvas));
            }

            ESP_ERROR_CHECK(flush());
            vTaskDelay(pdMS_TO_TICKS(8));
        }

        memcpy(previous, text, 4);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}
