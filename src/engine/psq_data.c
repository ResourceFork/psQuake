/*
 * psQuake -- user root and game data selection. See psq_data.h.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#include "psq_data.h"

#include <stdio.h>
#include <string.h>

/* Copy src[0..len) to out with trailing slashes removed. Returns the new
 * length, or (size_t)-1 if it would not fit with PSQ_PATH_RESERVE. */
static size_t copy_trimmed(const char *src, size_t len, char *out,
                           size_t outsize)
{
    while (len > 0 && src[len - 1] == '/')
        len--;
    if (outsize < PSQ_PATH_RESERVE || len + PSQ_PATH_RESERVE > outsize)
        return (size_t)-1;
    memcpy(out, src, len);
    out[len] = '\0';
    return len;
}

psq_root_status psq_root_resolve(int argc, char **argv, const char *home,
                                 char *out, size_t outsize)
{
    int i;
    size_t n;

    if (out == NULL || outsize == 0)
        return PSQ_ROOT_ERR_TOO_LONG;
    out[0] = '\0';

    for (i = 1; argv != NULL && i < argc; i++) {
        const char *dir;

        if (argv[i] == NULL || strcmp(argv[i], PSQ_ROOT_FLAG) != 0)
            continue;
        if (i + 1 >= argc || argv[i + 1] == NULL || argv[i + 1][0] == '\0')
            return PSQ_ROOT_ERR_MISSING_VALUE;
        dir = argv[i + 1];
        if (dir[0] != '/')
            return PSQ_ROOT_ERR_NOT_ABSOLUTE;
        n = copy_trimmed(dir, strlen(dir), out, outsize);
        if (n == (size_t)-1) {
            out[0] = '\0';
            return PSQ_ROOT_ERR_TOO_LONG;
        }
        if (n == 0)
            return PSQ_ROOT_ERR_IS_FS_ROOT;
        return PSQ_ROOT_OK;
    }

    if (home == NULL || home[0] != '/')
        return PSQ_ROOT_ERR_NO_HOME;
    n = copy_trimmed(home, strlen(home), out, outsize);
    if (n == (size_t)-1
        || n + strlen(PSQ_DEFAULT_ROOT_UNDER_HOME) + PSQ_PATH_RESERVE
               > outsize) {
        out[0] = '\0';
        return PSQ_ROOT_ERR_TOO_LONG;
    }
    memcpy(out + n, PSQ_DEFAULT_ROOT_UNDER_HOME,
           strlen(PSQ_DEFAULT_ROOT_UNDER_HOME) + 1);
    return PSQ_ROOT_OK;
}

const char *psq_root_status_str(psq_root_status st)
{
    switch (st) {
    case PSQ_ROOT_OK:
        return "ok";
    case PSQ_ROOT_ERR_MISSING_VALUE:
        return PSQ_ROOT_FLAG " needs a directory after it";
    case PSQ_ROOT_ERR_NOT_ABSOLUTE:
        return PSQ_ROOT_FLAG " needs an absolute directory (starting with /)";
    case PSQ_ROOT_ERR_IS_FS_ROOT:
        return PSQ_ROOT_FLAG " cannot be the file system root";
    case PSQ_ROOT_ERR_TOO_LONG:
        return "the psQuake data directory path is too long";
    case PSQ_ROOT_ERR_NO_HOME:
        return "cannot find the home directory; use " PSQ_ROOT_FLAG " DIR";
    }
    return "unknown error";
}

/* Length of path with its last component (and the slash) removed, or 0. */
static size_t parent_len(const char *path, size_t len)
{
    while (len > 0 && path[len - 1] != '/')
        len--;
    while (len > 1 && path[len - 1] == '/')
        len--;
    return len;
}

/* 1 if path[0..len) ends with the component name. */
static int last_is(const char *path, size_t len, const char *name)
{
    size_t nl = strlen(name);
    if (len < nl + 1)
        return 0;
    return path[len - nl - 1] == '/'
        && memcmp(path + len - nl, name, nl) == 0;
}

int psq_bundle_resources_from_exe(const char *exe_path, char *out,
                                  size_t outsize)
{
    static const char res[] = "/Resources";
    size_t len, macos, contents, app;

    if (exe_path == NULL || exe_path[0] != '/' || out == NULL)
        return 0;
    len = strlen(exe_path);
    macos = parent_len(exe_path, len);          /* .../Contents/MacOS */
    if (!last_is(exe_path, macos, "MacOS"))
        return 0;
    contents = parent_len(exe_path, macos);     /* .../X.app/Contents */
    if (!last_is(exe_path, contents, "Contents"))
        return 0;
    app = parent_len(exe_path, contents);       /* .../X.app */
    if (app < 5 || memcmp(exe_path + app - 4, ".app", 4) != 0
        || exe_path[app - 5] == '/')
        return 0;
    if (contents + sizeof(res) + PSQ_PATH_RESERVE > outsize)
        return 0;
    memcpy(out, exe_path, contents);
    memcpy(out + contents, res, sizeof(res));
    return 1;
}

static unsigned long le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

int psq_pak_header(const unsigned char hdr[PSQ_PAK_HEADER_LEN],
                   unsigned long long filesize, unsigned long *dirofs,
                   unsigned long *dirlen)
{
    unsigned long ofs, len;

    if (hdr == NULL || memcmp(hdr, "PACK", 4) != 0)
        return 0;
    ofs = le32(hdr + 4);
    len = le32(hdr + 8);
    /* The engine reads both as signed ints: refuse the top bit. */
    if (ofs > 0x7fffffffUL || len > 0x7fffffffUL)
        return 0;
    if (len == 0 || len % PSQ_PAK_ENTRY_LEN != 0 || len > PSQ_PAK_MAX_DIRLEN)
        return 0;
    if (ofs < PSQ_PAK_HEADER_LEN
        || (unsigned long long)ofs + len > filesize)
        return 0;
    if (dirofs != NULL)
        *dirofs = ofs;
    if (dirlen != NULL)
        *dirlen = len;
    return 1;
}

int psq_pak_dir_has(const unsigned char *dir, unsigned long dirlen,
                    const char *name)
{
    unsigned long i;
    size_t nl;

    if (dir == NULL || name == NULL)
        return 0;
    nl = strlen(name);
    if (nl == 0 || nl >= 56)
        return 0;
    for (i = 0; i + PSQ_PAK_ENTRY_LEN <= dirlen; i += PSQ_PAK_ENTRY_LEN) {
        const unsigned char *e = dir + i;
        if (memcmp(e, name, nl) == 0 && e[nl] == '\0')
            return 1;
    }
    return 0;
}

static int set_path(char *dst, const char *a, const char *b)
{
    int n;
    if (b != NULL)
        n = snprintf(dst, PSQ_PATH_MAX, "%s/%s", a, b);
    else
        n = snprintf(dst, PSQ_PATH_MAX, "%s", a);
    return n >= 0 && (size_t)n + PSQ_PATH_RESERVE <= PSQ_PATH_MAX;
}

int psq_data_select(const psq_data_facts *f, psq_data_choice *out)
{
    if (out == NULL)
        return -1;
    memset(out, 0, sizeof(*out));
    out->source = PSQ_DATA_NONE;
    if (f == NULL || f->root == NULL || f->root[0] != '/')
        return -1;

    out->shareware_ignored =
        f->root_has_pak0 && !f->root_has_pak1 && !f->root_has_pop;

    /* Registered Quake: pak0 with pak1 (1996), or pak0 holding pop.lmp
     * (2021 re-release). pak1 alone is incomplete data. */
    if (f->root_has_pak0 && (f->root_has_pak1 || f->root_has_pop)) {
        if (!set_path(out->basedir, f->root, NULL)
            || !set_path(out->userdir, f->root, NULL))
            return -1;
        out->source = PSQ_DATA_REGISTERED;
        return 0;
    }

    if (f->bundle_res != NULL && f->bundle_res[0] == '/'
        && f->bundle_has_librequake) {
        if (!set_path(out->basedir, f->bundle_res, NULL)
            || !set_path(out->userdir, f->root, PSQ_LIBREQUAKE_SUBDIR))
            return -1;
        out->source = PSQ_DATA_LIBREQUAKE;
        return 0;
    }

    return 0;
}
