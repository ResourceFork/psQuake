/*
 * psQuake -- user root and game data selection (engine-free, host-tested).
 *
 * The user root holds everything psQuake writes: config, saves, the console
 * log and history. It is ~/Library/Application Support/psQuake by default,
 * or the absolute directory given with -psquake-root DIR.
 *
 * Game data, in order:
 *   1. Registered Quake the user put in <root>/id1: pak0.pak with pak1.pak
 *      (the 1996 release), or a pak0.pak that holds gfx/pop.lmp (the 2021
 *      re-release, one pak). The engine reads and writes <root>/id1.
 *   2. LibreQuake from the app bundle (Contents/Resources/id1). The engine
 *      reads the bundle and writes <root>/librequake/id1, so a shareware or
 *      partial <root>/id1 is never mixed in and the signed bundle is never
 *      written.
 *   3. Nothing: the caller reports how to provide data.
 *
 * Shareware Quake (pak0.pak, no pak1.pak, no gfx/pop.lmp) is never used:
 * the engine's own registered test is gfx/pop.lmp, and id's shareware
 * licence forbids running it with user-made content, which psQuake adds in
 * later phases.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#ifndef PSQ_DATA_H
#define PSQ_DATA_H

#include <stddef.h>

#define PSQ_ROOT_FLAG          "-psquake-root"
#define PSQ_DEFAULT_ROOT_UNDER_HOME "/Library/Application Support/psQuake"
#define PSQ_LIBREQUAKE_SUBDIR  "librequake"

/* Room kept free after any directory psQuake hands the engine, for the
 * engine's own "/id1/<file>" suffixes (QuakeSpasm keeps 50). */
#define PSQ_PATH_RESERVE 64

/* QuakeSpasm's MAX_OSPATH (PATH_MAX, 1024 on macOS); psq_sys.c checks the
 * two agree. */
#define PSQ_PATH_MAX 1024

typedef enum
{
    PSQ_ROOT_OK,
    PSQ_ROOT_ERR_MISSING_VALUE, /* -psquake-root with nothing after it */
    PSQ_ROOT_ERR_NOT_ABSOLUTE,  /* the directory does not start with '/' */
    PSQ_ROOT_ERR_IS_FS_ROOT,    /* "/" (or only slashes) */
    PSQ_ROOT_ERR_TOO_LONG,      /* would not fit with PSQ_PATH_RESERVE */
    PSQ_ROOT_ERR_NO_HOME        /* no -psquake-root and no usable home */
} psq_root_status;

/*
 * Resolve the user root from argv[1..argc) and the account's home directory
 * (from getpwuid, not $HOME). Writes a path with no trailing slash to out.
 */
psq_root_status psq_root_resolve(int argc, char **argv, const char *home,
                                 char *out, size_t outsize);

const char *psq_root_status_str(psq_root_status st);

/*
 * From the real path of the running executable, the bundle's
 * Contents/Resources directory: ".../X.app/Contents/MacOS/exe" gives
 * ".../X.app/Contents/Resources". Returns 1 on success, 0 when the
 * executable is not inside Contents/MacOS of a .app, or out is too small.
 */
int psq_bundle_resources_from_exe(const char *exe_path, char *out,
                                  size_t outsize);

/* Pak files: a 12-byte header ("PACK", directory offset, directory length,
 * little-endian), and a directory of 64-byte entries (56-byte name, offset,
 * length). The cap matches the engine's MAX_FILES_IN_PACK (2048). */
#define PSQ_PAK_HEADER_LEN 12
#define PSQ_PAK_ENTRY_LEN  64
#define PSQ_PAK_MAX_DIRLEN (2048 * PSQ_PAK_ENTRY_LEN)
#define PSQ_POP_LMP        "gfx/pop.lmp"

/*
 * Validate a pak header for a file of filesize bytes. Returns 1 and the
 * directory's offset and length when the directory is whole entries, at
 * most PSQ_PAK_MAX_DIRLEN, and inside the file; 0 otherwise.
 */
int psq_pak_header(const unsigned char hdr[PSQ_PAK_HEADER_LEN],
                   unsigned long long filesize, unsigned long *dirofs,
                   unsigned long *dirlen);

/* 1 if the pak directory (dirlen bytes) lists exactly name. */
int psq_pak_dir_has(const unsigned char *dir, unsigned long dirlen,
                    const char *name);

typedef struct
{
    const char *root;           /* resolved user root */
    const char *bundle_res;     /* Contents/Resources, or NULL */
    int root_has_pak0;          /* <root>/id1/pak0.pak is a file */
    int root_has_pak1;          /* <root>/id1/pak1.pak is a file */
    int root_has_pop;           /* gfx/pop.lmp in <root>/id1 (pak0, pak1 or
                                   a loose file) */
    int bundle_has_librequake;  /* <bundle_res>/id1/pak0.pak is a file */
} psq_data_facts;

typedef enum
{
    PSQ_DATA_NONE,
    PSQ_DATA_REGISTERED,  /* the user's registered Quake in <root>/id1 */
    PSQ_DATA_LIBREQUAKE   /* LibreQuake from the bundle */
} psq_data_source;

typedef struct
{
    psq_data_source source;
    int  shareware_ignored;      /* <root>/id1 held pak0.pak, no pak1.pak,
                                    no gfx/pop.lmp */
    char basedir[PSQ_PATH_MAX];  /* engine basedir: id1 is read from here */
    char userdir[PSQ_PATH_MAX];  /* engine userdir: every write lands here */
} psq_data_choice;

/*
 * Choose the game data. Returns 0 with out filled (source may be
 * PSQ_DATA_NONE), or -1 for bad input or a path that would not fit.
 */
int psq_data_select(const psq_data_facts *facts, psq_data_choice *out);

#endif /* PSQ_DATA_H */
