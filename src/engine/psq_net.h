/*
 * psQuake -- UDP bind address (engine-free, host-tested).
 *
 * Upstream QuakeSpasm binds every UDP socket to INADDR_ANY, so the client's
 * control socket, and any server's port 26000, listen on every interface.
 * psQuake binds to 127.0.0.1 unless the player widens it explicitly:
 *
 *   -psquake-bind 0.0.0.0        every interface (upstream behaviour)
 *   -psquake-bind 192.168.1.20   one interface
 *
 * -noudp still opens no socket at all. Upstream's -ip keeps its meaning (the
 * address the game advertises) and does not change the bind.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#ifndef PSQ_NET_H
#define PSQ_NET_H

#include <stdint.h>

#define PSQ_BIND_FLAG "-psquake-bind"

typedef enum
{
    PSQ_BIND_OK,
    PSQ_BIND_ERR_MISSING_VALUE, /* -psquake-bind with nothing after it */
    PSQ_BIND_ERR_INVALID        /* not a dotted-quad IPv4 address */
} psq_bind_status;

/*
 * The IPv4 address to bind UDP sockets to, in network byte order, from
 * argv[1..argc). 127.0.0.1 when the flag is absent. On error *addr_be is
 * left at 127.0.0.1.
 */
psq_bind_status psq_net_bind_addr(int argc, char **argv, uint32_t *addr_be);

#endif /* PSQ_NET_H */
