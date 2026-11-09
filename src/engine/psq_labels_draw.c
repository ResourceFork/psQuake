/*
 * psQuake -- SCR_DrawProcessLabels: each live process monster's name, and
 * its PID when psq_showpid is on, drawn above it (psDoom's labels).
 *
 * Called from SCR_UpdateScreen (gl_screen.c) right after the crosshair, in
 * the 2D pass, so it never runs while a level loads, a dialog is up or an
 * intermission or finale screen is drawn. It also draws nothing unless a
 * local server is running the level the client shows, outside demo
 * playback: process monsters exist only on the local server (a demo or a
 * remote game carries no process names). Nor while a menu is open, whose
 * text the labels would run into, or during an intermission under the
 * console.
 *
 * Which monsters: the client's visible entities this frame
 * (cl_visedicts), whose index is the server edict number. From the edict it
 * reads only psq_pid (offset from GetEdictFieldValue, bounds-checked
 * against the progs' entity size) and health. The name is never read from
 * QuakeC: it is the process policy's own sanitized name for that pid in the
 * current poll (psq_policy_name), so QuakeC cannot put other text on
 * screen, and no QuakeC string index is ever followed here. A monster whose
 * pid is not in the poll (it is about to teleport out), a corpse and a
 * swarm member (psq_pid -1) get no label.
 *
 * Hidden when: beyond psq_labelrange (with a fade before it), outside the
 * view frustum (R_CullModelForEntity, as the renderer culls), the anchor is
 * behind the eye, or the line from the eye is blocked by the world or a
 * solid brush model such as a door to both the anchor above its head and
 * its centre. The anchor follows the model where the renderer drew it
 * (move interpolation included), at its standing height.
 *
 * The maths (projection, fade, text, layout) is psq_labels.c.
 *
 * MIT licensed, like the rest of psQuake's own files. It is compiled only
 * into the GPL-2 engine binary.
 */

#include "quakedef.h"
#include "psq_engine.h"
#include "psq_labels.h"
#include "psq_policy.h"
#include "psproc_options.h"

#include <math.h>

/* psq_labels.c's range table is indexed by psproc's PSD_LABELS_* order. */
typedef char psq_labels_range_order[
    (PSD_LABELS_NEAR == 0 && PSD_LABELS_NORMAL == 1 && PSD_LABELS_FAR == 2
     && PSD_LABELS_ALL == 3) ? 1 : -1];

void SCR_DrawProcessLabels (void);

extern float r_fovx, r_fovy;   /* gl_rmain.c: the fov the frame used */
extern cvar_t r_lerpmove;       /* gl_rmain.c */

static psq_label_trace_t psq_trace[PSQ_LABEL_TRACE_MAX];
static int               psq_trace_n;
static psq_labels_gate_t psq_trace_gate = PSQ_LABELS_NO_SERVER;
static int               psq_trace_frame = -1;

const psq_label_trace_t *PSQ_LabelsLastFrame (int *count,
                                              psq_labels_gate_t *gate,
                                              int *frame)
{
	if (frame)
		*frame = psq_trace_frame;
	if (count)
		*count = psq_trace_n;
	if (gate)
		*gate = psq_trace_gate;
	return psq_trace;
}

static psq_labels_gate_t PSQ_LabelsGate (void)
{
	if (!psproc_labels_enabled ())
		return PSQ_LABELS_OFF;
	if (cls.demoplayback)
		return PSQ_LABELS_DEMO;
	if (!sv.active || cls.state != ca_connected)
		return PSQ_LABELS_NO_SERVER;
	if (cls.signon != SIGNONS || !cl.worldmodel
	    || cl.worldmodel != sv.worldmodel)
		return PSQ_LABELS_NOT_IN_GAME;
	if (cl.intermission)
		return PSQ_LABELS_INTERMISSION;
	if (key_dest == key_menu)
		return PSQ_LABELS_MENU;   /* label text would clash with the menu's */
	/* The policy's gate is set only when a builtin runs; asked afresh here,
	 * a level that just became one where process monsters may not act (say
	 * psq_testarena 0 while paused) shows no label from the last poll. */
	if (!PSQ_ProcessGateAllows ())
		return PSQ_LABELS_GATED;
	return PSQ_LABELS_OPEN;
}

/* psq_pid's offset in entity fields (4-byte units), or -1 if the progs has
 * no such field or it lies outside the entity. GetEdictFieldValue caches
 * the lookup and drops its cache whenever progs are loaded. */
static int PSQ_PidFieldOffset (void)
{
	edict_t *world;
	eval_t  *v;
	ptrdiff_t ofs;

	if (!progs || sv.num_edicts < 1)
		return -1;
	world = EDICT_NUM (0);
	v = GetEdictFieldValue (world, "psq_pid");
	if (!v)
		return -1;
	ofs = (const byte *) v - (const byte *) &world->v;
	if (ofs < 0 || ofs % 4 != 0)
		return -1;
	ofs /= 4;
	if (ofs >= progs->entityfields)
		return -1;
	return (int) ofs;
}

/* The policy's name for pid in the current poll, or NULL. */
static const char *PSQ_PolicyNameForPid (int pid)
{
	int i;
	const psd_proc_t *p;

	for (i = 0; (p = psq_policy_entry (i)) != NULL; i++)
	{
		if (p->pid == pid)
			return psq_policy_name (i);
	}
	return NULL;
}

/* 1 if a point trace from a to b through hull's clipping nodes, moved by
 * -offset, hits nothing. */
static qboolean PSQ_LabelHullClear (hull_t *hull, const vec3_t offset,
				    vec3_t a, vec3_t b)
{
	trace_t tr;
	vec3_t  s, e;

	VectorSubtract (a, offset, s);
	VectorSubtract (b, offset, e);
	memset (&tr, 0, sizeof (tr));
	tr.fraction = 1;
	tr.allsolid = true;
	VectorCopy (e, tr.endpos);
	SV_RecursiveHullCheck (hull, hull->firstclipnode, 0, 1, s, e, &tr);
	return !tr.allsolid && !tr.startsolid && tr.fraction >= 1.0f;
}

/* 1 if the line from the eye to p is clear of the world and of solid brush
 * models (doors, lifts, func_walls). It traces each hull itself, as
 * SV_ClipMoveToEntity would, but only for entities the server's own rule
 * accepts (SOLID_BSP, MOVETYPE_PUSH, a brush model), so a malformed mod
 * entity is skipped instead of raising Host_Error from inside the frame. */
static qboolean PSQ_LabelLineClear (vec3_t eye, vec3_t p)
{
	vec3_t lo, hi;
	int    i, k;

	if (!sv.worldmodel || sv.worldmodel->type != mod_brush)
		return false;
	if (!PSQ_LabelHullClear (&sv.worldmodel->hulls[0], vec3_origin, eye, p))
		return false;
	for (k = 0; k < 3; k++)
	{
		lo[k] = q_min (eye[k], p[k]);
		hi[k] = q_max (eye[k], p[k]);
	}
	for (i = 1; i < sv.num_edicts; i++)
	{
		edict_t  *ed = EDICT_NUM (i);
		qmodel_t *m;
		int       mi;

		if (ed->free || ed->v.solid != SOLID_BSP
		    || ed->v.movetype != MOVETYPE_PUSH)
			continue;
		mi = (int) ed->v.modelindex;
		if (mi < 1 || mi >= MAX_MODELS || !(m = sv.models[mi])
		    || m->type != mod_brush)
			continue;
		if (ed->v.absmin[0] > hi[0] || ed->v.absmax[0] < lo[0]
		    || ed->v.absmin[1] > hi[1] || ed->v.absmax[1] < lo[1]
		    || ed->v.absmin[2] > hi[2] || ed->v.absmax[2] < lo[2])
			continue;
		if (!PSQ_LabelHullClear (&m->hulls[0], ed->v.origin, eye, p))
			return false;
	}
	return true;
}

static void PSQ_LabelsBuildView (psq_label_view_t *v)
{
	VectorCopy (r_refdef.vieworg, v->origin);
	VectorCopy (vpn, v->forward);
	VectorCopy (vright, v->right);
	VectorCopy (vup, v->up);
	v->fov_x = r_fovx;
	v->fov_y = r_fovy;
	v->x = r_refdef.vrect.x;
	v->y = r_refdef.vrect.y;
	v->width = r_refdef.vrect.width;
	v->height = r_refdef.vrect.height;
}

/* The top of the model's first frame (its standing pose for every Quake
 * monster) in model units above its origin, as the renderer scales it.
 * Not the box over every frame, which can be far taller (a dog's leap),
 * and not the current frame, whose top would make the label bob as the
 * monster animates. Falls back to the model's box. */
static float PSQ_LabelModelTop (entity_t *e)
{
	aliashdr_t *hdr;

	hdr = (aliashdr_t *) Mod_Extradata (e->model);
	if (!hdr || hdr->numframes <= 0)
		return e->model->maxs[2];
	return hdr->frames[0].bboxmax.v[2] * hdr->scale[2]
		+ hdr->scale_origin[2];
}

/* Where the renderer drew the entity this frame: R_SetupEntityTransform
 * blends a stepping monster from its previous to its newest origin over
 * 0.1 s with r_lerpmove on, while e->origin is already the newest. Read
 * only; the renderer has already advanced the lerp for this frame. */
static void PSQ_LabelDrawnOrigin (entity_t *e, vec3_t out)
{
	float blend, span;
	int   k;

	VectorCopy (e->origin, out);
	if (!r_lerpmove.value || !(e->lerpflags & LERP_MOVESTEP)
	    || (e->lerpflags & LERP_RESETMOVE)
	    || !VectorCompare (e->origin, e->currentorigin))
		return;
	span = (e->lerpflags & LERP_FINISH)
		? (float) (e->lerpfinish - e->movelerpstart) : 0.1f;
	blend = span > 0 ? (float) (cl.time - e->movelerpstart) / span : 1.0f;
	blend = CLAMP (0.0f, blend, 1.0f);
	for (k = 0; k < 3; k++)
		out[k] = e->previousorigin[k]
			+ (e->currentorigin[k] - e->previousorigin[k]) * blend;
}

/* Decide one entity. Returns 1 and fills *t when it is a live-or-dead
 * process monster (traced); 0 for anything else. */
static int PSQ_LabelDecide (const psq_label_view_t *view, entity_t *e,
			    int ent, int pid_ofs, int scale,
			    psq_label_trace_t *t)
{
	edict_t    *ed;
	float       pidf;
	int         pid;
	const char *pname;
	char        name[PSQ_LABEL_TEXT_MAX];
	char        pidtext[PSQ_LABEL_TEXT_MAX];
	vec3_t      origin, anchor, centre, d;
	float       entscale, depth;

	ed = EDICT_NUM (ent);
	if (ed->free)
		return 0;
	pidf = ((const float *) &ed->v)[pid_ofs];
	pid = psq_label_pid (pidf);
	if (!pid)
		return 0;   /* ordinary entity, swarm member, corpse tag */
	if (!e->model || e->model->type != mod_alias)
		return 0;

	memset (t, 0, sizeof (*t));
	t->entity = ent;
	t->pid = pid;

	if (!(ed->v.health > 0))
	{
		t->verdict = PSQ_LABEL_DEAD;
		return 1;
	}
	pname = PSQ_PolicyNameForPid (pid);
	if (!pname)
	{
		t->verdict = PSQ_LABEL_UNLISTED;
		return 1;
	}

	entscale = ENTSCALE_DECODE (e->scale);
	PSQ_LabelDrawnOrigin (e, origin);
	VectorCopy (origin, anchor);
	anchor[2] += PSQ_LabelModelTop (e) * entscale + PSQ_LABEL_ANCHOR_UP;
	VectorSubtract (anchor, view->origin, d);
	t->distance = VectorLength (d);
	t->alpha = psq_label_alpha (psproc_label_range, t->distance);
	if (t->alpha < PSQ_LABEL_MIN_ALPHA)
	{
		t->verdict = PSQ_LABEL_FADED;
		return 1;
	}
	if (R_CullModelForEntity (e))
	{
		t->verdict = PSQ_LABEL_CULLED;
		return 1;
	}
	if (!psq_label_project (view, anchor, &t->sx, &t->sy, &depth))
	{
		t->verdict = PSQ_LABEL_BEHIND;
		return 1;
	}
	/* The box can reach into the view while the head is beside it: no
	 * label pinned to the edge over some other monster. */
	if (!psq_label_anchor_in_view (view, t->sx))
	{
		t->verdict = PSQ_LABEL_CULLED;
		return 1;
	}

	centre[0] = origin[0];
	centre[1] = origin[1];
	centre[2] = origin[2]
		+ (e->model->mins[2] + e->model->maxs[2]) * 0.5f * entscale;
	if (!PSQ_LabelLineClear (r_refdef.vieworg, anchor)
	    && !PSQ_LabelLineClear (r_refdef.vieworg, centre))
	{
		t->verdict = PSQ_LABEL_OCCLUDED;
		return 1;
	}

	psq_label_sanitize (name, sizeof (name), pname, PSQ_LABEL_TEXT_MAX);
	pidtext[0] = '\0';
	if (psproc_pid_enabled ())
		q_snprintf (pidtext, sizeof (pidtext), "%d", pid);
	if (!psq_label_layout (view, t->sx, t->sy, scale, pidtext, name,
			       &t->layout))
	{
		t->verdict = PSQ_LABEL_NO_ROOM;
		return 1;
	}
	t->verdict = PSQ_LABEL_DRAWN;
	return 1;
}

static void PSQ_LabelDrawRow (int x, int y, int scale, const char *text)
{
	glPushMatrix ();
	glTranslatef ((float) x, (float) y, 0.0f);
	glScalef ((float) scale, (float) scale, 1.0f);
	Draw_String (0, 0, text);
	glPopMatrix ();
}

void SCR_DrawProcessLabels (void)
{
	psq_label_view_t view;
	psq_label_trace_t *order[PSQ_LABEL_TRACE_MAX];
	psq_label_layout_t blocks[PSQ_LABEL_TRACE_MAX];
	int  kept[PSQ_LABEL_TRACE_MAX];
	const psq_label_layout_t *l;
	int  norder = 0;
	int  pid_ofs;
	int  scale;
	int  i, j, r;

	psq_trace_n = 0;
	psq_trace_frame = host_framecount;
	psq_trace_gate = PSQ_LabelsGate ();
	if (psq_trace_gate != PSQ_LABELS_OPEN)
		return;
	pid_ofs = PSQ_PidFieldOffset ();
	if (pid_ofs < 0)
		return;

	PSQ_LabelsBuildView (&view);
	/* The status bar's own scale (GL_SetCanvas, CANVAS_SBAR). */
	scale = psq_label_text_scale (view.height,
		CLAMP (1.0f, scr_sbarscale.value, (float) glwidth / 320.0f));

	for (i = 0; i < cl_numvisedicts && psq_trace_n < PSQ_LABEL_TRACE_MAX; i++)
	{
		entity_t *e = cl_visedicts[i];
		ptrdiff_t ent;

		/* Only entities from cl_entities have an edict: static and
		 * temporary entities are elsewhere. */
		if (e < cl_entities || e >= cl_entities + cl_max_edicts)
			continue;
		ent = e - cl_entities;
		if (ent <= 0 || ent == cl.viewentity || ent >= sv.num_edicts)
			continue;
		if (PSQ_LabelDecide (&view, e, (int) ent, pid_ofs, scale,
				     &psq_trace[psq_trace_n]))
			psq_trace_n++;
	}

	/* Nearest first: nearer labels keep their place, farther ones move
	 * up out of their way or are dropped (psq_label_declutter). */
	for (i = 0; i < psq_trace_n; i++)
	{
		if (psq_trace[i].verdict != PSQ_LABEL_DRAWN)
			continue;
		for (j = norder; j > 0 && order[j - 1]->distance
					     > psq_trace[i].distance; j--)
			order[j] = order[j - 1];
		order[j] = &psq_trace[i];
		norder++;
	}
	if (norder == 0)
		return;
	for (i = 0; i < norder; i++)
		blocks[i] = order[i]->layout;
	psq_label_declutter (&view, blocks, norder, kept);
	for (i = 0; i < norder; i++)
	{
		if (kept[i])
			order[i]->layout = blocks[i];
		else
		{
			order[i]->verdict = PSQ_LABEL_CROWDED;
			memset (&order[i]->layout, 0, sizeof (order[i]->layout));
		}
	}

	GL_SetCanvas (CANVAS_DEFAULT);
	glDisable (GL_ALPHA_TEST);
	glEnable (GL_BLEND);
	/* QuakeSpasm draws 2D with GL_REPLACE, which ignores glColor: modulate,
	 * so the fade and the black shadow apply (as Draw_ConsoleBackground). */
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	for (i = norder - 1; i >= 0; i--)   /* far first: near ones on top */
	{
		const psq_label_trace_t *t = order[i];

		if (!kept[i])
			continue;
		l = &t->layout;
		/* A one-step drop shadow keeps the text readable on bright
		 * walls. */
		glColor4f (0.0f, 0.0f, 0.0f, t->alpha * 0.75f);
		for (r = 0; r < l->rows; r++)
			PSQ_LabelDrawRow (l->x[r] + l->scale, l->y[r] + l->scale,
					  l->scale, l->text[r]);
		glColor4f (1.0f, 1.0f, 1.0f, t->alpha);
		for (r = 0; r < l->rows; r++)
			PSQ_LabelDrawRow (l->x[r], l->y[r], l->scale, l->text[r]);
	}
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor4f (1.0f, 1.0f, 1.0f, 1.0f);
	glDisable (GL_BLEND);
	glEnable (GL_ALPHA_TEST);
}
