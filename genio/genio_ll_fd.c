/*
 *  genio - A library for abstracting stream I/O
 *  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <errno.h>
#include "genio_internal.h"
#include "genio_base.h"

#include <assert.h>
#include <unistd.h>

enum fd_state {
    FD_CLOSED,
    FD_IN_OPEN,
    FD_OPEN,
    FD_IN_CLOSE
};

struct fd_ll {
    struct genio_ll ll;
    struct genio_os_funcs *o;

    struct genio_lock *lock;

    unsigned int refcount;

    const struct genio_ll_callbacks *cbs;
    void *cb_data;

    int fd;

    enum fd_state state;

    bool read_enabled;
    bool write_enabled;

    const struct genio_fd_ll_ops *ops;
    void *handler_data;
    int (*check_open)(void *handler_data, int fd);
    int (*retry_open)(void *handler_data, int *fd);

    genio_ll_open_done open_done;
    void *open_data;
    int open_err;

    struct genio_timer *close_timer;
    genio_ll_close_done close_done;
    void *close_data;

    unsigned char *read_data;
    unsigned int read_data_size;
    unsigned int read_data_len;
    unsigned int read_data_pos;

    bool in_read;

    /*
     * Used to run read callbacks from the selector to avoid running
     * it directly from user calls.
     */
    bool deferred_op_pending;
    struct genio_runner *deferred_op_runner;

    bool deferred_read;
    bool deferred_close;
};

#define ll_to_fd(v) container_of(v, struct fd_ll, ll)

static void
fd_lock(struct fd_ll *fdll)
{
    fdll->o->lock(fdll->lock);
}

static void
fd_unlock(struct fd_ll *fdll)
{
    fdll->o->unlock(fdll->lock);
}

static void
fd_ref(struct fd_ll *fdll)
{
    fdll->refcount++;
}

static void
fd_lock_and_ref(struct fd_ll *fdll)
{
    fd_lock(fdll);
    fdll->refcount++;
}

static void fd_finish_free(struct fd_ll *fdll)
{
    if (fdll->lock)
	fdll->o->free_lock(fdll->lock);
    if (fdll->close_timer)
	fdll->o->free_timer(fdll->close_timer);
    if (fdll->deferred_op_runner)
	fdll->o->free_runner(fdll->deferred_op_runner);
    if (fdll->read_data)
	fdll->o->free(fdll->o, fdll->read_data);
    if (fdll->ops)
	fdll->ops->free(fdll->handler_data);
    fdll->o->free(fdll->o, fdll);
}

static void
fd_deref_and_unlock(struct fd_ll *fdll)
{
    unsigned int count;

    assert(fdll->refcount > 0);
    count = --fdll->refcount;
    fd_unlock(fdll);
    if (count == 0)
	fd_finish_free(fdll);
}

static void
fd_set_callbacks(struct genio_ll *ll, const struct genio_ll_callbacks *cbs,
		 void *cb_data)
{
    struct fd_ll *fdll = ll_to_fd(ll);

    fdll->cbs = cbs;
    fdll->cb_data = cb_data;
}

static int
fd_write(struct genio_ll *ll, unsigned int *rcount,
	 const unsigned char *buf, unsigned int buflen)
{
    struct fd_ll *fdll = ll_to_fd(ll);

    int rv, err = 0;

 retry:
    rv = write(fdll->fd, buf, buflen);
    if (rv < 0) {
	if (errno == EINTR)
	    goto retry;
	if (errno == EWOULDBLOCK || errno == EAGAIN)
	    rv = 0; /* Handle like a zero-byte write. */
	else
	    err = errno;
    } else if (rv == 0) {
	err = EPIPE;
    }

    if (!err && rcount)
	*rcount = rv;

    return err;
}

static int
fd_raddr_to_str(struct genio_ll *ll, int *pos,
		char *buf, unsigned int buflen)
{
    struct fd_ll *fdll = ll_to_fd(ll);

    return fdll->ops->raddr_to_str(fdll->handler_data, pos, buf, buflen);
}

static int
fd_get_raddr(struct genio_ll *ll, struct sockaddr *addr, socklen_t *addrlen)
{
    struct fd_ll *fdll = ll_to_fd(ll);

    if (fdll->ops->get_raddr)
	return fdll->ops->get_raddr(fdll->handler_data, addr, addrlen);
    return ENOTSUP;
}

static int
fd_remote_id(struct genio_ll *ll, int *id)
{
    struct fd_ll *fdll = ll_to_fd(ll);

    if (fdll->ops->remote_id)
	return fdll->ops->remote_id(fdll->handler_data, id);
    return ENOTSUP;
}

static void
fd_deliver_read_data(struct fd_ll *fdll, int err)
{
    if (err || fdll->read_data_len) {
	unsigned int count;

	fd_unlock(fdll);
	count = fdll->cbs->read_callback(fdll->cb_data, err,
					 fdll->read_data + fdll->read_data_pos,
					 fdll->read_data_len);
	fd_lock(fdll);
	if (err || count >= fdll->read_data_len) {
	    fdll->read_data_pos = 0;
	    fdll->read_data_len = 0;
	} else {
	    fdll->read_data_pos += count;
	    fdll->read_data_len -= count;
	}
    }
}

static void
fd_start_close(struct fd_ll *fdll)
{
    if (fdll->ops->check_close)
	fdll->ops->check_close(fdll->handler_data,
			       GENIO_LL_CLOSE_STATE_START, NULL);
    fdll->state = FD_IN_CLOSE;
    fdll->o->clear_fd_handlers(fdll->o, fdll->fd);
}

static void
fd_finish_open(struct fd_ll *fdll, int err)
{
    if (err) {
	fdll->open_err = err;
	fd_start_close(fdll);
	return;
    }

    fdll->state = FD_OPEN;
    if (fdll->open_done) {
	genio_ll_open_done open_done = fdll->open_done;

	fdll->open_done = NULL;
	fd_unlock(fdll);
	open_done(fdll->cb_data, 0, fdll->open_data);
	fd_lock(fdll);
    }

    if (fdll->state == FD_OPEN) {
	if (fdll->read_enabled) {
	    fdll->o->set_read_handler(fdll->o, fdll->fd, true);
	    fdll->o->set_except_handler(fdll->o, fdll->fd, true);
	}
	if (fdll->write_enabled)
	    fdll->o->set_write_handler(fdll->o, fdll->fd, true);
    }
}

static void fd_finish_close(struct fd_ll *fdll)
{
    fdll->state = FD_CLOSED;
    fdll->close_done = NULL;
    if (fdll->close_done) {
	fd_unlock(fdll);
	fdll->close_done(fdll->cb_data, fdll->close_data);
	fd_lock(fdll);
    }
}

static void
fd_deferred_op(struct genio_runner *runner, void *cbdata)
{
    struct fd_ll *fdll = cbdata;

    fd_lock(fdll);
    if (fdll->deferred_close) {
	fdll->deferred_close = false;
	fd_finish_close(fdll);
    }

 retry:
    if (fdll->deferred_read) {
	fdll->deferred_read = false;

	fd_deliver_read_data(fdll, 0);

	fdll->in_read = false;

	/* FIXME - error handling? */
    }

    if (fdll->deferred_read)
	goto retry;

    fdll->deferred_op_pending = false;
    if (fdll->state == FD_OPEN) {
	fdll->o->set_read_handler(fdll->o, fdll->fd, fdll->read_enabled);
	fdll->o->set_except_handler(fdll->o, fdll->fd, fdll->read_enabled);
	fdll->o->set_write_handler(fdll->o, fdll->fd, fdll->write_enabled);
    }
    fd_deref_and_unlock(fdll);
}

static void
fd_sched_deferred_op(struct fd_ll *fdll)
{
    if (!fdll->deferred_op_pending) {
	/* Call the read from the selector to avoid lock nesting issues. */
	fd_ref(fdll);
	fdll->deferred_op_pending = true;
	fdll->o->run(fdll->deferred_op_runner);
    }
}

static void
fd_handle_incoming(int fd, void *cbdata, bool urgent)
{
    struct fd_ll *fdll = cbdata;
    int c;
    int rv, err = 0;

    fd_lock(fdll);
    fdll->o->set_read_handler(fdll->o, fdll->fd, false);
    fdll->o->set_except_handler(fdll->o, fdll->fd, false);
    if (fdll->in_read)
	goto out_unlock;

    fdll->in_read = true;
    if (urgent) {
	/* We should have urgent data, a DATA MARK in the stream.  Read
	   the urgent data (whose contents are irrelevant) then inform
	   the user. */
	for (;;) {
	    rv = recv(fd, &c, 1, MSG_OOB);
	    if (rv == 0 || (rv < 0 && errno != EINTR))
		break;
	}
	if (fdll->cbs->urgent_callback) {
	    fd_unlock(fdll);
	    fdll->cbs->urgent_callback(fdll->cb_data);
	    fd_lock(fdll);
	}
    }

    if (!fdll->read_data_len) {
    retry:
	rv = read(fd, fdll->read_data, fdll->read_data_size);
	if (rv < 0) {
	    if (errno == EINTR)
		goto retry;
	    if (errno == EAGAIN || errno == EWOULDBLOCK)
		rv = 0; /* Pretend like nothing happened. */
	    else
		err = errno;
	} else if (rv == 0) {
	    err = EPIPE;
	} else {
	    fdll->read_data_len = rv;
	}
    }

    fd_deliver_read_data(fdll, err);

    fdll->in_read = false;
 out_unlock:
    if (fdll->state == FD_OPEN && fdll->read_enabled) {
	fdll->o->set_read_handler(fdll->o, fdll->fd, true);
	fdll->o->set_except_handler(fdll->o, fdll->fd, true);
    }
    fd_unlock(fdll);
}

static void
fd_read_ready(int fd, void *cbdata)
{
    fd_handle_incoming(fd, cbdata, false);
}

static void
fd_except_ready(int fd, void *cbdata)
{
    fd_handle_incoming(fd, cbdata, true);
}

static int fd_setup_handlers(struct fd_ll *fdll);

static void
fd_write_ready(int fd, void *cbdata)
{
    struct fd_ll *fdll = cbdata;

    fd_lock(fdll);
    fdll->o->set_write_handler(fdll->o, fdll->fd, false);
    if (fdll->state == FD_IN_OPEN) {
	int err;

	err = fdll->check_open(fdll->handler_data, fdll->fd);
	if (err) {
	    fdll->o->clear_fd_handlers_norpt(fdll->o, fdll->fd);
	    close(fdll->fd);
	    fdll->fd = -1;
	    err = fdll->retry_open(fdll->handler_data, &fdll->fd);
	    if (err != EINPROGRESS)
		goto opened;
	    else {
		err = fd_setup_handlers(fdll);
		if (err) {
		    close(fdll->fd);
		    fdll->fd = -1;
		    fd_finish_open(fdll, err);
		} else {
		    fdll->o->set_write_handler(fdll->o, fdll->fd, true);
		}
	    }
	} else {
	opened:
	    fd_finish_open(fdll, err);
	}
	fd_unlock(fdll);
    } else {
	fd_unlock(fdll);
	fdll->cbs->write_callback(fdll->cb_data);
	if (fdll->state == FD_OPEN && fdll->write_enabled)
	    fdll->o->set_write_handler(fdll->o, fdll->fd, true);
	fd_lock(fdll);
    }
    fd_unlock(fdll);
}

static void
fd_finish_cleared(struct fd_ll *fdll)
{
    fd_lock_and_ref(fdll);
    close(fdll->fd);
    fdll->fd = -1;
    if (fdll->open_done) {
	/* If an open fails, it comes to here. */
	genio_ll_open_done open_done = fdll->open_done;

	fdll->open_done = NULL;
	fd_unlock(fdll);
	open_done(fdll->open_data, fdll->open_err, fdll->open_data);
	fd_lock(fdll);
    }

    if (fdll->deferred_op_pending)
	/* Call it from the deferred_op handler. */
	fdll->deferred_close = true;
    else
	fd_finish_close(fdll);

    fd_deref_and_unlock(fdll);
}

static void
fd_close_timeout(struct genio_timer *t, void *cb_data)
{
    struct fd_ll *fdll = cb_data;
    struct timeval timeout;
    int err = 0;

    if (fdll->ops->check_close)
	err = fdll->ops->check_close(fdll->handler_data,
				     GENIO_LL_CLOSE_STATE_DONE, &timeout);

    if (err == EAGAIN) {
	fdll->o->start_timer(fdll->close_timer, &timeout);
	return;
    }

    fd_finish_cleared(fdll);
}

static void
fd_cleared(int fd, void *cb_data)
{
    struct fd_ll *fdll = cb_data;

    if (fdll->ops->check_close)
	fd_close_timeout(NULL, fdll);
    else
	fd_finish_cleared(fdll);
}

static int
fd_open(struct genio_ll *ll, genio_ll_open_done done, void *open_data)
{
    struct fd_ll *fdll = ll_to_fd(ll);
    int err;

    if (!fdll->ops->sub_open)
	return ENOTSUP;

    fd_lock(fdll);
    err = fdll->ops->sub_open(fdll->handler_data, &fdll->check_open,
			      &fdll->retry_open, &fdll->fd);
    if (err == EINPROGRESS || err == 0) {
	int err2 = fd_setup_handlers(fdll);
	if (err2) {
	    err = err2;
	    close(fdll->fd);
	    fdll->fd = -1;
	    goto out;
	}

	if (err == EINPROGRESS) {
	    fdll->state = FD_IN_OPEN;
	    fdll->open_done = done;
	    fdll->open_data = open_data;
	    fdll->o->set_write_handler(fdll->o, fdll->fd, true);
	} else {
	    fdll->state = FD_OPEN;
	}
    }

 out:
    fd_unlock(fdll);
    return err;
}

static int
fd_setup_handlers(struct fd_ll *fdll)
{
    if (fdll->o->set_fd_handlers(fdll->o, fdll->fd, fdll, fd_read_ready,
				 fd_write_ready, fd_except_ready,
				 fd_cleared))
	return ENOMEM;
    return 0;
}

static int fd_close(struct genio_ll *ll, genio_ll_close_done done,
		    void *close_data)
{
    struct fd_ll *fdll = ll_to_fd(ll);
    int err = EBUSY;

    fd_lock(fdll);
    if (fdll->state == FD_OPEN || fdll->state == FD_IN_OPEN) {
	fdll->close_done = done;
	fdll->close_data = close_data;
	fd_start_close(fdll);
	err = 0;
    }
    fd_unlock(fdll);

    return err;
}

static void
fd_set_read_callback_enable(struct genio_ll *ll, bool enabled)
{
    struct fd_ll *fdll = ll_to_fd(ll);

    fd_lock(fdll);
    fdll->read_enabled = enabled;

    if (fdll->in_read || fdll->state != FD_OPEN ||
			(fdll->read_data_len && !enabled)) {
	/* It will be handled in finish_read or open finish. */
    } else if (fdll->read_data_len) {
	/* Call the read from the selector to avoid lock nesting issues. */
	fdll->in_read = true;
	fdll->deferred_read = true;
	fd_sched_deferred_op(fdll);
    } else {
	fdll->o->set_read_handler(fdll->o, fdll->fd, enabled);
    }
    fd_unlock(fdll);
}

static void
fd_set_write_callback_enable(struct genio_ll *ll, bool enabled)
{
    struct fd_ll *fdll = ll_to_fd(ll);

    fd_lock(fdll);
    fdll->write_enabled = enabled;
    if (fdll->state == FD_OPEN || fdll->state == FD_IN_OPEN)
	fdll->o->set_write_handler(fdll->o, fdll->fd, enabled);
    fd_unlock(fdll);
}

static void fd_free(struct genio_ll *ll)
{
    struct fd_ll *fdll = ll_to_fd(ll);

    fd_lock(fdll);
    fd_deref_and_unlock(fdll);
}

const static struct genio_ll_ops fd_ll_ops = {
    .set_callbacks = fd_set_callbacks,
    .write = fd_write,
    .raddr_to_str = fd_raddr_to_str,
    .get_raddr = fd_get_raddr,
    .remote_id = fd_remote_id,
    .open = fd_open,
    .close = fd_close,
    .set_read_callback_enable = fd_set_read_callback_enable,
    .set_write_callback_enable = fd_set_write_callback_enable,
    .free = fd_free
};

struct genio_ll *
fd_genio_ll_alloc(struct genio_os_funcs *o,
		  int fd,
		  const struct genio_fd_ll_ops *ops,
		  void *handler_data,
		  unsigned int max_read_size)
{
    struct fd_ll *fdll;

    fdll = o->zalloc(o, sizeof(*fdll));
    if (!fdll)
	return NULL;

    fdll->o = o;
    fdll->ops = ops;
    fdll->handler_data = handler_data;
    fdll->fd = fd;
    fdll->refcount = 1;
    if (fd == -1)
	fdll->state = FD_CLOSED;
    else
	fdll->state = FD_OPEN;

    fdll->close_timer = o->alloc_timer(o, fd_close_timeout, fdll);
    if (!fdll->close_timer)
	goto out_nomem;

    fdll->deferred_op_runner = o->alloc_runner(o, fd_deferred_op, fdll);
    if (!fdll->deferred_op_runner)
	goto out_nomem;

    fdll->lock = o->alloc_lock(o);
    if (!fdll->lock)
	goto out_nomem;

    fdll->read_data_size = max_read_size;
    fdll->read_data = o->zalloc(o, max_read_size);
    if (!fdll->read_data)
	goto out_nomem;

    fdll->ll.ops = &fd_ll_ops;

    if (fd != -1) {
	int err = fd_setup_handlers(fdll);
	if (err)
	    goto out_nomem;
    }

    return &fdll->ll;

 out_nomem:
    fd_finish_free(fdll);
    return NULL;
}
