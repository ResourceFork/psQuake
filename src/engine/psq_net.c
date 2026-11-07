/*
 * psQuake -- UDP bind address. See psq_net.h.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#include "psq_net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>

psq_bind_status psq_net_bind_addr(int argc, char **argv, uint32_t *addr_be)
{
    uint32_t loopback = htonl(INADDR_LOOPBACK);
    struct in_addr a;
    int i;

    if (addr_be != NULL)
        *addr_be = loopback;

    for (i = 1; argv != NULL && i < argc; i++) {
        if (argv[i] == NULL || strcmp(argv[i], PSQ_BIND_FLAG) != 0)
            continue;
        if (i + 1 >= argc || argv[i + 1] == NULL || argv[i + 1][0] == '\0')
            return PSQ_BIND_ERR_MISSING_VALUE;
        /* inet_pton takes only the strict dotted quad, unlike inet_addr,
         * which also takes "1", "0x7f.1" and the like. */
        if (inet_pton(AF_INET, argv[i + 1], &a) != 1)
            return PSQ_BIND_ERR_INVALID;
        if (addr_be != NULL)
            *addr_be = a.s_addr;
        return PSQ_BIND_OK;
    }
    return PSQ_BIND_OK;
}
