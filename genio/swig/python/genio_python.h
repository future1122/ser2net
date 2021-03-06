/*
 *  ser2net - A program for allowing telnet connection to serial ports
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

typedef PyObject swig_cb;
typedef PyObject swig_cb_val;
typedef struct swig_ref {
    PyObject *val;
} swig_ref;

#define nil_swig_cb(v) ((v) == NULL)
#define invalidate_swig_cb(v) ((v) = NULL)

#ifdef WITH_THREAD
static void genio_swig_init_lang(void)
{
    PyEval_InitThreads();
}
#define OI_PY_STATE PyGILState_STATE
#define OI_PY_STATE_GET() PyGILState_Ensure()
#define OI_PY_STATE_PUT(s) PyGILState_Release(s)

/* We do need to work about blocking, though. */
#define GENIO_SWIG_C_BLOCK_ENTRY Py_BEGIN_ALLOW_THREADS
#define GENIO_SWIG_C_BLOCK_EXIT Py_END_ALLOW_THREADS
#else
static void genio_swig_init_lang(void)
{
}
#define OI_PY_STATE int
#define OI_PY_STATE_GET() 0
#define OI_PY_STATE_PUT(s) do { } while(s)

/* No threads */
#define GENIO_SWIG_C_BLOCK_ENTRY
#define GENIO_SWIG_C_BLOCK_EXIT
#endif

static swig_cb_val *
ref_swig_cb_i(swig_cb *cb)
{
    OI_PY_STATE gstate;

    gstate = OI_PY_STATE_GET();
    Py_INCREF(cb);
    OI_PY_STATE_PUT(gstate);
    return cb;
}
#define ref_swig_cb(cb, func) ref_swig_cb_i(cb)

static swig_ref
swig_make_ref_i(void *item, swig_type_info *class)
{
    swig_ref    rv;
    OI_PY_STATE gstate;

    gstate = OI_PY_STATE_GET();
    rv.val = SWIG_NewPointerObj(item, class, 0);
    OI_PY_STATE_PUT(gstate);
    return rv;
}
#define swig_make_ref(item, name) \
	swig_make_ref_i(item, SWIGTYPE_p_ ## name)

static void
swig_free_ref(swig_ref ref)
{
    OI_PY_STATE gstate;

    gstate = OI_PY_STATE_GET();
    Py_DECREF(ref.val);
    OI_PY_STATE_PUT(gstate);
}

static swig_cb_val *
deref_swig_cb_val(swig_cb_val *cb)
{
    OI_PY_STATE gstate;

    gstate = OI_PY_STATE_GET();
    Py_DECREF(cb);
    OI_PY_STATE_PUT(gstate);
    return cb;
}

/* No way to check the refcount in Python. */
#define swig_free_ref_check(r, c) \
	do {								\
	    swig_free_ref(r);						\
	} while(0)

static PyObject *
swig_finish_call_rv(swig_cb_val *cb, const char *method_name, PyObject *args)
{
    PyObject *p, *o = NULL;

    p = PyObject_GetAttrString(cb, method_name);
    if (p) {
	o = PyObject_CallObject(p, args);
	Py_DECREF(p);
	if (PyErr_Occurred())
	    wake_curr_waiter();
    } else {
	PyObject *t = PyObject_GetAttrString(cb, "__class__");
	PyObject *c = PyObject_GetAttrString(t, "__name__");
	char *class = PyString_AsString(c);

	PyErr_Format(PyExc_RuntimeError,
		     "genio callback: Class '%s' has no method '%s'\n",
		     class, method_name);
	wake_curr_waiter();
    }
    Py_DECREF(args);

    return o;
}

static void
swig_finish_call(swig_cb_val *cb, const char *method_name, PyObject *args)
{
    PyObject *o;

    o = swig_finish_call_rv(cb, method_name, args);
    if (o)
	Py_DECREF(o);
}

#if PY_VERSION_HEX >= 0x03000000
#define OI_PI_FromStringAndSize PyUnicode_FromStringAndSize
#define OI_PI_FromString PyUnicode_FromString
#else
#define OI_PI_FromStringAndSize PyString_FromStringAndSize
#define OI_PI_FromString PyString_FromString
#endif

struct genio_data {
    int refcount;
    swig_cb_val *handler_val;
    struct genio_os_funcs *o;
};

static void
genio_open_done(struct genio *io, int err, void *cb_data) {
    swig_cb_val *cb = cb_data;
    swig_ref io_ref;
    PyObject *args, *o;
    OI_PY_STATE gstate;

    gstate = OI_PY_STATE_GET();

    io_ref = swig_make_ref(io, genio);
    args = PyTuple_New(2);
    Py_INCREF(io_ref.val);
    PyTuple_SET_ITEM(args, 0, io_ref.val);
    o = PyInt_FromLong(err);
    PyTuple_SET_ITEM(args, 1, o);

    swig_finish_call(cb, "open_done", args);

    swig_free_ref_check(io_ref, acceptor);
    deref_swig_cb_val(cb);
    OI_PY_STATE_PUT(gstate);
}

static void
genio_close_done(struct genio *io, void *cb_data) {
    swig_cb_val *cb = cb_data;
    swig_ref io_ref;
    PyObject *args;
    OI_PY_STATE gstate;

    gstate = OI_PY_STATE_GET();

    io_ref = swig_make_ref(io, genio);
    args = PyTuple_New(1);
    Py_INCREF(io_ref.val);
    PyTuple_SET_ITEM(args, 0, io_ref.val);

    swig_finish_call(cb, "close_done", args);

    swig_free_ref_check(io_ref, acceptor);
    deref_swig_cb_val(cb);
    OI_PY_STATE_PUT(gstate);
}

static unsigned int
genio_got_read(struct genio *io, int readerr,
	       unsigned char *buf, unsigned int buflen,
	       unsigned int flags)
{
    struct genio_data *data = genio_get_user_data(io);
    swig_ref io_ref;
    PyObject *args, *o;
    OI_PY_STATE gstate;
    unsigned int rv;

    gstate = OI_PY_STATE_GET();

    if (!data->handler_val) {
	PyErr_Format(PyExc_RuntimeError, "genio callback: "
		     "genio handler was not set");
	wake_curr_waiter();
	rv = 0;
	goto out_put;
    }

    args = PyTuple_New(4);

    io_ref = swig_make_ref(io, genio);
    Py_INCREF(io_ref.val);
    PyTuple_SET_ITEM(args, 0, io_ref.val);

    if (readerr) {
	o = OI_PI_FromString(strerror(readerr));
    } else {
	Py_INCREF(Py_None);
	o = Py_None;
    }
    PyTuple_SET_ITEM(args, 1, o);

    o = OI_PI_FromStringAndSize((char *) buf, buflen);
    PyTuple_SET_ITEM(args, 2, o);

    o = PyInt_FromLong(flags);
    PyTuple_SET_ITEM(args, 3, o);

    o = swig_finish_call_rv(data->handler_val, "read_callback", args);
    if (o) {
	rv = PyLong_AsUnsignedLong(o);
	if (PyErr_Occurred()) {
	    PyObject *t = PyObject_GetAttrString(data->handler_val,
						 "__class__");
	    PyObject *c = PyObject_GetAttrString(t, "__name__");
	    char *class = PyString_AsString(c);

	    PyErr_Format(PyExc_RuntimeError, "genio callback: "
			 "Class '%s' method 'read_callback' did not return "
			 "an integer\n", class);
	    wake_curr_waiter();
	}
	Py_DECREF(o);
    }

    swig_free_ref_check(io_ref, acceptor);
 out_put:
    OI_PY_STATE_PUT(gstate);

    return rv;
}

static void
genio_write_read(struct genio *io)
{
    struct genio_data *data = genio_get_user_data(io);
    swig_ref io_ref;
    PyObject *args;
    OI_PY_STATE gstate;

    gstate = OI_PY_STATE_GET();

    if (!data->handler_val) {
	PyErr_Format(PyExc_RuntimeError, "genio callback: "
		     "genio handler was not set");
	wake_curr_waiter();
	goto out_put;
    }

    io_ref = swig_make_ref(io, genio);
    args = PyTuple_New(1);
    Py_INCREF(io_ref.val);
    PyTuple_SET_ITEM(args, 0, io_ref.val);

    swig_finish_call(data->handler_val, "write_callback", args);

    swig_free_ref_check(io_ref, acceptor);
 out_put:
    OI_PY_STATE_PUT(gstate);
}

static void
genio_got_urgent(struct genio *io)
{
    struct genio_data *data = genio_get_user_data(io);
    swig_ref io_ref;
    PyObject *args;
    OI_PY_STATE gstate;

    gstate = OI_PY_STATE_GET();

    if (!data->handler_val) {
	PyErr_Format(PyExc_RuntimeError, "genio callback: "
		     "genio handler was not set");
	wake_curr_waiter();
	goto out_put;
    }

    io_ref = swig_make_ref(io, genio);
    args = PyTuple_New(1);
    Py_INCREF(io_ref.val);
    PyTuple_SET_ITEM(args, 0, io_ref.val);

    swig_finish_call(data->handler_val, "urgent_callback", args);

    swig_free_ref_check(io_ref, acceptor);
 out_put:
    OI_PY_STATE_PUT(gstate);
}

static struct genio_callbacks gen_cbs = {
    .read_callback = genio_got_read,
    .write_callback = genio_write_read,
    .urgent_callback = genio_got_urgent
};

struct genio_acc_data {
    swig_cb_val *handler_val;
    struct genio_os_funcs *o;
};

static void
genio_acc_shutdown_done(struct genio_acceptor *acceptor, void *cb_data)
{
    swig_cb_val *cb = cb_data;
    swig_ref acc_ref;
    PyObject *args;
    OI_PY_STATE gstate;

    gstate = OI_PY_STATE_GET();

    acc_ref = swig_make_ref(acceptor, genio_acceptor);
    args = PyTuple_New(1);
    Py_INCREF(acc_ref.val);
    PyTuple_SET_ITEM(args, 0, acc_ref.val);

    swig_finish_call(cb, "shutdown_done", args);

    swig_free_ref_check(acc_ref, acceptor);
    deref_swig_cb_val(cb);
    OI_PY_STATE_PUT(gstate);
}

static void
genio_acc_got_new(struct genio_acceptor *acceptor, struct genio *io)
{
    struct genio_acc_data *data = genio_acc_get_user_data(acceptor);
    swig_ref acc_ref, io_ref;
    PyObject *args;
    OI_PY_STATE gstate;
    struct genio_data *iodata;

    iodata = malloc(sizeof(*data));
    if (!iodata)
	return;
    iodata->refcount = 1;
    iodata->handler_val = NULL;
    iodata->o = data->o;
    genio_set_callbacks(io, &gen_cbs, iodata);

    gstate = OI_PY_STATE_GET();

    acc_ref = swig_make_ref(acceptor, genio_acceptor);
    io_ref = swig_make_ref(io, genio);
    args = PyTuple_New(2);
    Py_INCREF(acc_ref.val);
    Py_INCREF(io_ref.val);
    PyTuple_SET_ITEM(args, 0, acc_ref.val);
    PyTuple_SET_ITEM(args, 1, io_ref.val);

    swig_finish_call(data->handler_val, "new_connection", args);

    swig_free_ref_check(acc_ref, acceptor);
    swig_free_ref_check(io_ref, acceptor);
    OI_PY_STATE_PUT(gstate);
}

static struct genio_acceptor_callbacks gen_acc_cbs = {
    .new_connection = genio_acc_got_new
};

struct sergenio_cbdata {
    const char *cbname;
    swig_cb_val *h_val;
};

#define stringify_1(x...)     #x
#define stringify(x...)       stringify_1(x)

#define sergenio_cbdata(name, h) \
({							\
    struct sergenio_cbdata *cbd = malloc(sizeof(*cbd));	\
    if (cbd) {						\
	cbd->cbname = stringify(name);			\
	cbd->h_val = ref_swig_cb(h, name);		\
    }							\
    cbd;						\
 })

static void
cleanup_sergenio_cbdata(struct sergenio_cbdata *cbd)
{
    deref_swig_cb_val(cbd->h_val);
    free(cbd);
}

static void
sergenio_cb(struct sergenio *snet, int err, int val, void *cb_data)
{
    struct sergenio_cbdata *cbd = cb_data;
    PyObject *o, *args;
    OI_PY_STATE gstate;

    gstate = OI_PY_STATE_GET();

    args = PyTuple_New(2);
    o = PyInt_FromLong(err);
    PyTuple_SET_ITEM(args, 0, o);
    o = PyInt_FromLong(val);
    PyTuple_SET_ITEM(args, 1, o);

    swig_finish_call(cbd->h_val, cbd->cbname, args);

    cleanup_sergenio_cbdata(cbd);
    OI_PY_STATE_PUT(gstate);
}

static PyObject *
add_python_result(PyObject *result, PyObject *val)
{
    if ((result == Py_None)) {
	Py_XDECREF(result);
	result = val;
    } else {
	PyObject *seq, *o2;

	if (!PyTuple_Check(result)) {
	    PyObject *tmpr = result;

	    result = PyTuple_New(1);
	    PyTuple_SetItem(result, 0, tmpr);
	}
	seq = PyTuple_New(1);
	PyTuple_SetItem(seq, 0, val);
	o2 = result;
	result = PySequence_Concat(o2, seq);
	Py_DECREF(o2);
	Py_DECREF(seq);
    }
    return result;
}

#define check_for_err PyErr_Occurred

static void err_handle(char *name, int rv)
{
    if (!rv)
	return;
    PyErr_Format(PyExc_Exception, "genio:%s: %s", name, strerror(rv));
}

static void ser_err_handle(char *name, int rv)
{
    if (!rv)
	return;
    PyErr_Format(PyExc_Exception, "sergenio:%s: %s", name, strerror(rv));
}

static void cast_error(char *to, char *from)
{
    PyErr_Format(PyExc_RuntimeError, "Error casting from %s to %s", from, to);
}

static void oom_err(void)
{
    PyErr_Format(PyExc_MemoryError, "Out of memory");
}
