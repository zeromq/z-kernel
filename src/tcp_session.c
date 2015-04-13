//  TCP session class

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.*/

#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "io_object.h"
#include "session.h"
#include "tcp_session.h"
#include "msg.h"
#include "zkernel.h"
#include "protocol_engine.h"

struct tcp_session {
    session_t base;
    int fd;
    pdu_t *queue_head;
    pdu_t *queue_tail;
    protocol_engine_t *protocol_engine;
    protocol_engine_info_t peinfo;
    iobuf_t *sendbuf;
    iobuf_t *recvbuf;
    actor_t *owner;
};

static int
    s_input (tcp_session_t *self);

static int
    s_output (tcp_session_t *self);

static struct io_object_ops io_ops;

static struct session_ops session_ops;

tcp_session_t *
tcp_session_new (int fd, protocol_engine_t *protocol_engine, actor_t *owner)
{
    tcp_session_t *self = (tcp_session_t *) malloc (sizeof *self);
    if (self) {
        const size_t buffer_size = 4096;
        *self = (tcp_session_t) {
            .base = (session_t) {
                .base = (io_object_t) { .ops = io_ops },
                .ops = session_ops,
            },
            .fd = fd,
            .protocol_engine = protocol_engine,
            .sendbuf = iobuf_new (buffer_size),
            .recvbuf = iobuf_new (buffer_size),
            .owner = owner
        };
        if (protocol_engine_init (protocol_engine, &self->peinfo) == -1)
            goto error;
        if (self->sendbuf == NULL || self->recvbuf == NULL)
            goto error;
    }
    return self;

error:
    close (self->fd);
    if (self->protocol_engine)
        protocol_engine_destroy (&self->protocol_engine);
    if (self->sendbuf)
        iobuf_destroy (&self->sendbuf);
    if (self->recvbuf)
        iobuf_destroy (&self->recvbuf);
    free (self);

    return NULL;
}

void
tcp_session_destroy (tcp_session_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        tcp_session_t *self = *self_p;
        msg_t *msg = (msg_t *) self->queue_head;
        while (msg) {
            msg_t *next = msg->next;
            msg_destroy (&msg);
            msg = next;
        }
        close (self->fd);
        protocol_engine_destroy (&self->protocol_engine);
        iobuf_destroy (&self->sendbuf);
        iobuf_destroy (&self->recvbuf);
        free (self);
        *self_p = NULL;
    }
}

static void
s_send_session_closed (tcp_session_t *self)
{
    session_closed_ev_t *ev = session_closed_ev_new ();
    assert (ev);
    ev->ptr = self;
    actor_send (self->owner, (msg_t *) ev);
}

static int
s_io_init (io_object_t *self_, int *fd, uint32_t *timer_interval)
{
    tcp_session_t *self = (tcp_session_t *) self_;
    assert (self);

    //  Set non-blocking mode
    const int flags = fcntl (self->fd, F_GETFL, 0);
    assert (flags != -1);
    int rc = fcntl (self->fd, F_SETFL, flags | O_NONBLOCK);
    assert (rc == 0);

    *fd = self->fd;
    return ZKERNEL_POLLIN | ZKERNEL_POLLOUT;
}

static int
s_io_event (io_object_t *self_, uint32_t io_flags, int *fd, uint32_t *timer_interval)
{
    tcp_session_t *self = (tcp_session_t *) self_;
    protocol_engine_info_t *peinfo = &self->peinfo;
    assert (self);

    if ((io_flags & ZKERNEL_IO_ERROR) != 0)
        goto error;

    while (1) {
        if ((io_flags & ZKERNEL_INPUT_READY) != 0) {
            if (s_input (self) == -1)
                goto error;
            if ((peinfo->flags & ZKERNEL_WRITE_OK) != 0)
                io_flags &= ~ZKERNEL_INPUT_READY;
        }

        while ((peinfo->flags & ZKERNEL_DECODER_READY) != 0) {
            pdu_t *pdu = protocol_engine_decode (self->protocol_engine, peinfo);
            if (pdu == NULL)
                goto error;
            pdu->io_object = self_;
            actor_send (self->owner, (msg_t *) pdu);
        }

        while (self->queue_head && (peinfo->flags & ZKERNEL_ENCODER_READY) != 0) {
            pdu_t *pdu = self->queue_head;
            self->queue_head = (pdu_t *) pdu->base.next;
            if (self->queue_head == NULL)
                self->queue_tail = NULL;
            if (protocol_engine_encode (self->protocol_engine, pdu, peinfo) == -1)
                goto error;
        }

        if ((io_flags & ZKERNEL_OUTPUT_READY) != 0) {
            if (s_output (self) == -1)
                goto error;
            if ((peinfo->flags & ZKERNEL_READ_OK) != 0)
                io_flags &= ~ZKERNEL_OUTPUT_READY;
        }

        if ((peinfo->flags & ZKERNEL_ENGINE_DONE) != 0) {
            const int rc = protocol_engine_next (&self->protocol_engine, peinfo);
            if (rc == -1)
                goto error;
        }

        uint32_t mask = peinfo->flags;
        if ((io_flags & ZKERNEL_INPUT_READY) == 0)
            mask &= ~ZKERNEL_WRITE_OK;
        if ((io_flags & ZKERNEL_OUTPUT_READY) == 0)
            mask &= ~ZKERNEL_READ_OK;

        if ((peinfo->flags & mask) == 0)
            break;
    }

    int io_mask = 0;
    if ((peinfo->flags & ZKERNEL_WRITE_OK) != 0)
        io_mask |= ZKERNEL_POLLIN;
    if ((peinfo->flags & ZKERNEL_READ_OK) != 0 || iobuf_available (self->sendbuf) > 0)
        io_mask |= ZKERNEL_POLLOUT;

    return io_mask;

error:
    s_send_session_closed (self);
    *fd = -1;
    return -1;
}

static int
s_input (tcp_session_t *self)
{
    protocol_engine_t *protocol_engine = self->protocol_engine;
    protocol_engine_info_t *peinfo = &self->peinfo;
    iobuf_t *recvbuf = self->recvbuf;

    while ((peinfo->flags & ZKERNEL_WRITE_OK) != 0) {
        if (iobuf_available (recvbuf) > 0) {
            if (protocol_engine_write (protocol_engine, recvbuf, peinfo) != 0)
                return -1;
        }
        else {
            if (peinfo->write_buffer_size > 256) {
                assert (peinfo->write_buffer);
                const ssize_t rc = recv (
                    self->fd, peinfo->write_buffer, peinfo->write_buffer_size, 0);
                if (rc == 0)
                    return -1;
                if (rc == -1) {
                    if (errno == EINTR || errno == EAGAIN)
                        return 0;
                    else
                        return -1;
                }
                if (protocol_engine_write_advance (
                        protocol_engine, (size_t) rc, peinfo) != 0)
                    return -1;
            }
            else {
                iobuf_reset (recvbuf);
                const ssize_t rc = iobuf_recv (recvbuf, self->fd);
                if (rc == 0)
                    return -1;
                if (rc == -1) {
                    if (errno == EINTR || errno == EAGAIN)
                        return 0;
                    else
                        return -1;
                }
            }
        }
    }

    return 0;
}

static int
s_output (tcp_session_t *self)
{
    protocol_engine_t *protocol_engine = self->protocol_engine;
    protocol_engine_info_t *peinfo = &self->peinfo;
    iobuf_t *sendbuf = self->sendbuf;

    while (iobuf_available (sendbuf)) {
        const ssize_t rc = iobuf_send (sendbuf, self->fd);
        if (rc == -1) {
            if (errno == EAGAIN || errno == EINTR)
                return 0;
            else
                return -1;
        }
    }

    while ((peinfo->flags & ZKERNEL_READ_OK) != 0) {
        if (peinfo->read_buffer_size > 256) {
            assert (peinfo->read_buffer);
            const ssize_t rc = send (self->fd, peinfo->read_buffer, peinfo->read_buffer_size, 0);
            if (rc == -1) {
                if (errno == EAGAIN || errno == EINTR)
                    return 0;
                else
                    return -1;
            }
            if (protocol_engine_read_advance (
                    protocol_engine, (size_t) rc, peinfo) != 0)
                return -1;
        }
        else {
            iobuf_reset (sendbuf);
            const int rc = protocol_engine_read (protocol_engine, sendbuf, peinfo);
            if (rc == -1)
                return -1;
            while (iobuf_available (sendbuf)) {
                const ssize_t rc = iobuf_send (sendbuf, self->fd);
                if (rc == -1) {
                    if (errno == EAGAIN || errno == EINTR)
                        return 0;
                    else
                        return -1;
                }
            }
        }
    }

    return 0;
}

static int
s_io_message (io_object_t *self_, msg_t *msg)
{
    tcp_session_t *self = (tcp_session_t *) self_;
    assert (self);

    if (msg->msg_type == ZKERNEL_MSG_TYPE_PDU) {
        pdu_t *pdu = (pdu_t *) msg;
        if (self->queue_head == NULL)
            self->queue_head = self->queue_tail = pdu;
        else {
            self->queue_tail->base.next = (msg_t *) pdu;
            self->queue_tail = pdu;
        }
        pdu->base.next = NULL;
    }
    else
        msg_destroy (&msg);

    return ZKERNEL_POLLIN | ZKERNEL_POLLOUT;
}

static int
s_session_set_socket_id (session_t *base, const char *socket_id)
{
    tcp_session_t *self = (tcp_session_t *) base;
    return protocol_engine_set_socket_id (self->protocol_engine, socket_id);
}

static void
s_session_destroy (session_t **self_p)
{
    tcp_session_destroy ((tcp_session_t **) self_p);
}

static struct io_object_ops io_ops = {
    .init  = s_io_init,
    .event = s_io_event,
    .message = s_io_message,
};

static struct session_ops session_ops = {
    .set_socket_id = s_session_set_socket_id,
    .destroy = s_session_destroy,
};
