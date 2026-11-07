/*
 * psQuake -- host tests for the user root and game data selection
 * (src/engine/psq_data.c).
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "psq_data.h"

#include <stdio.h>
#include <string.h>

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

#define ARGC(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void test_root_default(void)
{
    char out[PSQ_PATH_MAX];
    char *argv[] = { "psQuake", "-window" };

    CHECK(psq_root_resolve(ARGC(argv), argv, "/Users/pat", out, sizeof(out))
              == PSQ_ROOT_OK, "default root resolves");
    CHECK(strcmp(out, "/Users/pat/Library/Application Support/psQuake") == 0,
          "default root is under the home's Application Support: %s", out);

    CHECK(psq_root_resolve(ARGC(argv), argv, "/Users/pat/", out, sizeof(out))
              == PSQ_ROOT_OK
          && strcmp(out, "/Users/pat/Library/Application Support/psQuake")
                 == 0,
          "a trailing slash on home is dropped: %s", out);

    CHECK(psq_root_resolve(ARGC(argv), argv, NULL, out, sizeof(out))
              == PSQ_ROOT_ERR_NO_HOME, "no home and no flag is an error");
    CHECK(psq_root_resolve(ARGC(argv), argv, "relative", out, sizeof(out))
              == PSQ_ROOT_ERR_NO_HOME, "a relative home is not used");
    CHECK(psq_root_resolve(0, NULL, "/h", out, sizeof(out)) == PSQ_ROOT_OK,
          "no argv still gives the default");
}

static void test_root_flag(void)
{
    char out[PSQ_PATH_MAX];
    char *ok[] = { "psQuake", "-window", "-psquake-root", "/tmp/psq/" };
    char *missing[] = { "psQuake", "-psquake-root" };
    char *empty[] = { "psQuake", "-psquake-root", "" };
    char *rel[] = { "psQuake", "-psquake-root", "scratch/root" };
    char *flaglike[] = { "psQuake", "-psquake-root", "-window" };
    char *slash[] = { "psQuake", "-psquake-root", "///" };
    char *first[] = { "psQuake", "-psquake-root", "/a", "-psquake-root",
                      "/b" };
    char *prefix[] = { "psQuake", "-psquake-rootx", "/a" };

    CHECK(psq_root_resolve(ARGC(ok), ok, "/Users/pat", out, sizeof(out))
              == PSQ_ROOT_OK && strcmp(out, "/tmp/psq") == 0,
          "-psquake-root wins over home, trailing slash dropped: %s", out);
    CHECK(psq_root_resolve(ARGC(ok), ok, NULL, out, sizeof(out))
              == PSQ_ROOT_OK, "-psquake-root needs no home");
    CHECK(psq_root_resolve(ARGC(missing), missing, "/h", out, sizeof(out))
              == PSQ_ROOT_ERR_MISSING_VALUE, "flag without a value");
    CHECK(psq_root_resolve(ARGC(empty), empty, "/h", out, sizeof(out))
              == PSQ_ROOT_ERR_MISSING_VALUE, "flag with an empty value");
    CHECK(psq_root_resolve(ARGC(rel), rel, "/h", out, sizeof(out))
              == PSQ_ROOT_ERR_NOT_ABSOLUTE, "relative directory refused");
    CHECK(psq_root_resolve(ARGC(flaglike), flaglike, "/h", out, sizeof(out))
              == PSQ_ROOT_ERR_NOT_ABSOLUTE,
          "the next flag is not taken as the directory");
    CHECK(psq_root_resolve(ARGC(slash), slash, "/h", out, sizeof(out))
              == PSQ_ROOT_ERR_IS_FS_ROOT, "\"/\" refused");
    CHECK(psq_root_resolve(ARGC(first), first, "/h", out, sizeof(out))
              == PSQ_ROOT_OK && strcmp(out, "/a") == 0,
          "the first -psquake-root wins, as COM_CheckParm: %s", out);
    CHECK(psq_root_resolve(ARGC(prefix), prefix, "/h", out, sizeof(out))
              == PSQ_ROOT_OK
          && strcmp(out, "/h/Library/Application Support/psQuake") == 0,
          "only the exact flag counts: %s", out);
}

static void test_root_length(void)
{
    char out[PSQ_PATH_MAX];
    char longdir[PSQ_PATH_MAX + 8];
    char *argv[] = { "psQuake", "-psquake-root", longdir };
    size_t fit = PSQ_PATH_MAX - PSQ_PATH_RESERVE;

    memset(longdir, 'a', sizeof(longdir));
    longdir[0] = '/';
    longdir[fit] = '\0';
    CHECK(psq_root_resolve(ARGC(argv), argv, "/h", out, sizeof(out))
              == PSQ_ROOT_OK && strlen(out) == fit,
          "a root of exactly %zu bytes fits", fit);
    longdir[fit] = 'a';
    longdir[fit + 1] = '\0';
    CHECK(psq_root_resolve(ARGC(argv), argv, "/h", out, sizeof(out))
              == PSQ_ROOT_ERR_TOO_LONG && out[0] == '\0',
          "one byte more is refused, and out is left empty");
    CHECK(psq_root_resolve(ARGC(argv), argv, "/h", out, 10)
              == PSQ_ROOT_ERR_TOO_LONG, "a tiny buffer is refused");
}

static void test_bundle_resources(void)
{
    char out[PSQ_PATH_MAX];

    CHECK(psq_bundle_resources_from_exe(
              "/Applications/psQuake.app/Contents/MacOS/psQuake", out,
              sizeof(out))
              && strcmp(out, "/Applications/psQuake.app/Contents/Resources")
                     == 0,
          "bundle executable gives Contents/Resources: %s", out);
    CHECK(!psq_bundle_resources_from_exe("/usr/local/bin/psQuake", out,
                                         sizeof(out)),
          "a bare binary has no bundle");
    CHECK(!psq_bundle_resources_from_exe("/x/Contents/MacOS/psQuake", out,
                                         sizeof(out)),
          "Contents must sit in a .app");
    CHECK(!psq_bundle_resources_from_exe("/x/psQuake.app/MacOS/psQuake",
                                         out, sizeof(out)),
          "MacOS must sit in Contents");
    CHECK(!psq_bundle_resources_from_exe("/.app/Contents/MacOS/psQuake",
                                         out, sizeof(out)),
          "a bundle needs a name before .app");
    CHECK(!psq_bundle_resources_from_exe("relative.app/Contents/MacOS/p",
                                         out, sizeof(out)),
          "a relative path is refused");
    CHECK(!psq_bundle_resources_from_exe(NULL, out, sizeof(out)),
          "NULL is refused");
    CHECK(!psq_bundle_resources_from_exe(
              "/Applications/psQuake.app/Contents/MacOS/psQuake", out, 40),
          "a too-small buffer is refused");
}

static psq_data_choice pick_pop(int pak0, int pak1, int pop,
                                const char *bundle, int lq)
{
    psq_data_facts f;
    psq_data_choice c;

    f.root = "/Users/pat/Library/Application Support/psQuake";
    f.bundle_res = bundle;
    f.root_has_pak0 = pak0;
    f.root_has_pak1 = pak1;
    f.root_has_pop = pop;
    f.bundle_has_librequake = lq;
    CHECK(psq_data_select(&f, &c) == 0, "select succeeds");
    return c;
}

static psq_data_choice pick(int pak0, int pak1, const char *bundle, int lq)
{
    return pick_pop(pak0, pak1, pak1, bundle, lq);
}

static void test_select(void)
{
    const char *root = "/Users/pat/Library/Application Support/psQuake";
    const char *res = "/Applications/psQuake.app/Contents/Resources";
    psq_data_choice c;
    psq_data_facts bad;

    /* Registered Quake in the user root takes precedence. */
    c = pick(1, 1, res, 1);
    CHECK(c.source == PSQ_DATA_REGISTERED, "registered wins over LibreQuake");
    CHECK(strcmp(c.basedir, root) == 0 && strcmp(c.userdir, root) == 0,
          "registered reads and writes the root: %s | %s", c.basedir,
          c.userdir);
    CHECK(!c.shareware_ignored, "registered is not shareware");

    c = pick(1, 1, NULL, 0);
    CHECK(c.source == PSQ_DATA_REGISTERED, "registered needs no bundle");

    /* Shareware alone is ignored in favour of LibreQuake. */
    c = pick(1, 0, res, 1);
    CHECK(c.source == PSQ_DATA_LIBREQUAKE, "shareware gives way to LibreQuake");
    CHECK(c.shareware_ignored, "the ignored shareware is reported");
    CHECK(strcmp(c.basedir, res) == 0, "LibreQuake reads the bundle: %s",
          c.basedir);
    CHECK(strcmp(c.userdir,
                 "/Users/pat/Library/Application Support/psQuake/librequake")
              == 0,
          "LibreQuake writes beside, not into, <root>/id1: %s", c.userdir);

    /* Shareware and no LibreQuake: still never shareware. */
    c = pick(1, 0, res, 0);
    CHECK(c.source == PSQ_DATA_NONE && c.shareware_ignored,
          "shareware alone is never used");
    c = pick(1, 0, NULL, 1);
    CHECK(c.source == PSQ_DATA_NONE, "no bundle path, no LibreQuake");

    /* pak1 without pak0 is incomplete: not registered. */
    c = pick(0, 1, res, 1);
    CHECK(c.source == PSQ_DATA_LIBREQUAKE && !c.shareware_ignored,
          "pak1 alone is not registered Quake");

    /* The 2021 re-release: one pak0.pak that holds gfx/pop.lmp. */
    c = pick_pop(1, 0, 1, res, 1);
    CHECK(c.source == PSQ_DATA_REGISTERED && !c.shareware_ignored,
          "a single pak0 with pop.lmp is registered Quake");
    /* pak0 and pak1 count even if pop.lmp was not found in them. */
    c = pick_pop(1, 1, 0, res, 1);
    CHECK(c.source == PSQ_DATA_REGISTERED, "pak0 and pak1 are registered");
    /* pop.lmp loose but no pak0: incomplete. */
    c = pick_pop(0, 0, 1, res, 1);
    CHECK(c.source == PSQ_DATA_LIBREQUAKE && !c.shareware_ignored,
          "pop.lmp without pak0 is not registered Quake");

    /* Nothing in the root. */
    c = pick(0, 0, res, 1);
    CHECK(c.source == PSQ_DATA_LIBREQUAKE && !c.shareware_ignored,
          "empty root uses LibreQuake");
    c = pick(0, 0, res, 0);
    CHECK(c.source == PSQ_DATA_NONE, "nothing anywhere is NONE");

    /* Bad input. */
    CHECK(psq_data_select(NULL, &c) == -1 && c.source == PSQ_DATA_NONE,
          "NULL facts");
    memset(&bad, 0, sizeof(bad));
    bad.root = "/r";
    CHECK(psq_data_select(&bad, NULL) == -1, "NULL out");
    bad.root = "relative";
    bad.root_has_pak0 = bad.root_has_pak1 = 1;
    CHECK(psq_data_select(&bad, &c) == -1 && c.source == PSQ_DATA_NONE,
          "a relative root is refused");
    bad.root = "/r";
    bad.root_has_pak0 = bad.root_has_pak1 = 0;
    bad.bundle_res = "Resources";
    bad.bundle_has_librequake = 1;
    CHECK(psq_data_select(&bad, &c) == 0 && c.source == PSQ_DATA_NONE,
          "a relative bundle path is not used");
}

static void test_select_length(void)
{
    char root[PSQ_PATH_MAX];
    psq_data_facts f;
    psq_data_choice c;
    size_t fit = PSQ_PATH_MAX - PSQ_PATH_RESERVE;

    /* The longest root psq_root_resolve accepts, with the librequake
     * subdir appended, must be refused rather than truncated. */
    memset(root, 'r', fit);
    root[0] = '/';
    root[fit] = '\0';
    memset(&f, 0, sizeof(f));
    f.root = root;
    f.bundle_res = "/A/psQuake.app/Contents/Resources";
    f.bundle_has_librequake = 1;
    CHECK(psq_data_select(&f, &c) == -1 && c.source == PSQ_DATA_NONE,
          "a userdir that would not fit is refused");

    f.root_has_pak0 = f.root_has_pak1 = 1;
    CHECK(psq_data_select(&f, &c) == 0 && c.source == PSQ_DATA_REGISTERED
              && strlen(c.basedir) == fit,
          "the same root fits for registered data");
}

static void put_le32(unsigned char *p, unsigned long v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static void test_pak(void)
{
    unsigned char h[PSQ_PAK_HEADER_LEN];
    unsigned char dir[3 * PSQ_PAK_ENTRY_LEN];
    unsigned long ofs = 0, len = 0;

    memcpy(h, "PACK", 4);
    put_le32(h + 4, 1000);
    put_le32(h + 8, 3 * PSQ_PAK_ENTRY_LEN);
    CHECK(psq_pak_header(h, 1000 + 3 * PSQ_PAK_ENTRY_LEN, &ofs, &len)
              && ofs == 1000 && len == 3 * PSQ_PAK_ENTRY_LEN,
          "a valid header parses");
    CHECK(!psq_pak_header(h, 1000 + 3 * PSQ_PAK_ENTRY_LEN - 1, NULL, NULL),
          "a directory past the end of the file is refused");
    put_le32(h + 8, 65);
    CHECK(!psq_pak_header(h, 1u << 20, NULL, NULL),
          "a directory of partial entries is refused");
    put_le32(h + 8, 0);
    CHECK(!psq_pak_header(h, 1u << 20, NULL, NULL), "an empty directory");
    put_le32(h + 8, PSQ_PAK_MAX_DIRLEN + PSQ_PAK_ENTRY_LEN);
    CHECK(!psq_pak_header(h, 1ull << 32, NULL, NULL),
          "more entries than the engine loads are refused");
    put_le32(h + 4, 0x80000000UL);
    put_le32(h + 8, PSQ_PAK_ENTRY_LEN);
    CHECK(!psq_pak_header(h, 1ull << 33, NULL, NULL),
          "an offset the engine reads as negative is refused");
    put_le32(h + 4, 4);
    CHECK(!psq_pak_header(h, 1u << 20, NULL, NULL),
          "a directory overlapping the header is refused");
    memcpy(h, "PACX", 4);
    put_le32(h + 4, 1000);
    CHECK(!psq_pak_header(h, 1u << 20, NULL, NULL), "bad magic");
    CHECK(!psq_pak_header(NULL, 1u << 20, NULL, NULL), "NULL header");

    memset(dir, 0, sizeof(dir));
    memcpy(dir, "gfx/pop.lmpx", 12);
    memcpy(dir + PSQ_PAK_ENTRY_LEN, "gfx/pop.lm", 10);
    CHECK(!psq_pak_dir_has(dir, sizeof(dir), PSQ_POP_LMP),
          "only an exact name matches");
    memcpy(dir + 2 * PSQ_PAK_ENTRY_LEN, "gfx/pop.lmp", 11);
    CHECK(psq_pak_dir_has(dir, sizeof(dir), PSQ_POP_LMP),
          "the last entry is found");
    CHECK(!psq_pak_dir_has(dir, 2 * PSQ_PAK_ENTRY_LEN, PSQ_POP_LMP),
          "entries past dirlen are not read");
    memset(dir, 'x', PSQ_PAK_ENTRY_LEN);
    CHECK(!psq_pak_dir_has(dir, PSQ_PAK_ENTRY_LEN, "xxxx"),
          "an unterminated name does not match a prefix");
    CHECK(!psq_pak_dir_has(NULL, 64, PSQ_POP_LMP), "NULL directory");
}

int main(void)
{
    test_root_default();
    test_root_flag();
    test_root_length();
    test_bundle_resources();
    test_select();
    test_select_length();
    test_pak();

    printf("psq_data_test: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
