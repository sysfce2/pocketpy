#pragma once

#include "pocketpy/objects/namedict.h"
#include "pocketpy/objects/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PyObject{
    Type type;          // we have a duplicated type here for convenience
    bool gc_is_large;
    bool gc_marked;
    pk_NameDict* dict;  // gc will delete this on destruction
} PyObject;

static_assert(sizeof(PyObject) <= 16, "!(sizeof(PyObject) <= 16)");

#define PyObject__value_ptr(self)   ((char*)self + 16)
#define PyObject__as(T, self)       (T*)(PyObject__value_ptr(self))
#define PK_OBJ_GET(T, val)          (*(T*)(PyObject__value_ptr((val)._obj)))
#define PK_OBJ_SIZEOF(T)            (sizeof(T) + 16)

PyObject* PyObject__new(Type type, int size);
void PyObject__delete(PyObject* self);

PK_INLINE PyVar PyVar__fromobj(PyObject* obj){
    if(!obj) return PY_NULL;
    PyVar retval = {
        .type = obj->type,
        .is_ptr = true,
        ._obj = obj
    };
    return retval;
}

#ifdef __cplusplus
}
#endif