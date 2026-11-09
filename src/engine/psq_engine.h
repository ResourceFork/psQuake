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
/* 1 when process monsters may act on the level now: the predicate every
 * builtin applies to the policy's gate, asked afresh (psq_labels_draw.c). */
int PSQ_ProcessGateAllows(void);
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
