/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/**
 * This file contains routines for reading and writing data from and to a
 * socket
 * @author Mark Nunberg
 */

#include "internal.h"
#include "iotable.h"

lcbio_status_t lcb_sockrw_v0_read(lcbconn_t conn, ringbuffer_t *buf)
{
    struct lcb_iovec_st iov[2];
    lcb_ssize_t nr;
    lcb_iotable *iot = conn->iotable;
    lcbio_Ectx *e = &conn->u_model.e;

    if (!ringbuffer_ensure_capacity(buf,
                                    conn->settings ? conn->settings->rbufsize :
                                    LCB_DEFAULT_RBUFSIZE)) {
        return LCBIO_STATUS_INTERR;
    }

    ringbuffer_get_iov(buf, RINGBUFFER_WRITE, iov);
    nr = IOT_V0IO(iot).recvv(IOT_ARG(iot), e->sockfd, iov, 2);
    if (nr == -1) {
        switch (IOT_ERRNO(iot)) {
        case EINTR:
            break;
        case EWOULDBLOCK:
#ifdef USE_EAGAIN
        case EAGAIN:
#endif
            return LCBIO_STATUS_PENDING;
        default:
            return LCBIO_STATUS_IOERR;
            return -1;
        }

    } else if (nr == 0) {
        lcb_assert((iov[0].iov_len + iov[1].iov_len) != 0);
        /* TODO stash error message somewhere
         * "Connection closed... we should resend to other nodes or reconnect!!" */
        return LCBIO_STATUS_SHUTDOWN;

    } else {
        ringbuffer_produced(buf, (lcb_size_t)nr);
    }

    return LCBIO_STATUS_CANREAD;
}

lcbio_status_t lcb_sockrw_v0_slurp(lcbconn_t conn, ringbuffer_t *buf)
{
    lcbio_status_t status;
    while ((status = lcb_sockrw_v0_read(conn, buf)) == LCBIO_STATUS_CANREAD) {
        ;
    }
    return status;

}


lcbio_status_t lcb_sockrw_v0_write(lcbconn_t conn,
                                        ringbuffer_t *buf)
{
    lcb_iotable *iot = conn->iotable;
    lcbio_Ectx *e = &conn->u_model.e;

    while (buf->nbytes > 0) {
        struct lcb_iovec_st iov[2];
        lcb_ssize_t nw;
        ringbuffer_get_iov(buf, RINGBUFFER_READ, iov);
        nw = IOT_V0IO(iot).sendv(IOT_ARG(iot), e->sockfd, iov, 2);
        if (nw == -1) {
            switch (IOT_ERRNO(iot)) {
            case EINTR:
                /* retry */
                break;
            case EWOULDBLOCK:
#ifdef USE_EAGAIN
            case EAGAIN:
#endif
                return LCBIO_STATUS_PENDING;

            default:
                return LCBIO_STATUS_IOERR;
            }
        } else if (nw > 0) {
            ringbuffer_consumed(buf, (lcb_size_t)nw);
        }
    }

    return LCBIO_STATUS_WFLUSHED;
}

void lcb_sockrw_set_want(lcbconn_t conn, short events, int clear_existing)
{

    if (clear_existing) {
        conn->want = events;
    } else {
        conn->want |= events;
    }
}

static void apply_want_v0(lcbconn_t conn)
{
    lcb_iotable *iot = conn->iotable;
    lcbio_Ectx *e = &conn->u_model.e;
    if (!conn->want) {
        if (e->active) {
            e->active = 0;
            IOT_V0EV(iot).cancel(IOT_ARG(iot), e->sockfd, e->ptr);
        }
        return;
    }

    e->active = 1;
    IOT_V0EV(iot).watch(IOT_ARG(iot), e->sockfd, e->ptr, conn->want, conn,
                        e->handler);
}

static void apply_want_v1(lcbconn_t conn)
{
    lcbio_Cctx *c = &conn->u_model.c;
    if (!conn->want) {
        return;
    }
    if (!c->sockptr) {
        return;
    }
    if (c->sockptr->closed) {
        return;
    }

    if (conn->want & LCB_READ_EVENT) {
        lcb_sockrw_v1_start_read(conn, &conn->input, c->read);
    }

    if (conn->want & LCB_WRITE_EVENT) {

        if (conn->output == NULL || conn->output->nbytes == 0) {
            return;
        }

        lcb_sockrw_v1_start_write(conn, &conn->output, c->write);
    }

}

void lcb_sockrw_apply_want(lcbconn_t conn)
{
    if (conn->iotable == NULL) {
        return;
    }
    if (IOT_IS_EVENT(conn->iotable)) {
        apply_want_v0(conn);
    } else {
        apply_want_v1(conn);
    }
}

int lcb_sockrw_flushed(lcbconn_t conn)
{
    if (!IOT_IS_EVENT(conn->iotable)) {
        if (conn->output && conn->output->nbytes == 0) {
            return 1;
        } else {
            return 0;
        }
    } else {
        if (conn->output && conn->output->nbytes == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Request a read of data into the buffer
 * @param conn the connection object
 * @param buf a ringbuffer structure. If the read request is successful,
 * the ringbuffer is destroyed. Its allocated data is owned by the IO plugin
 * for the duration of the operation. It may be restored via
 * ringbuffer_take_buffer once the operation has finished.
 */
lcbio_status_t lcb_sockrw_v1_start_read(lcbconn_t conn,
                                             ringbuffer_t **buf,
                                             lcb_io_read_cb callback)
{
    int ret;
    lcb_iotable *io;
    lcbio_Cctx *c = &conn->u_model.c;
    lcb_sockdata_t *sd = c->sockptr;

    struct lcb_buf_info *bi = &c->sockptr->read_buffer;

    if (sd->is_reading) {
        return LCBIO_STATUS_PENDING;
    }

    ringbuffer_ensure_capacity(*buf,
                               conn->settings ? conn->settings->rbufsize :
                               LCB_DEFAULT_RBUFSIZE);
    ringbuffer_get_iov(*buf, RINGBUFFER_WRITE, bi->iov);

    lcb_assert(bi->ringbuffer == NULL);
    lcb_assert(bi->root == NULL);

    bi->ringbuffer = *buf;
    bi->root = bi->ringbuffer->root;

    *buf = NULL;


    io = conn->iotable;
    ret = IOT_V1(io).read(IOT_ARG(io), sd, callback);

    if (ret == 0) {
        sd->is_reading = 1;
        return LCBIO_STATUS_PENDING;

    } else {
        *buf = bi->ringbuffer;
        memset(bi, 0, sizeof(*bi));
        lcb_async_signal(conn->as_err);
    }

    return LCBIO_STATUS_IOERR;
}

/**
 * Request that a write begin.
 * @param conn the connection object
 * @param buf a pointer to a ringbuffer_t*. If the write request is successful,
 * the IO system takes exclusive ownership of the buffer, and the contents
 * of *buf are zeroed.
 */
lcbio_status_t lcb_sockrw_v1_start_write(lcbconn_t conn,
                                              ringbuffer_t **buf,
                                              lcb_ioC_write2_callback callback)
{
    int ret;
    lcb_iotable *io;
    struct lcb_iovec_st iov[2];
    ringbuffer_t *rb = *buf;
    lcbio_Cctx *c = &conn->u_model.c;

    io = conn->iotable;
    *buf = NULL;

    ringbuffer_get_iov(rb, RINGBUFFER_READ, iov);
    ret = IOT_V1(io).write2(IOT_ARG(io), c->sockptr, iov, 2, rb, callback);
    if (ret == 0) {
        return LCBIO_STATUS_PENDING;

    } else {
        lcb_async_signal(conn->as_err);
        return LCBIO_STATUS_IOERR;
    }
}

void lcb_sockrw_v1_onread_common(lcb_sockdata_t *sock,
                                 ringbuffer_t **dst,
                                 lcb_ssize_t nr)
{
    struct lcb_buf_info *bi = &sock->read_buffer;

    lcb_assert(*dst == NULL);

    *dst = bi->ringbuffer;
    memset(bi, 0, sizeof(*bi));

    sock->is_reading = 0;

    if (nr > 0) {
        ringbuffer_produced(*dst, nr);
    }

}

void lcb_sockrw_v1_onwrite_common(lcb_sockdata_t *sock, void *arg,
                                  ringbuffer_t **dst)
{
    ringbuffer_t *orig = arg;
    if (*dst) {
        ringbuffer_destruct(orig);
        free(orig);
        return;
    }

    *dst = orig;
    ringbuffer_reset(*dst);
    (void)sock;
}


unsigned int lcb_sockrw_v1_cb_common(lcb_sockdata_t *sock,
                                     void *wdata,
                                     void **datap)
{
    int is_closed;

    lcbconn_t conn = sock->lcbconn;
    is_closed = sock->closed;

    if (is_closed) {
        if (wdata) {
            ringbuffer_t *orig = wdata;
            ringbuffer_destruct(orig);
            free(orig);
        }
        return 0;
    }

    if (datap && conn) {
        *datap = conn->data;
    }

    return 1;
}

static void v0_generic_handler(lcb_socket_t sock, short which, void *arg)
{
    lcbconn_t conn = arg;
    lcbio_status_t status;
    lcb_size_t oldnr, newnr;

    lcb_assert(sock != INVALID_SOCKET);

    if (which & LCB_WRITE_EVENT) {
        status = lcb_sockrw_v0_write(conn, conn->output);
        if (status == LCBIO_STATUS_WFLUSHED) {
            if ((which & LCB_READ_EVENT) == 0) {
                lcb_sockrw_set_want(conn, LCB_READ_EVENT, 1);
                lcb_sockrw_apply_want(conn);
            }

        } else if (status == LCBIO_STATUS_PENDING) {
            lcb_sockrw_set_want(conn, LCB_WRITE_EVENT, 0);
            lcb_sockrw_apply_want(conn);

        } else {
            conn->errcb(conn);
            return;
        }
    }

    if ( (which & LCB_READ_EVENT) == 0) {
        return;
    }

    oldnr = conn->input->nbytes;
    status = lcb_sockrw_v0_slurp(conn, conn->input);
    newnr = conn->input->nbytes;
    if (LCBIO_IS_OK(status) == 0 && oldnr == newnr) {
        conn->errcb(conn);
    } else {
        conn->easy.read(conn);
    }
}

static void v1_generic_write_handler(lcb_sockdata_t *sd,
                                     int status,
                                     void *wdata)
{
    lcb_t instance;
    lcbconn_t conn = sd->lcbconn;

    if (!lcb_sockrw_v1_cb_common(sd, wdata, (void **)&instance)) {
        return;
    }

    lcb_sockrw_v1_onwrite_common(sd, wdata, &sd->lcbconn->output);

    if (status) {
        conn->errcb(conn);
    } else {
        lcb_sockrw_set_want(conn, LCB_READ_EVENT, 1);
        lcb_sockrw_apply_want(conn);
    }
}

static void v1_generic_read_handler(lcb_sockdata_t *sd, lcb_ssize_t nr)
{
    lcbconn_t conn = sd->lcbconn;
    if (!lcb_sockrw_v1_cb_common(sd, NULL, NULL)) {
        return;
    }

    lcb_sockrw_v1_onread_common(sd, &sd->lcbconn->input, nr);
    if (nr < 1) {
        conn->errcb(conn);
        return;

    } else {
        conn->easy.read(conn);
    }
}

void lcb__io_wire_easy(struct lcb_io_use_st *use)
{
    use->easy = 0;
    use->u.ex.v0_handler = v0_generic_handler;
    use->u.ex.v1_read = v1_generic_read_handler;
    use->u.ex.v1_write = v1_generic_write_handler;
}
