#ifndef DESERIALIZER_H
#define DESERIALIZER_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

extern PyObject *JSONDecodeError;

/* Fastcall entry point for loads(obj, /) */
PyObject *c_json_loads(PyObject *self, PyObject *const *args, Py_ssize_t nargs);

int init_deserializer_types(void);

#endif /* DESERIALIZER_H */
