/*
 * psQuake -- an offscreen SDL video layer for psq_labels_render.
 *
 * psq_labels_render links the real engine, renderer included, but no
 * window may appear on the human's screen. This file defines the SDL video,
 * window, GL, mouse and event functions the engine calls; the linker binds
 * the engine to these definitions before SDL2.framework's, so SDL's Cocoa
 * video driver is never initialised: no NSApplication, no window, no Dock
 * icon, no focus change. Rendering goes into a framebuffer object on a
 * windowless CGL context (an accelerated, offline-capable pixel format),
 * sized like the window the engine asked for, and psq_offscreen_read()
 * reads it back.
 *
 * Timers, RWops, error strings and the like stay SDL's own. Audio, CD,
 * joystick and game controller subsystems refuse to start; run with
 * -nosound -nocdaudio. The platform hooks in pl_osx.m and psq_macos.m are
 * replaced too: a Sys_Error prints instead of showing an alert. So are the
 * IOKit calls in_sdl.c makes to switch off the system's mouse acceleration
 * while the game has the mouse: here every one fails, so a run can never
 * change the host's mouse settings.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#define GL_SILENCE_DEPRECATION 1

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_types.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psq_offscreen.h"

/* The display: a Retina-class desktop, plus the modes a player picks. */
static const SDL_DisplayMode psq_modes[] = {
    { SDL_PIXELFORMAT_RGB888, 2560, 1600, 60, NULL },
    { SDL_PIXELFORMAT_RGB888, 1920, 1080, 60, NULL },
    { SDL_PIXELFORMAT_RGB888, 1280,  720, 60, NULL },
    { SDL_PIXELFORMAT_RGB888,  640,  480, 60, NULL },
};
#define PSQ_NMODES ((int) (sizeof(psq_modes) / sizeof(psq_modes[0])))

static struct
{
    int             exists;
    int             w, h;         /* windowed size              */
    Uint32          flags;
    SDL_DisplayMode mode;         /* exclusive fullscreen mode  */
} psq_win;

static CGLContextObj psq_ctx;
static GLuint        psq_fbo, psq_rb_color, psq_rb_depth;
static int           psq_fb_w, psq_fb_h;
static void         *psq_gl_lib;
static unsigned      psq_swaps;

#define PSQ_UNAVAILABLE \
    (SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMECONTROLLER | SDL_INIT_SENSOR)

static void psq_window_size(int *w, int *h)
{
    if ((psq_win.flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP)
    {
        *w = psq_modes[0].w;
        *h = psq_modes[0].h;
    }
    else if (psq_win.flags & SDL_WINDOW_FULLSCREEN)
    {
        *w = psq_win.mode.w ? psq_win.mode.w : psq_modes[0].w;
        *h = psq_win.mode.h ? psq_win.mode.h : psq_modes[0].h;
    }
    else
    {
        *w = psq_win.w;
        *h = psq_win.h;
    }
}

/* Size the framebuffer object to the window, keeping it bound. */
static void psq_fb_sync(void)
{
    int w, h;

    if (!psq_ctx)
        return;
    psq_window_size(&w, &h);
    if (w == psq_fb_w && h == psq_fb_h)
        return;
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, psq_rb_color);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, psq_rb_depth);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, psq_fbo);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
    {
        fprintf(stderr, "psq-offscreen: framebuffer %dx%d incomplete\n", w, h);
        exit(1);
    }
    psq_fb_w = w;
    psq_fb_h = h;
}

int psq_offscreen_read(unsigned char **rgb, int *w, int *h)
{
    unsigned char *buf;

    if (!psq_ctx || psq_fb_w <= 0 || psq_fb_h <= 0)
        return 0;
    buf = (unsigned char *) malloc((size_t) psq_fb_w * psq_fb_h * 3);
    if (!buf)
        return 0;
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, psq_fb_w, psq_fb_h, GL_RGB, GL_UNSIGNED_BYTE, buf);
    *rgb = buf;
    *w = psq_fb_w;
    *h = psq_fb_h;
    return 1;
}

unsigned psq_offscreen_swaps(void)
{
    return psq_swaps;
}

/* ------------------------------------------------------------ subsystems */

static Uint32 psq_masked(Uint32 flags)
{
    return flags & ~(Uint32) (SDL_INIT_VIDEO | PSQ_UNAVAILABLE);
}

int SDL_Init(Uint32 flags)
{
    int (*real)(Uint32) = (int (*)(Uint32)) dlsym(RTLD_NEXT, "SDL_Init");

    if (flags & PSQ_UNAVAILABLE)
        return SDL_SetError("psq-offscreen: subsystem unavailable");
    return real ? real(psq_masked(flags)) : -1;
}

int SDL_InitSubSystem(Uint32 flags)
{
    int (*real)(Uint32) = (int (*)(Uint32)) dlsym(RTLD_NEXT, "SDL_InitSubSystem");

    if (flags & PSQ_UNAVAILABLE)
        return SDL_SetError("psq-offscreen: subsystem unavailable");
    if (psq_masked(flags) == 0)
        return 0;
    return real ? real(psq_masked(flags)) : -1;
}

void SDL_QuitSubSystem(Uint32 flags)
{
    void (*real)(Uint32) = (void (*)(Uint32)) dlsym(RTLD_NEXT, "SDL_QuitSubSystem");

    if (real && psq_masked(flags))
        real(psq_masked(flags));
}

int SDL_PollEvent(SDL_Event *event)
{
    (void) event;
    return 0;
}

int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    (void) enabled;
    return 0;
}

void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y)
{
    (void) window; (void) x; (void) y;
}

void SDL_StartTextInput(void) {}
void SDL_StopTextInput(void) {}

/* --------------------------------------------------------------- display */

int SDL_GetNumDisplayModes(int display)
{
    return display == 0 ? PSQ_NMODES : SDL_SetError("no display %d", display);
}

int SDL_GetDisplayMode(int display, int index, SDL_DisplayMode *mode)
{
    if (display != 0 || index < 0 || index >= PSQ_NMODES || !mode)
        return SDL_SetError("no mode");
    *mode = psq_modes[index];
    return 0;
}

int SDL_GetDesktopDisplayMode(int display, SDL_DisplayMode *mode)
{
    return SDL_GetDisplayMode(display, 0, mode);
}

int SDL_GetCurrentDisplayMode(int display, SDL_DisplayMode *mode)
{
    return SDL_GetDisplayMode(display, 0, mode);
}

/* ---------------------------------------------------------------- window */

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    (void) title; (void) x; (void) y;
    if (psq_win.exists)
    {
        SDL_SetError("psq-offscreen: one window only");
        return NULL;
    }
    psq_win.exists = 1;
    psq_win.w = w;
    psq_win.h = h;
    psq_win.flags = (flags | SDL_WINDOW_OPENGL) & ~(Uint32) SDL_WINDOW_SHOWN;
    return (SDL_Window *) &psq_win;
}

void SDL_DestroyWindow(SDL_Window *window)
{
    (void) window;
    memset(&psq_win, 0, sizeof(psq_win));
}

Uint32 SDL_GetWindowFlags(SDL_Window *window)
{
    (void) window;
    return psq_win.flags;
}

void SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    int ww, hh;

    (void) window;
    psq_window_size(&ww, &hh);
    if (w) *w = ww;
    if (h) *h = hh;
}

void SDL_SetWindowSize(SDL_Window *window, int w, int h)
{
    (void) window;
    psq_win.w = w;
    psq_win.h = h;
    psq_fb_sync();
}

int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags)
{
    (void) window;
    psq_win.flags &= ~(Uint32) SDL_WINDOW_FULLSCREEN_DESKTOP;
    psq_win.flags |= flags & SDL_WINDOW_FULLSCREEN_DESKTOP;
    psq_fb_sync();
    return 0;
}

int SDL_SetWindowDisplayMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
    (void) window;
    if (mode)
        psq_win.mode = *mode;
    psq_fb_sync();
    return 0;
}

int SDL_GetWindowDisplayIndex(SDL_Window *window) { (void) window; return 0; }
Uint32 SDL_GetWindowPixelFormat(SDL_Window *window) { (void) window; return SDL_PIXELFORMAT_RGB888; }
void SDL_SetWindowPosition(SDL_Window *window, int x, int y) { (void) window; (void) x; (void) y; }
void SDL_SetWindowBordered(SDL_Window *window, SDL_bool b) { (void) window; (void) b; }
void SDL_SetWindowMinimumSize(SDL_Window *window, int w, int h) { (void) window; (void) w; (void) h; }
void SDL_RaiseWindow(SDL_Window *window) { (void) window; }

void SDL_ShowWindow(SDL_Window *window)
{
    (void) window;
    /* Shown only to the engine: input focus, so it renders as a player
     * would see it. Nothing reaches the screen. */
    psq_win.flags |= SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
}

int SDL_SetWindowBrightness(SDL_Window *window, float b)
{
    (void) window; (void) b;
    return SDL_SetError("psq-offscreen: no hardware gamma");
}

SDL_bool SDL_GetWindowWMInfo(SDL_Window *window, SDL_SysWMinfo *info)
{
    (void) window; (void) info;
    SDL_SetError("psq-offscreen: no window system");
    return SDL_FALSE;
}

/* -------------------------------------------------------------------- GL */

int SDL_GL_SetAttribute(SDL_GLattr attr, int value)
{
    (void) attr; (void) value;
    return 0;
}

int SDL_GL_GetAttribute(SDL_GLattr attr, int *value)
{
    int v = 0;

    switch (attr)
    {
    case SDL_GL_RED_SIZE: case SDL_GL_GREEN_SIZE: case SDL_GL_BLUE_SIZE:
    case SDL_GL_ALPHA_SIZE: case SDL_GL_STENCIL_SIZE:
        v = 8; break;
    case SDL_GL_DEPTH_SIZE: v = 24; break;
    case SDL_GL_DOUBLEBUFFER: v = 1; break;
    default: v = 0; break;
    }
    if (value)
        *value = v;
    return 0;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers,
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute) kCGLOGLPVersion_Legacy,
        kCGLPFAColorSize, (CGLPixelFormatAttribute) 24,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute) 8,
        kCGLPFADepthSize, (CGLPixelFormatAttribute) 24,
        kCGLPFAStencilSize, (CGLPixelFormatAttribute) 8,
        (CGLPixelFormatAttribute) 0 };
    CGLPixelFormatObj pf = NULL;
    GLint npf = 0;

    (void) window;
    if (psq_ctx)
        return psq_ctx;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || !pf)
    {
        SDL_SetError("psq-offscreen: no CGL pixel format");
        return NULL;
    }
    if (CGLCreateContext(pf, NULL, &psq_ctx) != kCGLNoError)
    {
        CGLReleasePixelFormat(pf);
        SDL_SetError("psq-offscreen: no CGL context");
        return NULL;
    }
    CGLReleasePixelFormat(pf);
    CGLSetCurrentContext(psq_ctx);
    glGenFramebuffersEXT(1, &psq_fbo);
    glGenRenderbuffersEXT(1, &psq_rb_color);
    glGenRenderbuffersEXT(1, &psq_rb_depth);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, psq_fbo);
    psq_fb_w = psq_fb_h = -1;
    {
        int w, h;
        psq_window_size(&w, &h);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, psq_rb_color);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                     GL_RENDERBUFFER_EXT, psq_rb_color);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, psq_rb_depth);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                     GL_RENDERBUFFER_EXT, psq_rb_depth);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT,
                                     GL_RENDERBUFFER_EXT, psq_rb_depth);
        psq_fb_w = psq_fb_h = 0;
    }
    psq_fb_sync();
    fprintf(stderr, "psq-offscreen: %s, %s, %s; framebuffer %dx%d\n",
            (const char *) glGetString(GL_VENDOR), (const char *) glGetString(GL_RENDERER),
            (const char *) glGetString(GL_VERSION), psq_fb_w, psq_fb_h);
    return psq_ctx;
}

void SDL_GL_DeleteContext(SDL_GLContext context)
{
    if (!psq_ctx || context != psq_ctx)
        return;
    glDeleteRenderbuffersEXT(1, &psq_rb_color);
    glDeleteRenderbuffersEXT(1, &psq_rb_depth);
    glDeleteFramebuffersEXT(1, &psq_fbo);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(psq_ctx);
    psq_ctx = NULL;
    psq_fb_w = psq_fb_h = 0;
}

void *SDL_GL_GetProcAddress(const char *proc)
{
    if (!psq_gl_lib)
        psq_gl_lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    return psq_gl_lib ? dlsym(psq_gl_lib, proc) : NULL;
}

void SDL_GL_SwapWindow(SDL_Window *window)
{
    (void) window;
    glFlush();
    psq_swaps++;
}

int SDL_GL_SetSwapInterval(int interval) { (void) interval; return 0; }
int SDL_GL_GetSwapInterval(void) { return 0; }

/* Message boxes: SDL can show a native alert even with no video running
 * (an assertion prompt, for one). Never here. */
int SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *button)
{
    fprintf(stderr, "psq-offscreen: message box refused: %s\n",
            data && data->message ? data->message : "");
    if (button)
        *button = -1;
    return SDL_SetError("psq-offscreen: no message boxes");
}

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window)
{
    (void) flags; (void) title; (void) window;
    fprintf(stderr, "psq-offscreen: message box refused: %s\n", message ? message : "");
    return SDL_SetError("psq-offscreen: no message boxes");
}

/* -------------------------------------------- platform hooks (pl_osx.m) */

void PL_SetWindowIcon(void) {}
void PL_VID_Shutdown(void) {}
char *PL_GetClipboardData(void) { return NULL; }

void PL_ErrorDialog(const char *text)
{
    fprintf(stderr, "psq-offscreen: error: %s\n", text);
}

/* ------------------------------------------ psQuake hooks (psq_macos.m) */

int PSQ_MacWindowInFullscreenSpace(SDL_Window *window)
{
    (void) window;
    return 0;
}

void PSQ_MacWatchFullscreenSpaces(SDL_Window *window)
{
    (void) window;
}

/* ------------------------------------- IOKit (in_sdl.c mouse acceleration) */

#define PSQ_KERN_FAILURE 5            /* KERN_FAILURE              */
#define PSQ_IO_UNSUPPORTED 0xe00002c7 /* kIOReturnUnsupported      */

kern_return_t IOMasterPort(mach_port_t bootstrap, mach_port_t *main_port)
{
    (void) bootstrap;
    if (main_port)
        *main_port = MACH_PORT_NULL;
    return PSQ_KERN_FAILURE;
}

mach_port_t IORegistryEntryFromPath(mach_port_t main_port, const char *path)
{
    (void) main_port; (void) path;
    return MACH_PORT_NULL;
}

kern_return_t IOServiceOpen(mach_port_t service, mach_port_t owning_task, uint32_t type,
                            mach_port_t *connect)
{
    (void) service; (void) owning_task; (void) type;
    if (connect)
        *connect = MACH_PORT_NULL;
    return PSQ_KERN_FAILURE;
}

kern_return_t IOServiceClose(mach_port_t connect) { (void) connect; return 0; }
kern_return_t IOObjectRelease(mach_port_t object) { (void) object; return 0; }

kern_return_t IOHIDGetAccelerationWithKey(mach_port_t handle, CFStringRef key, double *acceleration)
{
    (void) handle; (void) key; (void) acceleration;
    return (kern_return_t) PSQ_IO_UNSUPPORTED;
}

kern_return_t IOHIDSetAccelerationWithKey(mach_port_t handle, CFStringRef key, double acceleration)
{
    (void) handle; (void) key; (void) acceleration;
    return (kern_return_t) PSQ_IO_UNSUPPORTED;
}
