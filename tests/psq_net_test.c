/*
 * psQuake -- host tests for the UDP bind address (src/engine/psq_net.c).
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "psq_net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
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

#define ARGC(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void expect_ok(int argc, char **argv, const char *want, const char *what)
{
    uint32_t got = 0;
    struct in_addr w;

    inet_pton(AF_INET, want, &w);
    CHECK(psq_net_bind_addr(argc, argv, &got) == PSQ_BIND_OK, "%s: ok", what);
    CHECK(got == w.s_addr, "%s: got %08x, want %s", what, (unsigned)ntohl(got),
          want);
}

static void expect_err(int argc, char **argv, psq_bind_status want,
                       const char *what)
{
    uint32_t got = 0;

    CHECK(psq_net_bind_addr(argc, argv, &got) == want, "%s: status", what);
    CHECK(got == htonl(INADDR_LOOPBACK), "%s: stays on loopback", what);
}

int main(void)
{
    char *none[] = { "psQuake", "-window", "+map", "lq_e1m1" };
    char *any[] = { "psQuake", "-psquake-bind", "0.0.0.0" };
    char *one[] = { "psQuake", "-psquake-bind", "192.168.1.20" };
    char *lo[] = { "psQuake", "-psquake-bind", "127.0.0.1" };
    char *ip[] = { "psQuake", "-ip", "0.0.0.0" };
    char *missing[] = { "psQuake", "-psquake-bind" };
    char *empty[] = { "psQuake", "-psquake-bind", "" };
    char *name[] = { "psQuake", "-psquake-bind", "localhost" };
    char *shortform[] = { "psQuake", "-psquake-bind", "1" };
    char *hex[] = { "psQuake", "-psquake-bind", "0x7f.1" };
    char *v6[] = { "psQuake", "-psquake-bind", "::1" };
    char *junk[] = { "psQuake", "-psquake-bind", "10.0.0.1x" };
    char *first[] = { "psQuake", "-psquake-bind", "10.0.0.1",
                      "-psquake-bind", "0.0.0.0" };
    char *prefix[] = { "psQuake", "-psquake-bindx", "0.0.0.0" };

    expect_ok(ARGC(none), none, "127.0.0.1", "default is loopback");
    expect_ok(0, NULL, "127.0.0.1", "no argv is loopback");
    expect_ok(ARGC(any), any, "0.0.0.0", "explicit widen to every interface");
    expect_ok(ARGC(one), one, "192.168.1.20", "one interface");
    expect_ok(ARGC(lo), lo, "127.0.0.1", "explicit loopback");
    expect_ok(ARGC(ip), ip, "127.0.0.1", "-ip does not change the bind");
    expect_ok(ARGC(first), first, "10.0.0.1", "first flag wins");
    expect_ok(ARGC(prefix), prefix, "127.0.0.1", "only the exact flag counts");

    expect_err(ARGC(missing), missing, PSQ_BIND_ERR_MISSING_VALUE, "missing");
    expect_err(ARGC(empty), empty, PSQ_BIND_ERR_MISSING_VALUE, "empty");
    expect_err(ARGC(name), name, PSQ_BIND_ERR_INVALID, "host name");
    expect_err(ARGC(shortform), shortform, PSQ_BIND_ERR_INVALID,
               "inet_addr short form");
    expect_err(ARGC(hex), hex, PSQ_BIND_ERR_INVALID, "inet_addr hex form");
    expect_err(ARGC(v6), v6, PSQ_BIND_ERR_INVALID, "IPv6");
    expect_err(ARGC(junk), junk, PSQ_BIND_ERR_INVALID, "trailing junk");

    CHECK(psq_net_bind_addr(ARGC(any), any, NULL) == PSQ_BIND_OK,
          "NULL out is allowed");

    printf("psq_net_test: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
