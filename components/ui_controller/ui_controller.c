/*
 * ui_controller.c
 *
 * Implements button behavior you specified:
 * - UP toggles overview <-> min/max
 * - LEFT/RIGHT/DOWN only function in min/max
 * - First press arms confirmation prompt
 * - Second press of SAME reset button is handled by caller (does reset),
 *   then caller cancels prompt.
 * - Any other button cancels the prompt.
 */

#include "ui_controller.h"

void ui_controller_init(ui_controller_t *u)
{
    if (!u) return;
    u->screen = UI_SCREEN_OVERVIEW;
    u->confirm_armed = false;
    u->confirm_target = UI_CONFIRM_NONE;
}

ui_screen_t ui_controller_screen(const ui_controller_t *u)
{
    return u ? u->screen : UI_SCREEN_OVERVIEW;
}

bool ui_controller_confirm_active(const ui_controller_t *u)
{
    return u ? u->confirm_armed : false;
}

ui_confirm_target_t ui_controller_confirm_target(const ui_controller_t *u)
{
    return u ? u->confirm_target : UI_CONFIRM_NONE;
}

bool ui_controller_cancel_confirm(ui_controller_t *u)
{
    if (!u) return false;
    if (!u->confirm_armed) return false;
    u->confirm_armed = false;
    u->confirm_target = UI_CONFIRM_NONE;
    return true;
}

static bool arm_confirm(ui_controller_t *u, ui_confirm_target_t tgt)
{
    u->confirm_armed = true;
    u->confirm_target = tgt;
    return true;
}

bool ui_controller_handle_action(ui_controller_t *u, ui_action_t act)
{
    if (!u) return false;
    bool changed = false;

    // If confirmation active: different button cancels prompt and proceeds.
    if (u->confirm_armed) {
        bool same_reset =
            (u->confirm_target == UI_CONFIRM_TEMP  && act == UI_ACTION_LEFT)  ||
            (u->confirm_target == UI_CONFIRM_RH    && act == UI_ACTION_RIGHT) ||
            (u->confirm_target == UI_CONFIRM_PRESS && act == UI_ACTION_DOWN);
            
        if (!same_reset) {
            changed |= ui_controller_cancel_confirm(u);
        } else {
            // same reset pressed again: caller will execute reset + cancel
            return true;
        }
    }

    switch (act) {
    case UI_ACTION_UP:
        u->screen = (u->screen == UI_SCREEN_OVERVIEW) ? UI_SCREEN_MINMAX : UI_SCREEN_OVERVIEW;
        changed = true;
        break;

    case UI_ACTION_LEFT:
        if (u->screen == UI_SCREEN_MINMAX) changed |= arm_confirm(u, UI_CONFIRM_TEMP);
        break;

    case UI_ACTION_RIGHT:
        if (u->screen == UI_SCREEN_MINMAX) changed |= arm_confirm(u, UI_CONFIRM_RH);
        break;

    case UI_ACTION_DOWN:
        if (u->screen == UI_SCREEN_MINMAX) changed |= arm_confirm(u, UI_CONFIRM_PRESS);
        break;
    }

    return changed;
}