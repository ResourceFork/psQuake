/*
 * psQuake -- hooks the vendored QuakeSpasm engine calls into psQuake code,
 * and psQuake functions added to engine files. Every call site is marked
 * [psQuake] and listed in third_party/README.md.
 *
 * Include after quakedef.h.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#ifndef PSQ_ENGINE_H
#define PSQ_ENGINE_H

/* psq_sys.c: choose basedir/userdir (registered Quake in the user root, or
 * the bundled LibreQuake). Called from Sys_Init. */
void PSQ_InitPaths(quakeparms_t *parms);

/* gl_vidsdl.c: follow a window size change without a vid_restart. Called
 * from in_sdl.c on SDL_WINDOWEVENT_SIZE_CHANGED. */
void VID_PSQ_WindowSizeChanged(void);

/* menu.c: the key-binding menu is waiting for a key (or mouse button). */
qboolean M_PSQ_BindingKey(void);

#endif /* PSQ_ENGINE_H */
