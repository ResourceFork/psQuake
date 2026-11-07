/*
 * psQuake -- mouse grab decision (engine-free, host-tested).
 *
 * QuakeSpasm grabs the mouse (SDL relative mode, hidden cursor) during play.
 * psq_grab_wanted() says whether it should be grabbed for the current input
 * state; psq_grab_next() turns that into the one SDL call needed to get there.
 * in_sdl.c's IN_UpdateGrab() applies the result once per frame, so a menu or
 * console opened or closed by any path is picked up on the same frame.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#ifndef PSQ_GRAB_H
#define PSQ_GRAB_H

/* Where keys go; mirrors QuakeSpasm's keydest_t (keys.h). */
typedef enum
{
    PSQ_DEST_GAME,
    PSQ_DEST_CONSOLE,
    PSQ_DEST_MESSAGE, /* typing a chat message during play */
    PSQ_DEST_MENU
} psq_dest;

typedef struct
{
    int      mouse_enabled;  /* 0 with -nomouse */
    int      window_focused; /* the game window has input focus */
    int      fullscreen;     /* modestate == MS_FULLSCREEN */
    int      binding_key;    /* the key-binding menu waits for a key or
                                mouse button (M_PSQ_BindingKey) */
    psq_dest dest;
} psq_grab_inputs;

/* What the engine last did to the mouse. */
typedef enum
{
    PSQ_MOUSE_UNKNOWN,  /* nothing applied yet */
    PSQ_MOUSE_GRABBED,  /* IN_Activate: relative mode on, events read */
    PSQ_MOUSE_RELEASED, /* IN_Deactivate(true): cursor free and shown */
    PSQ_MOUSE_HELD      /* IN_Deactivate(false): relative mode still on, but
                           mouse events discarded (upstream's fullscreen menu) */
} psq_mouse_state;

typedef enum
{
    PSQ_GRAB_NONE,
    PSQ_GRAB_ACTIVATE, /* call IN_Activate() */
    PSQ_GRAB_RELEASE   /* call IN_Deactivate(true) */
} psq_grab_action;

/* 1 if the mouse should be grabbed for this input state, 0 if released. */
int psq_grab_wanted(const psq_grab_inputs *in);

/* The call that moves the mouse from `state` to `wanted`, if any. */
psq_grab_action psq_grab_next(int wanted, psq_mouse_state state);

#endif /* PSQ_GRAB_H */
