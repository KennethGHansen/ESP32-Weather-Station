#pragma once
#include <stdint.h>
#include "baro_forecast.h"
#include "bme68x.h"
#include "air_quality.h"

/**
 * @file ui.h
 *
 * @brief UI module:
 *  - Owns display layout (x/y positioning, line spacing)
 *  - Owns helper string formatting and drawing
 *
 * IMPORTANT:
 * We do NOT clear the entire screen per update because the display is pixel-write over SPI
 * and that would cause blinking. Each printed line is drawn with background color, so new
 * text overwrites old text cleanly.
 */

typedef struct
{
    uint8_t  scale;
    uint16_t line_height;
    uint16_t y_pos_start;
    uint16_t x_pos;
} ui_layout_t;

/**
 * Render one full “frame” worth of UI text.
 * This function overwrites the same text regions each update.
 */
void ui_render_frame(const ui_layout_t *layout,
                     float ambient_temp_c,
                     const struct bme68x_data *data,
                     const baro_forecast_t *baro,
                     const air_quality_out_t *aq_out);