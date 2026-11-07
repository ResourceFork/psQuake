/*
 * psQuake -- Cocoa window helpers for gl_vidsdl.c (see psq_macos.h).
 *
 * SDL 2.0.22 puts desktop-fullscreen windows in a macOS Space and gives a
 * resizable window the title-bar fullscreen button. When macOS itself
 * enters or leaves the Space (that button, or Ctrl-Cmd-F through SDL's
 * default Window menu), SDL never updates SDL_WINDOW_FULLSCREEN_DESKTOP
 * (SDL_cocoawindow.m, windowDidEnterFullScreen / windowDidExitFullScreen),
 * so the engine would save the wrong window state and need two Alt-Enter
 * presses to toggle.
 *
 * PSQ_MacWatchFullscreenSpaces() observes the window's did-enter and
 * did-exit notifications. When one arrives and SDL's flag disagrees with
 * what the window now is, it calls SDL_SetWindowFullscreen to agree. That
 * call cannot start a second transition: SDL's setFullscreenSpace returns
 * "already there" because its own isFullscreenSpace already matches. SDL's
 * own requests set the flag before they toggle, so they never disagree here.
 *
 * MIT licensed, like the rest of psQuake's own files. It is compiled only
 * into the GPL-2 engine binary.
 */

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#import <Cocoa/Cocoa.h>

#include "psq_macos.h"

static NSWindow *psq_nswindow(SDL_Window *window)
{
    SDL_SysWMinfo info;

    if (window == NULL)
        return nil;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)
        || info.subsystem != SDL_SYSWM_COCOA)
        return nil;
    return info.info.cocoa.window;
}

int PSQ_MacWindowInFullscreenSpace(SDL_Window *window)
{
    NSWindow *w = psq_nswindow(window);
    return w != nil && ([w styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

static int psq_sdl_desktop_fullscreen(SDL_Window *window)
{
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP)
        == SDL_WINDOW_FULLSCREEN_DESKTOP;
}

void PSQ_MacWatchFullscreenSpaces(SDL_Window *window)
{
    static id entered = nil, exited = nil;
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    NSWindow *w = psq_nswindow(window);

    if (w == nil || entered != nil)
        return;

    entered = [nc addObserverForName:NSWindowDidEnterFullScreenNotification
                              object:w
                               queue:nil
                          usingBlock:^(NSNotification *note) {
        (void)note;
        if (!psq_sdl_desktop_fullscreen(window))
            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }];
    exited = [nc addObserverForName:NSWindowDidExitFullScreenNotification
                             object:w
                              queue:nil
                         usingBlock:^(NSNotification *note) {
        (void)note;
        if (psq_sdl_desktop_fullscreen(window))
            SDL_SetWindowFullscreen(window, 0);
    }];
    [entered retain];
    [exited retain];
}
