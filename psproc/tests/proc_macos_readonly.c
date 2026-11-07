/*
 * psQuake -- the native macOS backend, built READ-ONLY for host tests.
 *
 * psproc_select_test checks proc_macos_list / proc_macos_lookup against the
 * live process table, so it needs the native backend's code. This file
 * compiles psproc/src/macos/proc_macos.c unchanged, with its only two process
 * actions bound to local stubs that abort() instead of acting:
 *
 *     setpriority(2) in proc_macos_renice  ->  psproc_readonly_setpriority
 *     kill(2) SIGTERM in proc_macos_kill   ->  psproc_readonly_kill
 *
 * The system headers proc_macos.c uses are included first, so their
 * declarations keep the real names and only the two call sites are renamed.
 * Keep this list in step with proc_macos.c's includes after each re-import:
 * a header first parsed below the #defines fails to compile or is caught by
 * the check below.
 *
 * The ctest psproc_native_actions_unlinked proves the result with nm
 * (psproc/tests/native_actions_unlinked.cmake): this object's undefined
 * symbols must all be on an allowlist (no setpriority, no kill, nothing new
 * such as proc_terminate until it is reviewed), and no test binary
 * references setpriority. Every host test runs only after that check passes.
 *
 * psDoom has no such file (its test links the native backend as is); it is a
 * psQuake addition, outside tools/psproc-drift.sh's path map.
 */

#include "proc_backend.h"
#include "proc_macos.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <libproc.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach/mach_time.h>

static int psproc_readonly_setpriority(int which, id_t who, int prio)
{
    (void) which;
    (void) who;
    (void) prio;
    fprintf(stderr, "psproc test: native setpriority reached; aborting\n");
    abort();
}

static int psproc_readonly_kill(pid_t pid, int sig)
{
    (void) pid;
    (void) sig;
    fprintf(stderr, "psproc test: native kill reached; aborting\n");
    abort();
}

#define setpriority psproc_readonly_setpriority
#define kill        psproc_readonly_kill

#include "proc_macos.c"
