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

/* psq_sys.c: put psQuake's progs.dat (the bundle's Resources/psquake) ahead
 * of the game data in the base search path. Called from
 * COM_InitFilesystem right after id1 is added. */
void PSQ_AddGameCode(void);

/* psq_builtins.c: the process-monster builtins and their rules. */
#define PSQ_BUILTIN_BASE 1200
void PSQ_Init(void);              /* Host_Init, after PR_Init: cvars, backend */
void PSQ_LevelReset(void);        /* SV_SpawnServer, after PR_LoadProgs      */
int PSQ_SaveRule(void);           /* psq_save_rule (psq_arena.h) for this level */
void PSQ_AfterLoad(void);         /* Host_Loadgame_f, after the edicts load  */
void PSQ_StuffText(const char *text);      /* cl_parse.c svc_stufftext   */
qboolean PSQ_CvarSetRefused(const char *name); /* pr_cmds.c PF_cvar_set  */
void PSQ_QuakeCCommand(const char *text);      /* PF_localcmd, PF_stuffcmd */
qboolean PSQ_ChangelevelRefused(const char *map); /* PF_changelevel      */
/* 1 when process monsters may act on the level now: the predicate every
 * builtin applies to the policy's gate, asked afresh (psq_labels_draw.c). */
int PSQ_ProcessGateAllows(void);
/* The kill mode (PSD_KILL_*): renice at every launch, never saved, changed
 * only by the Options > psQuake page (psq_menu.c) through PSQ_SetKillMode,
 * which returns a psq_killmode_result_t (psq_policy.h). live_confirmed is
 * passed only by the live-kill warning answered yes. */
int PSQ_SetKillMode(int mode, qboolean live_confirmed);
int PSQ_KillMode(void);
/* A server, demo or game code ran commands this session (options may then
 * only go down). */
qboolean PSQ_SessionUntrusted(void);

/* psq_menu.c: the Options > psQuake page, its live-kill warning and the
 * credits, drawn and keyed from menu.c's M_Draw and M_Keydown. */
void M_Menu_PSQ_f(void);
void M_PSQ_Draw(void);
void M_PSQ_Key(int key);
void M_PSQ_LiveWarn_Draw(void);
void M_PSQ_LiveWarn_Key(int key);
void M_PSQ_LiveWarn_Char(int key);   /* M_Charinput; M_TextEntry is on */
void M_PSQ_Credits_Draw(void);
void M_PSQ_Credits_Key(int key);
/* The max-live-monsters slider's fill, 0 at PSD_MONSTER_CAP_MIN to 1 at
 * PSD_MONSTER_CAP_MAX; a cap outside the range is clamped first. */
float PSQ_CapSliderFraction(int cap);
/* The page's layout in menu-canvas pixels (the options render test reads
 * it): row r's text at y PSQ_MENU_ROW_Y(r), labels right-aligned to
 * PSQ_MENU_LABEL_R, values from PSQ_MENU_VALUE_X. The cap row's slider bar
 * starts at PSQ_MENU_SLIDER_X (its left end one character before it) and
 * is M_DrawSlider's 10 characters long, its number after the right end. */
#define PSQ_MENU_ROW_Y(r)   (40 + 8 * (r))
#define PSQ_MENU_LABEL_R    160
#define PSQ_MENU_VALUE_X    184
#define PSQ_MENU_SLIDER_X   (PSQ_MENU_VALUE_X + 8)
#define PSQ_MENU_CAP_ROW    1
void PF_psq_poll(void);           /* #1200 */
void PF_psq_proc_pid(void);       /* #1201 */
void PF_psq_proc_name(void);      /* #1202 */
void PF_psq_class(void);          /* #1203 */
void PF_psq_burst(void);          /* #1204 */
void PF_psq_wound(void);          /* #1205 */
void PF_psq_kill(void);           /* #1206 */
void PF_psq_present(void);        /* #1207 */
void PF_psq_tier_class(void);     /* #1208 */
void PF_psq_grid_cell(void);      /* #1209 */
void PF_psq_grid_spot(void);      /* #1210 */

/* Test builds: called on every psq_poll with the refresh count so far. */
extern void (*psq_test_poll_hook)(int poll_count);
#ifdef PSQ_HEADLESS_TEST
void psq_test_install_backend(void);   /* tests/psq_headless_driver.c */
void psq_test_register(void);
#endif

#endif /* PSQ_ENGINE_H */
