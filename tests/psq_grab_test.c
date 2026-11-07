/*
 * psQuake -- host tests for the mouse grab decision (src/engine/psq_grab.c).
 *
 * The requirement: the menu or console releases the mouse and shows the
 * cursor, and closing it grabs the mouse again at once, in windowed and
 * fullscreen mode alike, except while the key-binding menu waits for a key
 * or mouse button. Play keeps the mouse grabbed. Losing window focus
 * releases it.
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "psq_grab.h"

#include <stdio.h>

static int checks_run;
static int checks_failed;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        checks_run++;                                                     \
        if (!(cond)) {                                                    \
            checks_failed++;                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

static const char *dest_name(psq_dest d)
{
    switch (d) {
    case PSQ_DEST_GAME: return "game";
    case PSQ_DEST_CONSOLE: return "console";
    case PSQ_DEST_MESSAGE: return "message";
    case PSQ_DEST_MENU: return "menu";
    }
    return "?";
}

static int wanted_b(int mouse, int focus, int fullscreen, int binding,
                    psq_dest dest)
{
    psq_grab_inputs in;
    in.mouse_enabled = mouse;
    in.window_focused = focus;
    in.fullscreen = fullscreen;
    in.binding_key = binding;
    in.dest = dest;
    return psq_grab_wanted(&in);
}

static int wanted(int mouse, int focus, int fullscreen, psq_dest dest)
{
    return wanted_b(mouse, focus, fullscreen, 0, dest);
}

static void test_wanted_matrix(void)
{
    static const psq_dest dests[] = {
        PSQ_DEST_GAME, PSQ_DEST_CONSOLE, PSQ_DEST_MESSAGE, PSQ_DEST_MENU };
    int fs, focus, mouse, bind;
    unsigned i;

    for (mouse = 0; mouse <= 1; mouse++)
    for (focus = 0; focus <= 1; focus++)
    for (fs = 0; fs <= 1; fs++)
    for (bind = 0; bind <= 1; bind++)
    for (i = 0; i < sizeof(dests) / sizeof(dests[0]); i++) {
        psq_dest d = dests[i];
        int expect = mouse && focus
            && (d == PSQ_DEST_GAME || d == PSQ_DEST_MESSAGE
                || (d == PSQ_DEST_MENU && bind));
        int got = wanted_b(mouse, focus, fs, bind, d);
        CHECK(got == expect,
              "wanted(mouse=%d focus=%d fullscreen=%d binding=%d dest=%s)"
              " = %d, want %d", mouse, focus, fs, bind, dest_name(d), got,
              expect);
    }

    CHECK(psq_grab_wanted(NULL) == 0, "NULL inputs must not grab");
}

/* Named cases from the requirement, so a failure reads as the bug. */
static void test_named_cases(void)
{
    CHECK(wanted(1, 1, 1, PSQ_DEST_MENU) == 0,
          "fullscreen menu must release the mouse");
    CHECK(wanted(1, 1, 1, PSQ_DEST_CONSOLE) == 0,
          "fullscreen console must release the mouse");
    CHECK(wanted(1, 1, 0, PSQ_DEST_MENU) == 0,
          "windowed menu must release the mouse");
    CHECK(wanted(1, 1, 0, PSQ_DEST_CONSOLE) == 0,
          "windowed console must release the mouse");
    CHECK(wanted(1, 1, 1, PSQ_DEST_GAME) == 1,
          "fullscreen play must grab the mouse");
    CHECK(wanted(1, 1, 0, PSQ_DEST_GAME) == 1,
          "windowed play must grab the mouse");
    CHECK(wanted(1, 0, 1, PSQ_DEST_GAME) == 0,
          "an unfocused fullscreen window must not hold the mouse");
    CHECK(wanted_b(1, 1, 0, 1, PSQ_DEST_MENU) == 1,
          "the key-binding menu must capture the mouse, so a click binds");
    CHECK(wanted_b(1, 1, 0, 1, PSQ_DEST_CONSOLE) == 0,
          "the binding flag does not hold the console");
}

static void test_next(void)
{
    /* Grab wanted. */
    CHECK(psq_grab_next(1, PSQ_MOUSE_UNKNOWN) == PSQ_GRAB_ACTIVATE,
          "unknown -> grab must activate");
    CHECK(psq_grab_next(1, PSQ_MOUSE_RELEASED) == PSQ_GRAB_ACTIVATE,
          "closing the menu must re-grab without a click");
    CHECK(psq_grab_next(1, PSQ_MOUSE_HELD) == PSQ_GRAB_ACTIVATE,
          "held (events ignored) -> grab must activate, or mouse look stays dead");
    CHECK(psq_grab_next(1, PSQ_MOUSE_GRABBED) == PSQ_GRAB_NONE,
          "already grabbed: no call");

    /* Release wanted. */
    CHECK(psq_grab_next(0, PSQ_MOUSE_UNKNOWN) == PSQ_GRAB_RELEASE,
          "unknown -> release must release");
    CHECK(psq_grab_next(0, PSQ_MOUSE_GRABBED) == PSQ_GRAB_RELEASE,
          "opening the menu must release");
    CHECK(psq_grab_next(0, PSQ_MOUSE_HELD) == PSQ_GRAB_RELEASE,
          "upstream's fullscreen menu hold must become a real release");
    CHECK(psq_grab_next(0, PSQ_MOUSE_RELEASED) == PSQ_GRAB_NONE,
          "already released: no call");
}

/* Menu open and close, as the per-frame update sees it. */
static void test_round_trip(int fullscreen)
{
    psq_mouse_state st = PSQ_MOUSE_GRABBED;
    psq_grab_action a;

    /* Upstream's menu entry: IN_Deactivate(modestate == MS_WINDOWED). */
    st = fullscreen ? PSQ_MOUSE_HELD : PSQ_MOUSE_RELEASED;
    a = psq_grab_next(wanted(1, 1, fullscreen, PSQ_DEST_MENU), st);
    if (a == PSQ_GRAB_RELEASE)
        st = PSQ_MOUSE_RELEASED;
    else if (a == PSQ_GRAB_ACTIVATE)
        st = PSQ_MOUSE_GRABBED;
    CHECK(st == PSQ_MOUSE_RELEASED,
          "fullscreen=%d: menu open must end released, got state %d",
          fullscreen, (int)st);

    /* Menu closed: the next frame must grab with no extra input. */
    a = psq_grab_next(wanted(1, 1, fullscreen, PSQ_DEST_GAME), st);
    CHECK(a == PSQ_GRAB_ACTIVATE,
          "fullscreen=%d: menu close must re-grab on the next frame",
          fullscreen);
}

int main(void)
{
    test_wanted_matrix();
    test_named_cases();
    test_next();
    test_round_trip(0);
    test_round_trip(1);

    printf("psq_grab_test: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
