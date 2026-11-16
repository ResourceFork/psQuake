/*
 * psQuake -- the offscreen SDL video layer's own calls (psq_offscreen_sdl.c).
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#ifndef PSQ_OFFSCREEN_H
#define PSQ_OFFSCREEN_H

/* Read the framebuffer the engine last rendered: bottom-up RGB, freed by
 * the caller. 1 on success. */
int psq_offscreen_read(unsigned char **rgb, int *w, int *h);

/* Frames the engine has finished (SDL_GL_SwapWindow calls). */
unsigned psq_offscreen_swaps(void);

/* 1 while SDL text input is on (SDL_StartTextInput without a later
 * SDL_StopTextInput). Starts on, as SDL2's video init leaves it. */
int psq_offscreen_text_input(void);

#endif /* PSQ_OFFSCREEN_H */
