#ifndef SERIALIZER_H
#define SERIALIZER_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define OPT_INDENT_2 1
#define OPT_NAIVE_UTC 2
#define OPT_NON_STR_KEYS 4
#define OPT_OMIT_MICROSECONDS 8
#define OPT_SERIALIZE_NUMPY 16
#define OPT_SORT_KEYS 32
#define OPT_STRICT_INTEGER 64
#define OPT_UTC_Z 128
#define OPT_PASSTHROUGH_SUBCLASS 256
#define OPT_PASSTHROUGH_DATETIME 512
#define OPT_APPEND_NEWLINE 1024
#define OPT_PASSTHROUGH_DATACLASS 2048
#define OPT_SERIALIZE_DATACLASS 0
#define OPT_SERIALIZE_UUID 0

#define OPT_ALL_MASK (OPT_INDENT_2 | OPT_NAIVE_UTC | OPT_NON_STR_KEYS | \
                      OPT_OMIT_MICROSECONDS | OPT_SERIALIZE_NUMPY | \
                      OPT_SORT_KEYS | OPT_STRICT_INTEGER | OPT_UTC_Z | \
                      OPT_PASSTHROUGH_SUBCLASS | OPT_PASSTHROUGH_DATETIME | \
                      OPT_APPEND_NEWLINE | OPT_PASSTHROUGH_DATACLASS)

typedef struct {
  PyObject_HEAD
  PyObject *contents;
} FragmentObject;

extern PyTypeObject Fragment_Type;

/* Fastcall entry point for dumps(obj, /, default=None, option=None) */
PyObject *c_json_dumps(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames);

int init_serializer_types(void);

#endif /* SERIALIZER_H */
