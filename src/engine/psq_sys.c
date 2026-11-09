/*
 * psQuake -- engine glue for the user root and game data (psq_data.h).
 *
 * Called from Sys_Init (sys_sdl_unix.c) in place of upstream's
 * basedir/userdir setup. Probes the file system, lets psq_data_select()
 * choose, creates the directories psQuake writes to, and fills in
 * host_parms->basedir and ->userdir. The engine then reads id1 from basedir
 * and userdir and writes only under userdir (COM_AddGameDirectory).
 *
 * MIT licensed, like the rest of psQuake's own files. It is compiled only
 * into the GPL-2 engine binary.
 */

#include "quakedef.h"
#include "psq_data.h"

#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

typedef char psq_path_max_matches_engine[(PSQ_PATH_MAX == MAX_OSPATH) ? 1 : -1];

static char psq_root[PSQ_PATH_MAX];
static char psq_bundle_res[PSQ_PATH_MAX];
static psq_data_choice psq_choice;

static int psq_is_file(const char *dir, const char *rel)
{
    char path[PSQ_PATH_MAX * 2];
    struct stat st;

    if (q_snprintf(path, sizeof(path), "%s/%s", dir, rel) >= (int)sizeof(path))
        return 0;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int psq_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* 1 if the pak at <dir>/<rel> lists name. Reads only the header and the
 * directory, bounded by PSQ_PAK_MAX_DIRLEN; anything malformed is 0. */
static int psq_pak_has(const char *dir, const char *rel, const char *name)
{
    char path[PSQ_PATH_MAX * 2];
    unsigned char hdr[PSQ_PAK_HEADER_LEN];
    unsigned char *buf;
    unsigned long dirofs, dirlen;
    struct stat st;
    int fd, found = 0;

    if (q_snprintf(path, sizeof(path), "%s/%s", dir, rel) >= (int)sizeof(path))
        return 0;
    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)
        && pread(fd, hdr, sizeof(hdr), 0) == (ssize_t)sizeof(hdr)
        && psq_pak_header(hdr, (unsigned long long)st.st_size, &dirofs,
                          &dirlen)
        && (buf = malloc(dirlen)) != NULL) {
        if (pread(fd, buf, dirlen, (off_t)dirofs) == (ssize_t)dirlen)
            found = psq_pak_dir_has(buf, dirlen, name);
        free(buf);
    }
    close(fd);
    return found;
}

/* Contents/Resources of the bundle this executable runs from, or "". */
static void psq_find_bundle(void)
{
#ifdef PSQ_TEST_RESOURCES
    /* Test builds (not a bundle): a build-tree folder laid out like
     * Contents/Resources. */
    q_strlcpy(psq_bundle_res, PSQ_TEST_RESOURCES, sizeof(psq_bundle_res));
    if (!psq_is_dir(psq_bundle_res))
        psq_bundle_res[0] = '\0';
    return;
#else
    char exe[PATH_MAX];
    char real[PATH_MAX];
    uint32_t size = sizeof(exe);

    psq_bundle_res[0] = '\0';
    if (_NSGetExecutablePath(exe, &size) != 0 || realpath(exe, real) == NULL)
        return;
    if (!psq_bundle_resources_from_exe(real, psq_bundle_res,
                                       sizeof(psq_bundle_res))
        || !psq_is_dir(psq_bundle_res))
        psq_bundle_res[0] = '\0';
#endif
}

void PSQ_AddGameCode(void)
{
    char dir[PSQ_PATH_MAX];
    searchpath_t *search;

    if (!psq_bundle_res[0]
        || q_snprintf(dir, sizeof(dir), "%s/psquake", psq_bundle_res)
               >= (int)sizeof(dir)
        || !psq_is_file(dir, "progs.dat")) {
        Sys_Printf("psQuake: no psQuake game code (Resources/psquake/"
                   "progs.dat); playing without process monsters\n");
        return;
    }
    /* A loose directory, searched before id1's paks, with id1's path_id so
     * the engine treats it as part of the base game. A -game mod added later
     * is searched before it, as it is before id1. */
    search = (searchpath_t *) Z_Malloc(sizeof(searchpath_t));
    search->path_id = com_searchpaths ? com_searchpaths->path_id : 1U;
    q_strlcpy(search->filename, dir, sizeof(search->filename));
    search->next = com_searchpaths;
    com_searchpaths = search;
    Sys_Printf("psQuake: game code from %s\n", dir);
}

void PSQ_InitPaths(quakeparms_t *parms)
{
    struct passwd *pw;
    const char *home = NULL;
    psq_root_status st;
    psq_data_facts facts;

    /* The account's home from the user database, not $HOME: an inherited
     * HOME must not move where psQuake writes. -psquake-root is the
     * explicit override. */
    /* Upstream's -basedir would move every read and, with registered data,
     * every write outside the user root (even into the bundle). */
    if (COM_CheckParm("-basedir"))
        Sys_Error("psQuake: -basedir is not supported; use "
                  PSQ_ROOT_FLAG " DIR for another data folder");

    pw = getpwuid(getuid());
    if (pw != NULL)
        home = pw->pw_dir;

    st = psq_root_resolve(com_argc, com_argv, home, psq_root, sizeof(psq_root));
    if (st != PSQ_ROOT_OK)
        Sys_Error("psQuake: %s", psq_root_status_str(st));

    psq_find_bundle();

    facts.root = psq_root;
    facts.bundle_res = psq_bundle_res[0] ? psq_bundle_res : NULL;
    facts.root_has_pak0 = psq_is_file(psq_root, GAMENAME "/pak0.pak");
    facts.root_has_pak1 = psq_is_file(psq_root, GAMENAME "/pak1.pak");
    facts.root_has_pop =
        psq_pak_has(psq_root, GAMENAME "/pak0.pak", PSQ_POP_LMP)
        || psq_pak_has(psq_root, GAMENAME "/pak1.pak", PSQ_POP_LMP)
        || psq_is_file(psq_root, GAMENAME "/" PSQ_POP_LMP);
    facts.bundle_has_librequake =
        facts.bundle_res != NULL
        && psq_is_file(facts.bundle_res, GAMENAME "/pak0.pak");

    if (psq_data_select(&facts, &psq_choice) != 0)
        Sys_Error("psQuake: the data directory path is too long:\n%s",
                  psq_root);

    if (psq_choice.shareware_ignored)
        Sys_Printf("psQuake: ignoring shareware Quake in %s/" GAMENAME
                   " (pak0.pak without pak1.pak or gfx/pop.lmp).\n"
                   "psQuake does not run on shareware data.\n",
                   psq_root);

    switch (psq_choice.source) {
    case PSQ_DATA_REGISTERED:
        Sys_Printf("psQuake: registered Quake from %s/" GAMENAME "\n",
                   psq_choice.basedir);
        /* One directory: userdir must be the same pointer as basedir, or
         * COM_AddGameDirectory adds id1 twice (common.c compares them). */
        parms->basedir = psq_choice.basedir;
        parms->userdir = parms->basedir;
        break;
    case PSQ_DATA_LIBREQUAKE:
        Sys_mkdir(psq_root);
        Sys_mkdir(psq_choice.userdir);
        Sys_Printf("psQuake: LibreQuake from %s/" GAMENAME "\n",
                   psq_choice.basedir);
        parms->basedir = psq_choice.basedir;
        parms->userdir = psq_choice.userdir;
        break;
    case PSQ_DATA_NONE:
    default:
        Sys_Error("psQuake found no game data.\n\n"
                  "Put registered Quake (its pak files) in\n"
                  "%s/" GAMENAME "\n"
                  "or build psQuake with LibreQuake: run "
                  "tools/fetch-librequake.sh, then rebuild.",
                  psq_root);
    }
    Sys_Printf("psQuake: config, saves and logs in %s\n", parms->userdir);
}
