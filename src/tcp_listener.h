//  TCP listener class

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.*/

#ifndef __TCP_LISTENER_H_INCLUDED__
#define __TCP_LISTENER_H_INCLUDED__

#include "proxy.h"
#include "socket.h"
#include "protocol_engine.h"

typedef struct tcp_listener tcp_listener_t;

tcp_listener_t *
    tcp_listener_new (
            protocol_engine_constructor_t *protocol_engine_constructor,
            socket_t *owner, proxy_t *proxy);

void
    tcp_listener_destroy (tcp_listener_t **self_p);

int
    tcp_listener_bind (tcp_listener_t *self, unsigned short port);

#endif
