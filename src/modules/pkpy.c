#include "pocketpy/interpreter/objectpool.h"
#include "pocketpy/objects/base.h"
#include "pocketpy/pocketpy.h"

#include "pocketpy/common/utils.h"
#include "pocketpy/objects/object.h"
#include "pocketpy/common/sstream.h"
#include "pocketpy/interpreter/vm.h"

#include <threads.h>
#include <stdatomic.h>

#define DEF_TVALUE_METHODS(T, Field)                                                               \
    static bool TValue_##T##__new__(int argc, py_Ref argv) {                                       \
        PY_CHECK_ARGC(2);                                                                          \
        PY_CHECK_ARG_TYPE(0, tp_type);                                                             \
        PY_CHECK_ARG_TYPE(1, tp_##T);                                                              \
        *py_retval() = (py_TValue){                                                                \
            .type = py_totype(&argv[0]),                                                           \
            .is_ptr = false,                                                                       \
            .Field = py_to##T(&argv[1]),                                                           \
        };                                                                                         \
        return true;                                                                               \
    }                                                                                              \
    static bool TValue_##T##_value(int argc, py_Ref argv) {                                        \
        PY_CHECK_ARGC(1);                                                                          \
        py_new##T(py_retval(), argv->Field);                                                       \
        return true;                                                                               \
    }                                                                                              \
    static bool TValue_##T##__repr__(int argc, py_Ref argv) {                                      \
        PY_CHECK_ARGC(1);                                                                          \
        py_newstr(py_retval(), "<TValue_" #T " object>");                                          \
        return true;                                                                               \
    }

DEF_TVALUE_METHODS(int, _i64)
DEF_TVALUE_METHODS(float, _f64)
DEF_TVALUE_METHODS(vec2, _vec2)
DEF_TVALUE_METHODS(vec2i, _vec2i)

static bool pkpy_memory_usage(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    ManagedHeap* heap = &pk_current_vm->heap;
    c11_string* small_objects_usage = MultiPool__summary(&heap->small_objects);
    int large_object_count = heap->large_objects.length;
    c11_sbuf buf;
    c11_sbuf__ctor(&buf);
    c11_sbuf__write_cstr(&buf, "== heap.small_objects ==\n");
    c11_sbuf__write_cstr(&buf, small_objects_usage->data);
    c11_sbuf__write_cstr(&buf, "== heap.large_objects ==\n");
    pk_sprintf(&buf, "len(large_objects)=%d\n", large_object_count);
    c11_sbuf__write_cstr(&buf, "== heap.gc ==\n");
    pk_sprintf(&buf, "gc_counter=%d\n", heap->gc_counter);
    pk_sprintf(&buf, "gc_threshold=%d", heap->gc_threshold);
    // c11_sbuf__write_cstr(&buf, "== vm.pool_frame ==\n");
    c11_sbuf__py_submit(&buf, py_retval());
    c11_string__delete(small_objects_usage);
    return true;
}

static bool pkpy_is_user_defined_type(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    PY_CHECK_ARG_TYPE(0, tp_type);
    py_TypeInfo* ti = pk__type_info(py_totype(argv));
    py_newbool(py_retval(), ti->is_python);
    return true;
}

static bool pkpy_currentvm(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    py_newint(py_retval(), py_currentvm());
    return true;
}

typedef struct c11_ComputeThread c11_ComputeThread;

typedef struct {
    c11_ComputeThread* self;
    char* eval_src;
    unsigned char* args_data;
    int args_size;
    unsigned char* kwargs_data;
    int kwargs_size;
} ComputeThreadJobCall;

typedef struct {
    c11_ComputeThread* self;
    char* source;
    enum py_CompileMode mode;
} ComputeThreadJobExec;

static void ComputeThreadJobCall__dtor(void* arg) {
    ComputeThreadJobCall* self = arg;
    PK_FREE(self->eval_src);
    PK_FREE(self->args_data);
    PK_FREE(self->kwargs_data);
}

static void ComputeThreadJobExec__dtor(void* arg) {
    ComputeThreadJobExec* self = arg;
    PK_FREE(self->source);
}

typedef struct c11_ComputeThread {
    int vm_index;
    atomic_bool is_done;
    unsigned char* last_retval_data;
    int last_retval_size;
    char* last_error;

    thrd_t thread;
    void* job;
    void (*job_dtor)(void*);
} c11_ComputeThread;

static void
    c11_ComputeThread__reset_job(c11_ComputeThread* self, void* job, void (*job_dtor)(void*)) {
    if(self->job) {
        self->job_dtor(self->job);
        PK_FREE(self->job);
    }
    self->job = job;
    self->job_dtor = job_dtor;
}

static bool _pk_compute_thread_flags[16];

static void c11_ComputeThread__dtor(c11_ComputeThread* self) {
    if(!self->is_done) {
        c11__abort("ComputeThread(%d) is not done yet!! But the object was deleted.",
                   self->vm_index);
    }
    c11_ComputeThread__reset_job(self, NULL, NULL);
    _pk_compute_thread_flags[self->vm_index] = false;
}

static void c11_ComputeThread__on_job_begin(c11_ComputeThread* self) {
    if(self->last_retval_data) {
        PK_FREE(self->last_retval_data);
        self->last_retval_data = NULL;
        self->last_retval_size = 0;
    }
    if(self->last_error) {
        PK_FREE(self->last_error);
        self->last_error = NULL;
    }
    py_switchvm(self->vm_index);
}

static bool ComputeThread__new__(int argc, py_Ref argv) {
    c11_ComputeThread* self =
        py_newobject(py_retval(), py_totype(argv), 0, sizeof(c11_ComputeThread));
    self->vm_index = 0;
    self->is_done = true;
    self->last_retval_data = NULL;
    self->last_retval_size = 0;
    self->last_error = NULL;
    self->job = NULL;
    self->job_dtor = NULL;
    return true;
}

static bool ComputeThread__init__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    PY_CHECK_ARG_TYPE(1, tp_int);
    c11_ComputeThread* self = py_touserdata(py_arg(0));
    int index = py_toint(py_arg(1));
    if(index >= 1 && index < 16) {
        if(_pk_compute_thread_flags[index]) {
            return ValueError("vm_index %d is already in use", index);
        }
        _pk_compute_thread_flags[index] = true;
        self->vm_index = index;
    } else {
        return ValueError("vm_index %d is out of range", index);
    }
    py_newnone(py_retval());
    return true;
}

static bool ComputeThread_is_done(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    c11_ComputeThread* self = py_touserdata(argv);
    py_newbool(py_retval(), self->is_done);
    return true;
}

static bool ComputeThread_join(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    c11_ComputeThread* self = py_touserdata(argv);
    while(!self->is_done)
        thrd_yield();
    py_newnone(py_retval());
    return true;
}

static bool ComputeThread_last_error(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    c11_ComputeThread* self = py_touserdata(argv);
    if(!self->is_done) return OSError("thread is not done yet");
    if(self->last_error) {
        py_newstr(py_retval(), self->last_error);
    } else {
        py_newnone(py_retval());
    }
    return true;
}

static bool ComputeThread_last_retval(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    c11_ComputeThread* self = py_touserdata(argv);
    if(!self->is_done) return OSError("thread is not done yet");
    if(self->last_retval_data == NULL) return ValueError("no retval available");
    return py_pickle_loads(self->last_retval_data, self->last_retval_size);
}

static int ComputeThreadJob_call(void* arg) {
    ComputeThreadJobCall* job = arg;
    c11_ComputeThread* self = job->self;
    c11_ComputeThread__on_job_begin(self);

    py_StackRef p0 = py_peek(0);

    if(!py_pusheval(job->eval_src, NULL)) goto __ERROR;
    // [callable]
    if(!py_pickle_loads(job->args_data, job->args_size)) goto __ERROR;
    py_push(py_retval());
    // [callable, args]
    if(!py_pickle_loads(job->kwargs_data, job->kwargs_size)) goto __ERROR;
    py_push(py_retval());
    // [callable, args, kwargs]
    if(!py_smarteval("_0(*_1, **_2)", NULL, py_peek(-3), py_peek(-2), py_peek(-1))) goto __ERROR;

    py_shrink(3);
    if(!py_pickle_dumps(py_retval())) goto __ERROR;
    int retval_size;
    unsigned char* retval_data = py_tobytes(py_retval(), &retval_size);
    self->last_retval_data = c11_memdup(retval_data, retval_size);
    self->last_retval_size = retval_size;
    self->is_done = true;
    return 0;

__ERROR:
    self->last_error = py_formatexc();
    self->is_done = true;
    py_clearexc(p0);
    py_newnone(py_retval());
    return 0;
}

static int ComputeThreadJob_exec(void* arg) {
    ComputeThreadJobExec* job = arg;
    c11_ComputeThread* self = job->self;
    c11_ComputeThread__on_job_begin(self);

    py_StackRef p0 = py_peek(0);
    if(!py_exec(job->source, "<job>", job->mode, NULL)) goto __ERROR;
    if(!py_pickle_dumps(py_retval())) goto __ERROR;
    int retval_size;
    unsigned char* retval_data = py_tobytes(py_retval(), &retval_size);
    self->last_retval_data = c11_memdup(retval_data, retval_size);
    self->last_retval_size = retval_size;
    self->is_done = true;
    return 0;

__ERROR:
    self->last_error = py_formatexc();
    self->is_done = true;
    py_clearexc(p0);
    return 0;
}

static bool ComputeThread_exec(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    c11_ComputeThread* self = py_touserdata(py_arg(0));
    if(!self->is_done) return OSError("thread is not done yet");
    PY_CHECK_ARG_TYPE(1, tp_str);
    const char* source = py_tostr(py_arg(1));
    /**************************/
    ComputeThreadJobExec* job = PK_MALLOC(sizeof(ComputeThreadJobExec));
    job->self = self;
    job->source = c11_strdup(source);
    job->mode = EXEC_MODE;
    c11_ComputeThread__reset_job(self, job, ComputeThreadJobExec__dtor);
    /**************************/
    self->is_done = false;
    int res = thrd_create(&self->thread, ComputeThreadJob_exec, job);
    if(res != thrd_success) {
        self->is_done = true;
        return OSError("thrd_create() failed");
    }
    py_newnone(py_retval());
    return true;
}

static bool ComputeThread_eval(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    c11_ComputeThread* self = py_touserdata(py_arg(0));
    if(!self->is_done) return OSError("thread is not done yet");
    PY_CHECK_ARG_TYPE(1, tp_str);
    const char* source = py_tostr(py_arg(1));
    /**************************/
    ComputeThreadJobExec* job = PK_MALLOC(sizeof(ComputeThreadJobExec));
    job->self = self;
    job->source = c11_strdup(source);
    job->mode = EVAL_MODE;
    c11_ComputeThread__reset_job(self, job, ComputeThreadJobExec__dtor);
    /**************************/
    self->is_done = false;
    int res = thrd_create(&self->thread, ComputeThreadJob_exec, job);
    if(res != thrd_success) {
        self->is_done = true;
        return OSError("thrd_create() failed");
    }
    py_newnone(py_retval());
    return true;
}

static bool ComputeThread_call(int argc, py_Ref argv) {
    PY_CHECK_ARGC(4);
    c11_ComputeThread* self = py_touserdata(py_arg(0));
    if(!self->is_done) return OSError("thread is not done yet");
    PY_CHECK_ARG_TYPE(1, tp_str);
    PY_CHECK_ARG_TYPE(2, tp_tuple);
    PY_CHECK_ARG_TYPE(3, tp_dict);
    // eval_src
    const char* eval_src = py_tostr(py_arg(1));
    // *args
    if(!py_pickle_dumps(py_arg(2))) return false;
    int args_size;
    unsigned char* args_data = py_tobytes(py_retval(), &args_size);
    // *kwargs
    if(!py_pickle_dumps(py_arg(3))) return false;
    int kwargs_size;
    unsigned char* kwargs_data = py_tobytes(py_retval(), &kwargs_size);
    /**************************/
    ComputeThreadJobCall* job = PK_MALLOC(sizeof(ComputeThreadJobCall));
    job->self = self;
    job->eval_src = c11_strdup(eval_src);
    job->args_data = c11_memdup(args_data, args_size);
    job->args_size = args_size;
    job->kwargs_data = c11_memdup(kwargs_data, kwargs_size);
    job->kwargs_size = kwargs_size;
    c11_ComputeThread__reset_job(self, job, ComputeThreadJobCall__dtor);
    /**************************/
    self->is_done = false;
    int res = thrd_create(&self->thread, ComputeThreadJob_call, job);
    if(res != thrd_success) {
        self->is_done = true;
        return OSError("thrd_create() failed");
    }
    py_newnone(py_retval());
    return true;
}

static void pk_ComputeThread__register(py_Ref mod) {
    py_Type type = py_newtype("ComputeThread", tp_object, mod, (py_Dtor)c11_ComputeThread__dtor);

    py_bindmagic(type, __new__, ComputeThread__new__);
    py_bindmagic(type, __init__, ComputeThread__init__);
    py_bindproperty(type, "is_done", ComputeThread_is_done, NULL);
    py_bindmethod(type, "join", ComputeThread_join);
    py_bindmethod(type, "last_error", ComputeThread_last_error);
    py_bindmethod(type, "last_retval", ComputeThread_last_retval);

    py_bindmethod(type, "exec", ComputeThread_exec);
    py_bindmethod(type, "eval", ComputeThread_eval);
    py_bind(py_tpobject(type), "call(self, eval_src, *args, **kwargs)", ComputeThread_call);
}

void pk__add_module_pkpy() {
    py_Ref mod = py_newmodule("pkpy");

    py_Type ttype;
    py_Ref TValue_dict = py_pushtmp();
    py_newdict(TValue_dict);

    ttype = pk_newtype("TValue_int", tp_object, mod, NULL, false, false);
    py_bindmagic(ttype, __new__, TValue_int__new__);
    py_bindmagic(ttype, __repr__, TValue_int__repr__);
    py_bindproperty(ttype, "value", TValue_int_value, NULL);
    py_dict_setitem(TValue_dict, py_tpobject(tp_int), py_tpobject(ttype));

    ttype = pk_newtype("TValue_float", tp_object, mod, NULL, false, false);
    py_bindmagic(ttype, __new__, TValue_float__new__);
    py_bindmagic(ttype, __repr__, TValue_float__repr__);
    py_bindproperty(ttype, "value", TValue_float_value, NULL);
    py_dict_setitem(TValue_dict, py_tpobject(tp_float), py_tpobject(ttype));

    ttype = pk_newtype("TValue_vec2", tp_object, mod, NULL, false, false);
    py_bindmagic(ttype, __new__, TValue_vec2__new__);
    py_bindmagic(ttype, __repr__, TValue_vec2__repr__);
    py_bindproperty(ttype, "value", TValue_vec2_value, NULL);
    py_dict_setitem(TValue_dict, py_tpobject(tp_vec2), py_tpobject(ttype));

    ttype = pk_newtype("TValue_vec2i", tp_object, mod, NULL, false, false);
    py_bindmagic(ttype, __new__, TValue_vec2i__new__);
    py_bindmagic(ttype, __repr__, TValue_vec2i__repr__);
    py_bindproperty(ttype, "value", TValue_vec2i_value, NULL);
    py_dict_setitem(TValue_dict, py_tpobject(tp_vec2i), py_tpobject(ttype));

    py_setdict(mod, py_name("TValue"), TValue_dict);
    py_pop();

    py_bindfunc(mod, "memory_usage", pkpy_memory_usage);
    py_bindfunc(mod, "is_user_defined_type", pkpy_is_user_defined_type);

    py_bindfunc(mod, "currentvm", pkpy_currentvm);

    pk_ComputeThread__register(mod);
}

#undef DEF_TVALUE_METHODS