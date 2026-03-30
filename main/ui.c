/**
 * @file ui.c
 *
 * @brief UI module implementation:
 * All display layout + helper print functions.
 *
 * No full-screen clears are performed here.
 * The caller (main_app) performs a one-time st7789h2_fill() at boot. [1](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!s759f06148909480da87e953a55018e95)
 */

#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "st7789h2.h"

/* Font setup for display writing (same values you used in main_app.c) [1](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!s759f06148909480da87e953a55018e95) */
#define FONT_W 7 // pixels per character at scale=1 (check your font!)
#define FONT_H 5

/* Common colors used throughout UI */
#define UI_FG 0xFFFF
#define UI_BG 0x0000

/**
 * Helper: draw a string that overwrites its background.
 * Because bg color is supplied, this naturally "clears" the previous characters
 * behind the text, without clearing the entire screen.
 */
static inline void ui_draw_text(uint16_t x, uint16_t y, uint8_t scale, const char *s)
{
    st7789h2_draw_string_scaled(x, y, s, UI_FG, UI_BG, scale);
}

/**
 * Helper: printf-style formatting into a local buffer and draw.
 * IMPORTANT SAFETY:
 * Always use a real format string ("%s", ...) when printing other strings to avoid
 * accidental format string interpretation.
 */
static inline void ui_draw_printf(uint16_t x, uint16_t y, uint8_t scale,
                                  char *buf, size_t buf_sz,
                                  const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, buf_sz, fmt, ap);
    va_end(ap);

    ui_draw_text(x, y, scale, buf);
}

void ui_render_frame(const ui_layout_t *layout,
                     float ambient_temp_c,
                     const struct bme68x_data *data,
                     const baro_forecast_t *baro,
                     const air_quality_out_t *aq_out)
{
    uint16_t x = layout->x_pos;
    uint16_t y = layout->y_pos_start;
    uint8_t  scale = layout->scale;
    uint16_t lh = layout->line_height;

    char buf[64];

    /* ---------------------------------------------------------------------- */
    /* Temperature (same style as before, including raised "0" and "C")        */
    /* ---------------------------------------------------------------------- */

    ui_draw_printf(x, y, scale, buf, sizeof(buf), "Temp: %.1f", ambient_temp_c);

    /* Draw raised "0" and then "C" (preserved idea from your original code) [1](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!s759f06148909480da87e953a55018e95) */
    int text_width = (int)strlen(buf) * FONT_W * scale - 10;
    st7789h2_draw_string_scaled(x + text_width,
                                y - (scale * 4),
                                "0",
                                UI_FG, UI_BG,
                                (scale > 1) ? (scale - 1) : 1);

    st7789h2_draw_string_scaled(x + text_width + (FONT_W * ((scale > 1) ? (scale - 1) : 1)),
                                y,
                                "C",
                                UI_FG, UI_BG,
                                scale);

    /* Line shift */
    y += lh;

    /* ---------------------------------------------------------------------- */
    /* Relative humidity                                                       */
    /* ---------------------------------------------------------------------- */
    ui_draw_printf(x, y, scale, buf, sizeof(buf), "Hum: %.1f %%RH", data->humidity);

    /* Line shift */
    y += lh;

    /* ---------------------------------------------------------------------- */
    /* Barometer forecast evaluation output (SLP + Forecast + Alerts/Trend)     */
    /* ---------------------------------------------------------------------- */

    ui_draw_printf(x, y, scale, buf, sizeof(buf), "SLP: %.0f hPa", baro_forecast_slp_hpa(baro));
    y += lh;
    y += lh;

    ui_draw_text(x, y, scale, "Forecast:");
    y += lh;

    ui_draw_printf(x, y, scale, buf, sizeof(buf), "%s", baro_forecast_text(baro));
    y += lh;

    /**
     * Alerts override Trend:
     * - storm_level is an enum from baro_forecast_storm_level() [2](https://onedrive.live.com?cid=4BD8DE1550EDB1B8&id=4BD8DE1550EDB1B8!sd30a4f9147c046bdbac0de62dc6306dc)
     * - compare against STORM_NONE (not string text)
     */
    storm_level_t storm = baro_forecast_storm_level(baro);
    if (storm != STORM_NONE)
    {
        ui_draw_text(x, y, scale, "Alerts:");
        y += lh;

        ui_draw_printf(x, y, scale, buf, sizeof(buf), "%s", storm_level_str(storm));
        y += lh;
    }
    else
    {
        ui_draw_text(x, y, scale, "Trend:");
        y += lh;

        ui_draw_printf(x, y, scale, buf, sizeof(buf), "%s", baro_trend_str(baro_forecast_trend(baro)));
        y += lh;
    }

    /* ---------------------------------------------------------------------- */
    /* Air quality                                                             */
    /* ---------------------------------------------------------------------- */

    ui_draw_text(x, y, scale, "Air Quality:");
    y += lh;

    /* If not ready, aq_out->text is "Warming up..." (from module) */
    ui_draw_printf(x, y, scale, buf, sizeof(buf), "%s", aq_out->text);
}