/*
 * psQuake -- the Options > psQuake page, its live-kill warning and the
 * credits view.
 *
 * menu.c adds the "psQuake Options" entry to the Options menu and hands the
 * three menu states (m_psquake, m_psq_livewarn, m_psq_credits) to the
 * functions here; everything else about the page lives in this file.
 *
 * The kill mode row is the only way the kill mode changes: it asks
 * PSQ_SetKillMode (psq_builtins.c, which asks psq_killmode_change). Moving
 * up to live kill opens the warning, and only Y on the warning passes
 * live_confirmed; any other key cancels. The kill mode is never saved, so
 * every launch starts renice-only. The other rows set the archived psq_*
 * cvars, whose callbacks keep the trust guard (after a server, demo or
 * game code ran commands, all users and infighting may only be turned
 * off).
 *
 * MIT licensed, like the rest of psQuake's own files. It is compiled only
 * into the GPL-2 engine binary.
 */

#include "quakedef.h"
#include "psq_engine.h"

#include "psq_policy.h"
#include "psproc_options.h"

#ifndef PSQ_VERSION
#define PSQ_VERSION "0.1.0"
#endif

void M_DrawTextBox(int x, int y, int width, int lines);   /* menu.c */

enum
{
    PSQ_ROW_KILL,
    PSQ_ROW_CAP,
    PSQ_ROW_CLASSIFY,
    PSQ_ROW_INFIGHT,
    PSQ_ROW_ALLUSERS,
    PSQ_ROW_LABELS,
    PSQ_ROW_PID,
    PSQ_ROW_RANGE,
    PSQ_ROW_CREDITS,
    PSQ_ROWS
};

#define PSQ_ROW_Y(r)  (40 + 8 * (r))
#define PSQ_LABEL_R   160    /* labels end here (right-aligned)   */
#define PSQ_CURSOR_X  168
#define PSQ_VALUE_X   184

static int psq_menu_cursor;

static const char *const psq_kill_names[PSD_KILL_NUM] = {
    "simulate", "renice (no kill)", "renice + KILL"
};
static const char *const psq_range_names[PSD_LABELRANGE_NUM] = {
    "near", "normal", "far", "all"
};

static void PSQ_PrintRight(int right, int y, const char *s)
{
    M_Print(right - 8 * (int) strlen(s), y, s);
}

static void PSQ_PrintCentred(int y, const char *s, qboolean white)
{
    int x = 160 - 4 * (int) strlen(s);

    if (white)
        M_PrintWhite(x, y, s);
    else
        M_Print(x, y, s);
}

static int PSQ_Opt(const char *name)
{
    return (int) Cvar_VariableValue(name);
}

/* ------------------------------------------------------------ the page */

void M_Menu_PSQ_f(void)
{
    IN_Deactivate(modestate == MS_WINDOWED);
    key_dest = key_menu;
    m_state = m_psquake;
    m_entersound = true;
}

static void PSQ_Row(int row, const char *label, const char *value,
                    qboolean white)
{
    PSQ_PrintRight(PSQ_LABEL_R, PSQ_ROW_Y(row), label);
    if (value && white)
        M_PrintWhite(PSQ_VALUE_X, PSQ_ROW_Y(row), value);
    else if (value)
        M_Print(PSQ_VALUE_X, PSQ_ROW_Y(row), value);
}

/* Two or three lines about the row under the cursor, in a box. */
static void PSQ_Help(void)
{
    const char *l[3] = { "", "", "" };
    int mode = PSQ_KillMode();
    int i;

    switch (psq_menu_cursor)
    {
    case PSQ_ROW_KILL:
        if (mode == PSD_KILL_LIVE)
        {
            l[0] = "Hits lower priority. A killed";
            l[1] = "monster sends SIGTERM to its";
            l[2] = "process. Until quit.";
        }
        else if (mode == PSD_KILL_RENICE)
        {
            l[0] = "Hits lower a process's priority.";
            l[1] = "Kills signal nothing. Every";
            l[2] = "launch starts here.";
        }
        else
        {
            l[0] = "Just a game: no process is";
            l[1] = "touched. Until quit.";
        }
        break;
    case PSQ_ROW_CAP:
        l[0] = "Most process monsters alive at";
        l[1] = "once (5 to 35).";
        break;
    case PSQ_ROW_CLASSIFY:
        l[0] = "What makes a process monster";
        l[1] = "tougher: memory or CPU load.";
        break;
    case PSQ_ROW_INFIGHT:
        if (PSQ_Opt("psq_infighting"))
        {
            l[0] = "Monsters can hurt and kill";
            l[1] = "process monsters, which acts on";
            l[2] = "their processes like your hits.";
        }
        else
        {
            l[0] = "Monsters may attack process";
            l[1] = "monsters but do them no damage.";
        }
        break;
    case PSQ_ROW_ALLUSERS:
        l[0] = "Other users' processes too.";
        l[1] = "macOS may refuse to act on them.";
        break;
    case PSQ_ROW_LABELS:
        l[0] = "Each process monster's name";
        l[1] = "above it.";
        break;
    case PSQ_ROW_PID:
        l[0] = "The process ID above each name.";
        break;
    case PSQ_ROW_RANGE:
        l[0] = "How far away labels still show.";
        break;
    case PSQ_ROW_CREDITS:
        l[0] = "The parts psQuake is made of and";
        l[1] = "their licences.";
        break;
    }
    M_DrawTextBox(16, 120, 34, 3);
    if (PSQ_SessionUntrusted())
    {
        /* In place of the help: what can no longer go up. */
        PSQ_PrintCentred(128, "A server, demo or game code ran", true);
        PSQ_PrintCentred(136, "commands: kill mode, infighting,", true);
        PSQ_PrintCentred(144, "all users only go down till quit.", true);
        return;
    }
    for (i = 0; i < 3; i++)
        M_Print(32, 128 + 8 * i, l[i]);
}

void M_PSQ_Draw(void)
{
    qpic_t *p;
    int mode = PSQ_KillMode();
    int labels = PSQ_Opt("psq_labels");
    int range = PSQ_Opt("psq_labelrange");
    char cap[16];

    M_DrawTransPic(16, 4, Draw_CachePic("gfx/qplaque.lmp"));
    p = Draw_CachePic("gfx/p_option.lmp");
    M_DrawPic((320 - p->width) / 2, 4, p);
    PSQ_PrintCentred(30, "psQuake", true);

    if (mode < 0 || mode >= PSD_KILL_NUM)
        mode = PSD_KILL_SIMULATE;
    if (range < 0 || range >= PSD_LABELRANGE_NUM)
        range = PSD_LABELS_NORMAL;
    q_snprintf(cap, sizeof(cap), "%d", PSQ_Opt("psq_cap"));

    PSQ_Row(PSQ_ROW_KILL, "Kill mode", psq_kill_names[mode],
            mode == PSD_KILL_LIVE);
    PSQ_Row(PSQ_ROW_CAP, "Max live monsters", cap, false);
    PSQ_Row(PSQ_ROW_CLASSIFY, "Monster size by",
            PSQ_Opt("psq_classify") ? "CPU load" : "memory", false);
    PSQ_Row(PSQ_ROW_INFIGHT, "Monster infighting",
            PSQ_Opt("psq_infighting") ? "on" : "off", false);
    PSQ_Row(PSQ_ROW_ALLUSERS, "Target all users",
            PSQ_Opt("psq_allusers") ? "on" : "off", false);
    PSQ_Row(PSQ_ROW_LABELS, "Process labels", labels ? "on" : "off", false);
    PSQ_Row(PSQ_ROW_PID, "Show PID", PSQ_Opt("psq_showpid") ? "on" : "off",
            false);
    PSQ_Row(PSQ_ROW_RANGE, "Label distance", psq_range_names[range], false);
    PSQ_Row(PSQ_ROW_CREDITS, "Credits, licences", NULL, false);

    M_DrawCharacter(PSQ_CURSOR_X, PSQ_ROW_Y(psq_menu_cursor),
                    12 + ((int) (realtime * 4) & 1));
    if (m_state == m_psquake)   /* not under the warning */
        PSQ_Help();
}

/* Left (-1) or right/Enter (+1) on a row. */
static void PSQ_Adjust(int dir)
{
    int v;

    S_LocalSound("misc/menu3.wav");
    switch (psq_menu_cursor)
    {
    case PSQ_ROW_KILL:
        v = (PSQ_KillMode() + dir + PSD_KILL_NUM) % PSD_KILL_NUM;
        if (PSQ_SetKillMode(v, false) == PSQ_KILLMODE_CONFIRM)
        {
            m_state = m_psq_livewarn;
            m_entersound = true;
        }
        break;
    case PSQ_ROW_CAP:
        v = PSQ_Opt("psq_cap") + dir;
        if (v < PSD_MONSTER_CAP_MIN)
            v = PSD_MONSTER_CAP_MIN;
        if (v > PSD_MONSTER_CAP_MAX)
            v = PSD_MONSTER_CAP_MAX;
        Cvar_SetValue("psq_cap", (float) v);
        break;
    case PSQ_ROW_CLASSIFY:
        Cvar_SetValue("psq_classify", (float) !PSQ_Opt("psq_classify"));
        break;
    case PSQ_ROW_INFIGHT:
        Cvar_SetValue("psq_infighting", (float) !PSQ_Opt("psq_infighting"));
        break;
    case PSQ_ROW_ALLUSERS:
        Cvar_SetValue("psq_allusers", (float) !PSQ_Opt("psq_allusers"));
        break;
    case PSQ_ROW_LABELS:
        Cvar_SetValue("psq_labels", (float) !PSQ_Opt("psq_labels"));
        break;
    case PSQ_ROW_PID:
        Cvar_SetValue("psq_showpid", (float) !PSQ_Opt("psq_showpid"));
        break;
    case PSQ_ROW_RANGE:
        v = (PSQ_Opt("psq_labelrange") + dir + PSD_LABELRANGE_NUM)
            % PSD_LABELRANGE_NUM;
        Cvar_SetValue("psq_labelrange", (float) v);
        break;
    case PSQ_ROW_CREDITS:
        m_state = m_psq_credits;
        m_entersound = true;
        break;
    }
}

void M_PSQ_Key(int key)
{
    switch (key)
    {
    case K_ESCAPE:
    case K_BBUTTON:
        M_Menu_Options_f();
        break;
    case K_UPARROW:
        S_LocalSound("misc/menu1.wav");
        if (--psq_menu_cursor < 0)
            psq_menu_cursor = PSQ_ROWS - 1;
        break;
    case K_DOWNARROW:
        S_LocalSound("misc/menu1.wav");
        if (++psq_menu_cursor >= PSQ_ROWS)
            psq_menu_cursor = 0;
        break;
    case K_LEFTARROW:
        PSQ_Adjust(-1);
        break;
    case K_RIGHTARROW:
    case K_ENTER:
    case K_KP_ENTER:
    case K_ABUTTON:
        PSQ_Adjust(1);
        break;
    }
}

/* ------------------------------------------------- the live-kill warning */

static const char *const psq_warn_lines[] = {
    "Killing a process monster sends",
    "SIGTERM to its real process: that",
    "app or tool quits, and unsaved",
    "work in it can be lost. There is",
    "no in-game confirmation and no",
    "undo. Live kill lasts until you",
    "quit; every launch starts renice.",
};
#define PSQ_WARN_LINES ((int) (sizeof(psq_warn_lines) / sizeof(psq_warn_lines[0])))

void M_PSQ_LiveWarn_Draw(void)
{
    const char *extra[2];
    int n_extra = 0;
    int i;
    int y = 48;
    int lines;

    /* The saved options that widen what a kill reaches, when they are on. */
    if (PSQ_Opt("psq_infighting"))
        extra[n_extra++] = "Infighting on: monsters' kills too.";
    if (PSQ_Opt("psq_allusers"))
        extra[n_extra++] = "All users on: other users' too.";
    lines = PSQ_WARN_LINES + n_extra;

    M_PSQ_Draw();
    M_DrawTextBox(8, y - 8, 36, lines + 5);
    PSQ_PrintCentred(y, "LIVE KILL WARNING", true);
    for (i = 0; i < PSQ_WARN_LINES; i++)
        M_Print(24, y + 16 + 8 * i, psq_warn_lines[i]);
    for (i = 0; i < n_extra; i++)
        M_PrintWhite(24, y + 16 + 8 * (PSQ_WARN_LINES + i), extra[i]);
    PSQ_PrintCentred(y + 24 + 8 * lines, "Press Y to turn live kill on,", true);
    PSQ_PrintCentred(y + 32 + 8 * lines, "any other key to cancel.", true);
}

static void PSQ_LiveWarn_Close(qboolean confirmed)
{
    m_state = m_psquake;
    m_entersound = true;
    if (confirmed)
        PSQ_SetKillMode(PSD_KILL_LIVE, true);
}

/* Only the typed character Y confirms (M_PSQ_LiveWarn_Char: text input
 * follows the keyboard layout, as the quit prompt's does); any other
 * character, and every key that types none (Escape, Enter, arrows, mouse
 * and gamepad buttons), cancels. Key events for printable keys are left to
 * the character that follows them: they are mapped by physical position. */
void M_PSQ_LiveWarn_Key(int key)
{
    if (key >= 32 && key < 127)
        return;
    PSQ_LiveWarn_Close(false);
}

void M_PSQ_LiveWarn_Char(int key)
{
    PSQ_LiveWarn_Close(key == 'y' || key == 'Y');
}

/* --------------------------------------------------------------- credits */

static const char *const psq_credit_lines[] = {
    "psQuake " PSQ_VERSION " (MIT licence)",
    "after psDooM by Dennis Chao",
    "",
    "Quake by id Software; its engine",
    "here is QuakeSpasm 0.97.0: GPL-2+",
    "LibreQuake QuakeC: GPL-2",
    "LibreQuake game data: BSD-3",
    "SDL 2.0.22: zlib licence",
    "libogg, libvorbis: BSD-3 (Xiph)",
    "Build tools, not shipped:",
    "  fteqcc (GPL-2) compiles QuakeC",
    "  ericw-tools (GPL-3+), the arena",
    "",
    "The app as a whole is GPL-2.",
    "Licence texts are in psQuake.app/",
    "Contents/Resources/Licenses.",
};
#define PSQ_CREDIT_LINES ((int) (sizeof(psq_credit_lines) / sizeof(psq_credit_lines[0])))

void M_PSQ_Credits_Draw(void)
{
    int i;

    M_DrawTransPic(16, 4, Draw_CachePic("gfx/qplaque.lmp"));
    PSQ_PrintCentred(16, "psQuake credits and licences", true);
    for (i = 0; i < PSQ_CREDIT_LINES; i++)
        M_Print(32, 36 + 8 * i, psq_credit_lines[i]);
    PSQ_PrintCentred(36 + 8 * PSQ_CREDIT_LINES + 8, "Any key: back", false);
}

void M_PSQ_Credits_Key(int key)
{
    (void) key;
    m_state = m_psquake;
    m_entersound = true;
}
