#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "serializer.h"
#include "deserializer.h"
#include "cjson_simd.h"

static PyMethodDef CJsonMethods[] = {
    {"dumps", (PyCFunction)c_json_dumps, METH_FASTCALL | METH_KEYWORDS,
     "Serialize Python objects to JSON."},
    {"loads", (PyCFunction)c_json_loads, METH_FASTCALL,
     "Deserialize JSON to Python objects."},
    {NULL, NULL, 0, NULL} /* Sentinel */
};

static struct PyModuleDef cxjsonmodule = {
    PyModuleDef_HEAD_INIT,
    "_cxjson",
    "C extension module for cxjson",
    -1,
    CJsonMethods,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit__cxjson(void) {
    cjson_simd_init();
    if (init_serializer_types() < 0) {
        return NULL;
    }
    if (init_deserializer_types() < 0) {
        return NULL;
    }

    PyObject *m = PyModule_Create(&cxjsonmodule);
    if (m == NULL) {
        return NULL;
    }

    Py_INCREF(&Fragment_Type);
    if (PyModule_AddObject(m, "Fragment", (PyObject *)&Fragment_Type) < 0) {
        Py_DECREF(&Fragment_Type);
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(JSONDecodeError);
    if (PyModule_AddObject(m, "JSONDecodeError", JSONDecodeError) < 0) {
        Py_DECREF(JSONDecodeError);
        Py_DECREF(m);
        return NULL;
    }

    PyModule_AddIntConstant(m, "OPT_INDENT_2", OPT_INDENT_2);
    PyModule_AddIntConstant(m, "OPT_NAIVE_UTC", OPT_NAIVE_UTC);
    PyModule_AddIntConstant(m, "OPT_NON_STR_KEYS", OPT_NON_STR_KEYS);
    PyModule_AddIntConstant(m, "OPT_OMIT_MICROSECONDS", OPT_OMIT_MICROSECONDS);
    PyModule_AddIntConstant(m, "OPT_SERIALIZE_NUMPY", OPT_SERIALIZE_NUMPY);
    PyModule_AddIntConstant(m, "OPT_SORT_KEYS", OPT_SORT_KEYS);
    PyModule_AddIntConstant(m, "OPT_STRICT_INTEGER", OPT_STRICT_INTEGER);
    PyModule_AddIntConstant(m, "OPT_UTC_Z", OPT_UTC_Z);
    PyModule_AddIntConstant(m, "OPT_PASSTHROUGH_SUBCLASS", OPT_PASSTHROUGH_SUBCLASS);
    PyModule_AddIntConstant(m, "OPT_PASSTHROUGH_DATETIME", OPT_PASSTHROUGH_DATETIME);
    PyModule_AddIntConstant(m, "OPT_APPEND_NEWLINE", OPT_APPEND_NEWLINE);
    PyModule_AddIntConstant(m, "OPT_PASSTHROUGH_DATACLASS", OPT_PASSTHROUGH_DATACLASS);
    PyModule_AddIntConstant(m, "OPT_SERIALIZE_DATACLASS", OPT_SERIALIZE_DATACLASS);
    PyModule_AddIntConstant(m, "OPT_SERIALIZE_UUID", OPT_SERIALIZE_UUID);

    return m;
}
