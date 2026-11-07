/*
 * psQuake -- mouse grab decision (engine-free, host-tested). See psq_grab.h.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#include "psq_grab.h"

#include <stddef.h>

int psq_grab_wanted(const psq_grab_inputs *in)
{
    if (in == NULL || !in->mouse_enabled || !in->window_focused)
        return 0;

    /*
     * Upstream QuakeSpasm 0.97.0 calls IN_Deactivate(modestate ==
     * MS_WINDOWED) on every menu and console entry, so in fullscreen the
     * cursor stayed captured and hidden. psQuake releases it for the menu
     * and the console whatever the window mode; in->fullscreen is
     * deliberately unused.
     */
    if (in->dest == PSQ_DEST_MENU)
        return in->binding_key; /* capture, so a mouse button can be bound */
    if (in->dest == PSQ_DEST_CONSOLE)
        return 0;

    return 1;
}

psq_grab_action psq_grab_next(int wanted, psq_mouse_state state)
{
    if (wanted)
        return state == PSQ_MOUSE_GRABBED ? PSQ_GRAB_NONE : PSQ_GRAB_ACTIVATE;
    return state == PSQ_MOUSE_RELEASED ? PSQ_GRAB_NONE : PSQ_GRAB_RELEASE;
}
