/*
 * psQuake -- Cocoa window helpers (psq_macos.m). Include after SDL.h.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#ifndef PSQ_MACOS_H
#define PSQ_MACOS_H

/* 1 if the window is in a macOS fullscreen Space, whatever SDL's flags say. */
int PSQ_MacWindowInFullscreenSpace(SDL_Window *window);

/* Keep SDL's desktop-fullscreen flag in step when macOS itself enters or
 * leaves the window's fullscreen Space. Call once, after the window exists. */
void PSQ_MacWatchFullscreenSpaces(SDL_Window *window);

#endif /* PSQ_MACOS_H */
