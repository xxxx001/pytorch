#define PY_SSIZE_T_CLEAN
#include <torch/csrc/dynamo/cpp_shim.h>
#include <torch/csrc/dynamo/cpython_defs.h>
#include <torch/csrc/utils/python_compat.h>
#include <opcode.h>
#include <stdbool.h>

// Problem in CPython includes when mixing core and non-core build
// The fix was not backported to 3.12 so this is needed here
// https://github.com/python/cpython/issues/105268
#if IS_PYTHON_3_12_PLUS
#undef _PyGC_FINALIZED
#endif

// see https://bugs.python.org/issue35886
#if PY_VERSION_HEX >= 0x03080000
#define Py_BUILD_CORE
#include <internal/pycore_pystate.h>

// These headers were added in 3.11
#if IS_PYTHON_3_11_PLUS
#include <internal/pycore_frame.h>
#endif

#undef Py_BUILD_CORE
#endif // PY_VERSION_HEX >= 0x03080000

// All the eval APIs change in 3.11 so we need to decide which one to use on the fly
// https://docs.python.org/3/c-api/init.html#c._PyFrameEvalFunction
#if IS_PYTHON_3_11_PLUS
#define THP_EVAL_API_FRAME_OBJECT _PyInterpreterFrame

// We need to be able to return the _PyInterpreterFrame to python so create
// a python binding for it

typedef struct THPPyInterpreterFrame {
  PyObject_HEAD
  _PyInterpreterFrame* frame; // Borrowed reference
} THPPyInterpreterFrame;

THPPyInterpreterFrame* THPPyInterpreterFrame_New(_PyInterpreterFrame* frame);

#define DECLARE_PYOBJ_ATTR(name) \
static PyObject* THPPyInterpreterFrame_##name(THPPyInterpreterFrame* self, PyObject* _noargs) { \
  PyObject* res = (PyObject*)self->frame->name; \
  Py_XINCREF(res); \
  return res; \
}

#if IS_PYTHON_3_12_PLUS
DECLARE_PYOBJ_ATTR(f_funcobj)
#else
DECLARE_PYOBJ_ATTR(f_func)
#endif
DECLARE_PYOBJ_ATTR(f_globals)
DECLARE_PYOBJ_ATTR(f_builtins)
DECLARE_PYOBJ_ATTR(f_locals)
DECLARE_PYOBJ_ATTR(f_code)
DECLARE_PYOBJ_ATTR(frame_obj)

#undef DECLARE_PYOBJ_ATTR

static THPPyInterpreterFrame* THPPyInterpreterFrame_previous(THPPyInterpreterFrame* self, PyObject* _noargs) {
  THPPyInterpreterFrame* res = THPPyInterpreterFrame_New(self->frame->previous);
  return res;
}

// This is not a true attribute of the class but we do access it in python and it is hard to implement
// on the python side, so do it here:
static PyObject* THPPyInterpreterFrame_f_lasti(THPPyInterpreterFrame* self, PyObject* _noargs) {
  return PyLong_FromLong(_PyInterpreterFrame_LASTI(self->frame));
}

static PyObject* THPPyInterpreterFrame_f_lineno(THPPyInterpreterFrame* self, PyObject* _noargs) {
  if (!self->frame->frame_obj) {
    return PyLong_FromLong(self->frame->f_code->co_firstlineno);
  }
  int lineno = PyFrame_GetLineNumber(self->frame->frame_obj);
  if (lineno < 0) {
    Py_RETURN_NONE;
  }
  return PyLong_FromLong(lineno);
}

static PyObject* THPPyInterpreterFrame_f_back(THPPyInterpreterFrame* self, PyObject* _noargs) {
  if (!self->frame->frame_obj) {
    Py_RETURN_NONE;
  }
  return (PyObject*)PyFrame_GetBack(self->frame->frame_obj);
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,cppcoreguidelines-avoid-non-const-global-variables,modernize-avoid-c-arrays)
static struct PyGetSetDef THPPyInterpreterFrame_properties[] = {
#if IS_PYTHON_3_12_PLUS
    {"f_func", (getter)THPPyInterpreterFrame_f_funcobj, NULL, NULL, NULL},
#else
    {"f_func", (getter)THPPyInterpreterFrame_f_func, NULL, NULL, NULL},
#endif
    {"f_globals", (getter)THPPyInterpreterFrame_f_globals, NULL, NULL, NULL},
    {"f_builtins", (getter)THPPyInterpreterFrame_f_builtins, NULL, NULL, NULL},
    {"f_locals", (getter)THPPyInterpreterFrame_f_locals, NULL, NULL, NULL},
    {"f_code", (getter)THPPyInterpreterFrame_f_code, NULL, NULL, NULL},
    {"frame_obj", (getter)THPPyInterpreterFrame_frame_obj, NULL, NULL, NULL},
    {"previous", (getter)THPPyInterpreterFrame_previous, NULL, NULL, NULL},
    {"f_lasti", (getter)THPPyInterpreterFrame_f_lasti, NULL, NULL, NULL},
    {"f_lineno", (getter)THPPyInterpreterFrame_f_lineno, NULL, NULL, NULL},
    {"f_back", (getter)THPPyInterpreterFrame_f_back, NULL, NULL, NULL},
    {NULL}};

static PyTypeObject THPPyInterpreterFrameType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "torch._C.dynamo.eval_frame._PyInterpreterFrame",
    .tp_basicsize = sizeof(THPPyInterpreterFrame),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_getset = THPPyInterpreterFrame_properties,
};


THPPyInterpreterFrame* THPPyInterpreterFrame_New(_PyInterpreterFrame* frame) {
  PyTypeObject* type = (PyTypeObject*)&THPPyInterpreterFrameType;
  THPPyInterpreterFrame* self = (THPPyInterpreterFrame*)type->tp_alloc(type, 0);
  if (!self)
    return NULL;
  self->frame = frame;
  return self;
}


#else
#define THP_EVAL_API_FRAME_OBJECT PyFrameObject

#define THP_PyFrame_FastToLocalsWithError PyFrame_FastToLocalsWithError
#endif

#ifdef _WIN32
#define unlikely(x) (x)
#else
#define unlikely(x) __builtin_expect((x), 0)
#endif

#define NULL_CHECK(val)                                         \
  if (unlikely((val) == NULL)) {                                \
    fprintf(stderr, "NULL ERROR: %s:%d\n", __FILE__, __LINE__); \
    PyErr_Print();                                              \
    abort();                                                    \
  } else {                                                      \
  }

#define CHECK(cond)                                                     \
  if (unlikely(!(cond))) {                                              \
    fprintf(stderr, "DEBUG CHECK FAILED: %s:%d\n", __FILE__, __LINE__); \
    abort();                                                            \
  } else {                                                              \
  }

// Uncomment next line to print debug message
// #define TORCHDYNAMO_DEBUG 1

#ifdef TORCHDYNAMO_DEBUG

#define DEBUG_CHECK(cond) CHECK(cond)
#define DEBUG_NULL_CHECK(val) NULL_CHECK(val)
#define DEBUG_TRACE(msg, ...) \
  fprintf(stderr, "TRACE[%s:%d] " msg "\n", __func__, __LINE__, __VA_ARGS__)
#define DEBUG_TRACE0(msg) \
  fprintf(stderr, "TRACE[%s:%d] " msg "\n", __func__, __LINE__)

#else

#define DEBUG_CHECK(cond)
#define DEBUG_NULL_CHECK(val)
#define DEBUG_TRACE(msg, ...)
#define DEBUG_TRACE0(msg)

#endif

// Flag to just run a frame normally
#define SKIP_CODE ((void*)0x1)

static PyObject* guard_error_hook = NULL;
const char* cache_lookup_profiler_str = "TorchDynamo Cache Lookup";

// Points to the extra scratch space on the code object
static Py_ssize_t extra_index = -1;

static Py_tss_t eval_frame_callback_key = Py_tss_NEEDS_INIT;

inline static PyObject* eval_frame_callback_get(void) {
  void* result = PyThread_tss_get(&eval_frame_callback_key);
  if (unlikely(result == NULL)) {
    return (PyObject*)Py_None;
  } else {
    return (PyObject*)result;
  }
}

inline static void eval_frame_callback_set(PyObject* obj) {
  PyThread_tss_set(&eval_frame_callback_key, obj);
}

static PyObject* _custom_eval_frame_shim(
    PyThreadState* tstate,
    THP_EVAL_API_FRAME_OBJECT* frame,
    int throw_flag);
static PyObject* _custom_eval_frame(
    PyThreadState* tstate,
    THP_EVAL_API_FRAME_OBJECT* frame,
    int throw_flag,
    PyObject* callback);
static PyObject *(*previous_eval_frame)(PyThreadState *tstate,
                                        THP_EVAL_API_FRAME_OBJECT* frame, int throw_flag) = NULL;

#if PY_VERSION_HEX >= 0x03090000
static PyObject* custom_eval_frame_shim(
    PyThreadState* tstate,
    THP_EVAL_API_FRAME_OBJECT* frame,
    int throw_flag) {
  return _custom_eval_frame_shim(tstate, frame, throw_flag);
}
#else
static PyObject* custom_eval_frame_shim(THP_EVAL_API_FRAME_OBJECT* frame, int throw_flag) {
  PyThreadState* tstate = PyThreadState_GET();
  return _custom_eval_frame_shim(tstate, frame, throw_flag);
}
#endif

inline static PyObject* eval_frame_default(
    PyThreadState* tstate,
    THP_EVAL_API_FRAME_OBJECT* frame,
    int throw_flag) {
#if PY_VERSION_HEX >= 0x03090000
  if (tstate == NULL) {
    tstate = PyThreadState_GET();
  }
  if (previous_eval_frame) {
    return previous_eval_frame(tstate, frame, throw_flag);
  }
  else {
    return _PyEval_EvalFrameDefault(tstate, frame, throw_flag);
  }
#else
  return _PyEval_EvalFrameDefault(frame, throw_flag);
#endif
}

inline static void enable_eval_frame_shim(PyThreadState* tstate) {
#if PY_VERSION_HEX >= 0x03090000
  if (_PyInterpreterState_GetEvalFrameFunc(tstate->interp) !=
      &custom_eval_frame_shim) {
    DEBUG_CHECK(previous_eval_frame == NULL);
    previous_eval_frame = _PyInterpreterState_GetEvalFrameFunc(tstate->interp);
    _PyInterpreterState_SetEvalFrameFunc(tstate->interp,
                                         &custom_eval_frame_shim);
  }
#else
  if (tstate->interp->eval_frame != &custom_eval_frame_shim) {
    // First call
    tstate->interp->eval_frame = &custom_eval_frame_shim;
  }
#endif
}

inline static void enable_eval_frame_default(PyThreadState* tstate) {
#if PY_VERSION_HEX >= 0x03090000
  if (_PyInterpreterState_GetEvalFrameFunc(tstate->interp) !=
      previous_eval_frame) {
    DEBUG_CHECK(previous_eval_frame != NULL);
    _PyInterpreterState_SetEvalFrameFunc(tstate->interp,
                                         previous_eval_frame);
    previous_eval_frame = NULL;
  }
#else
  if (tstate->interp->eval_frame != &_PyEval_EvalFrameDefault) {
    // First call
    tstate->interp->eval_frame = &_PyEval_EvalFrameDefault;
  }
#endif
}


inline static const char* get_frame_name(THP_EVAL_API_FRAME_OBJECT* frame) {
  // Returns the C string name of the current frame.
  DEBUG_CHECK(PyUnicode_Check(frame->f_code->co_name));
  return PyUnicode_AsUTF8(frame->f_code->co_name);
}

typedef PyObject FrameState;
/*
Our cache resides on the extra scratch space of the code object. The structure
of the cache is as follows:

-> ExtraState
  -> CacheEntry
    -> check_fn
    -> optimized_code
    -> next
  -> FrameState

CacheEntry is a linked list, with each node containing the check_fn for guards
and the optimized code.

The frame_state is a PyDict that enables sharing between different frames. This
is used to detect dynamism in automatic dynamic shapes.

These two are encapsulated into a ExtraState.
*/

// Linked list of cache entries, where each cache entry stores
// the check_fn and the torch.compile optimized python bytecode.
typedef struct cache_entry {
  PyObject_HEAD
  // check the guards: lambda: <locals of user function>: bool
  PyObject* check_fn;
  // modified user bytecode (protected by check_fn's guards)
  PyCodeObject* code;
  // on a cache miss, linked list of next thing to try
  struct cache_entry* next;
} CacheEntry;

static void cache_entry_dealloc(CacheEntry* e);

#define DECLARE_CACHE_ENTRY_ATTR(name) \
static PyObject* CacheEntry_##name(CacheEntry* self, PyObject* _noargs) { \
  PyObject* res = (PyObject*)self->name; \
  Py_INCREF(res); \
  return res; \
}

DECLARE_CACHE_ENTRY_ATTR(check_fn)
DECLARE_CACHE_ENTRY_ATTR(code)
DECLARE_CACHE_ENTRY_ATTR(next)

static struct PyGetSetDef CacheEntry_properties[] = {
    {"check_fn", (getter)CacheEntry_check_fn, NULL, NULL, NULL},
    {"code", (getter)CacheEntry_code, NULL, NULL, NULL},
    {"next", (getter)CacheEntry_next, NULL, NULL, NULL},
    {NULL}};


static PyObject* cache_entry_new(PyTypeObject* type, PyObject* args, PyObject* kwargs) {
  CacheEntry *self = (CacheEntry*) type->tp_alloc(type, 0);
  if (self != NULL) {
    // The corresponding decrefs for Py_None are in cache_entry_init.
    Py_INCREF(Py_None);
    self->check_fn = Py_None;
    Py_INCREF(Py_None);
    self->code = (PyCodeObject*)Py_None;
    Py_INCREF(Py_None);
    self->next = (CacheEntry*)Py_None;
  }
  return (PyObject*)self;
}


static int cache_entry_init(CacheEntry* self, PyObject* args, PyObject* kwds) {
  PyObject* check_fn = NULL;
  PyCodeObject* code = NULL;
  CacheEntry* next = NULL;

  static char *kwlist[] = {"check_fn", "code", "next", NULL};

  int ret = PyArg_ParseTupleAndKeywords(
    args, kwds, "OOO", kwlist,
    &check_fn, &code, &next);

  if (!ret) return -1;

  if (check_fn) {
    PyObject* tmp = self->check_fn;
    Py_INCREF(check_fn);
    self->check_fn = check_fn;
    Py_XDECREF(tmp);
  }

  if (code) {
    PyCodeObject* tmp = self->code;
    Py_INCREF(code);
    self->code = code;
    Py_XDECREF(tmp);
  }

  if (next) {
    CacheEntry* tmp = self->next;
    Py_INCREF(next);
    self->next = next;
    Py_XDECREF(tmp);
  }
  return 0;
}

static PyTypeObject CacheEntryType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "torch._C.dynamo.eval_frame.CacheEntryWrapper",
  .tp_basicsize = sizeof(CacheEntry),
  .tp_itemsize = 0,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_new = cache_entry_new,
  .tp_init = (initproc)cache_entry_init,
  .tp_dealloc = (destructor)cache_entry_dealloc,
  .tp_getset = CacheEntry_properties,
};

// ExtraState encasulates CacheEntry and FrameState. ExtraState is the highest
// level of abstraction of what is stored on the extra code object. Previously,
// we saved different parts on different extra indexes.  We prefer this way
// because of cleaner abstraction and faster SetExtra access.

// TODO(anijain2305) - Consider making this a PyObject. Benefits are
//   1) Modular dealloc - destroy_extra_state just becomes Py_DECREF(extra)
//   2) We can directly send the extra object to convert_frame callback. One
//   data structure - easier to understand code.
// There might be some perf impact of going through a PyObject on the critical
// path, but it should not be too bad.
typedef struct {
  // Cache entry for the code object
  CacheEntry* cache_entry;
  // Frame state to detect dynamic shape dims
  FrameState* frame_state;
} ExtraState;


/* CacheEntry helper functions begins */

static CacheEntry* create_cache_entry(
    CacheEntry* next,
    PyObject* guarded_code) {
  // Ownership contract
  // args
  //   - next: steals
  //   - guarded_code: Borrowed
  //  return
  //   - CacheEntry*: new reference.
  PyObject* check_fn = PyObject_GetAttrString(guarded_code, "check_fn"); // new reference
  PyCodeObject* code = (PyCodeObject*)PyObject_GetAttrString(guarded_code, "code"); // new reference

  // equivalent to CacheEntry(check_fn, code, next) in Python
  PyObject* args = Py_BuildValue("OOO", check_fn, code, next);
  CacheEntry* e = (CacheEntry*)PyObject_CallObject((PyObject*)&CacheEntryType, args); // new reference
  // CacheEntry e is the now the owner of old cachey entry next. This happens
  // when we incref the next pointer in cache_entry_init.
  Py_DECREF(next);
  Py_DECREF(check_fn);
  Py_DECREF(code);
  Py_DECREF(args);
  return e;
}

static void cache_entry_dealloc(CacheEntry* e) {
  Py_XDECREF(e->check_fn);
  Py_XDECREF(e->code);
  // This will recursively call cache_entry_dealloc for the next items in the
  // linked list.
  Py_XDECREF(e->next);
  Py_TYPE(e)->tp_free((PyObject*)e);
}

/* CacheEntry helper functions ends */

/* Extractions helper functions begins. They help with NULL and SKIP_CODE corner cases */

inline static CacheEntry* extract_cache_entry(ExtraState* extra_state) {
  // Helper to extra the cache_entry from the extra state.

  // Ownership contract
  // args
  //  - extra_state: Borrowed
  // return
  //  - CacheEntry: Borrowed.
  if (extra_state == NULL || extra_state == SKIP_CODE) {
    return NULL;
  }
  return extra_state->cache_entry;
}


inline static FrameState* extract_frame_state(ExtraState* extra_state) {
  // Returns either the previously stored frame state or an empty dict.

  // Ownership contract
  // args
  //  - extra_state: Borrowed
  // return
  //  - extra_state->frame_state: Borrowed.
  if (extra_state == NULL || extra_state == SKIP_CODE) {
    return NULL;
  }
  return extra_state->frame_state;
}

/* Extractions helper functions ends */

/* Extra state helper functions begins */

inline static ExtraState* get_extra_state(PyCodeObject* code) {
  // Ownership contract
  // args
  //  - code: Borrowed
  // return
  //  - extra_state: Borrowed.
  ExtraState* extra = NULL;
  _PyCode_GetExtra((PyObject*)code, extra_index, (void*)&extra);
  return extra;
}

inline static void destroy_extra_state(void* obj) {
  // This is passed as freefunc to _PyEval_RequestCodeExtraIndex. This acts as a
  // deleter for the object on extra scratch space. This function is called
  // internally in _PyCode_SetExtra and also during the code deallocation.

  // Destroys the extra state by deleting cache_entry, frame state and finally
  // freeing the constructed extra state.

  // Developer note - You should not call this function directly. This is called
  // directly inside set_extra_state. If you are in a situation trying to call
  // this function, consider if set_extra_state should be called.

  ExtraState* extra = (ExtraState*)obj;
  if (extra != NULL && extra != SKIP_CODE) {
    // Cpython gc will call cache_entry_dealloc on its own when the ref count
    // goes to 0.
    Py_XDECREF(extra->cache_entry);
    Py_XDECREF(extra->frame_state);
    free(extra);
  }
}

inline static void set_extra_state(PyCodeObject* code, ExtraState* extra_state) {
  // Clears the existing object sitting on the extra scratch spance and sets it
  // up with the new state. Note that _PyCode_SetExtra calls the
  // destroy_extra_state deleter internally, and therefore we don't call it
  // explicity here.

  // Ownership contract
  // args
  //  - extra_state: Stolen
  // return
  //  - there is no return, but the extra_state is stolen, so it becomes
  //  set_extra_state responsibility to clean it up. It will be deleted during
  //  the reset_code/skip, when the set_extra_state is called with
  //  NULL/SKIP_CODE.

  // Invariant - Dont set the extra state for the extra state that is already on
  // the code object. Otherwise, we will first free up the old extra state
  // (which is also the new extra state) and write something invalid on the
  // scratch space.
  ExtraState* old_extra_state = get_extra_state(code);
  CHECK(old_extra_state == NULL || old_extra_state == SKIP_CODE || old_extra_state != extra_state);
  _PyCode_SetExtra((PyObject*)code, extra_index, extra_state);
}

inline static ExtraState* init_and_set_extra_state(PyCodeObject* code) {
  // Creates a new extra state and put it on the extra scrach space of the code
  // object.

  // Ownership contract
  // args
  //  - code: Borrowed
  // return:
  //   - extra_state: New reference.
  // These references are then further passed to set_extra_state which becomes
  // the final owner of these references.

  // Invariant - Extra state should not have been set before, therefore it should be NULL.
  CHECK(get_extra_state(code) == NULL);
  ExtraState* extra_state = (ExtraState*)malloc(sizeof(ExtraState));
  DEBUG_NULL_CHECK(extra_state);
  // We set the last node in the linked list to Py_None. We incref the Py_None
  // here, the corresponding decref is in cache_entry_dealloc.
  Py_INCREF(Py_None);
  extra_state->cache_entry = (CacheEntry*)Py_None;
  extra_state->frame_state = PyDict_New();
  set_extra_state(code, extra_state);
  return extra_state;
}

/* Extra state helper functions ends */

/*
Debugger helper functions.
*/

PyObject* _debug_get_cache_entry_list(PyObject* self, PyObject* args) {
  // get the cache entry out of a code object
  PyObject* object = NULL;
  if (!PyArg_ParseTuple(args, "O", &object)) {
    return NULL;
  }
  if (!PyCode_Check(object)) {
    PyErr_SetString(PyExc_TypeError, "expected a code object!");
    return NULL;
  }
  PyCodeObject* code = (PyCodeObject*)object;

  ExtraState* extra = get_extra_state(code);
  CacheEntry* current_node = extract_cache_entry(extra);
  if (current_node == NULL)
  {
    Py_RETURN_NONE;
  }
  Py_INCREF(current_node);
  return (PyObject*)current_node;
}

static inline PyObject* call_callback(
    PyObject* callable,
    THP_EVAL_API_FRAME_OBJECT* _frame,
    CacheEntry* cache_entry,
    FrameState* frame_state) {

// remember to update the type signature for DynamoCallbackFn.__call__ in torch/_dynamo/types.py
// if this function changes
#if IS_PYTHON_3_11_PLUS
  THPPyInterpreterFrame* frame = THPPyInterpreterFrame_New(_frame);
  if (frame == NULL) {
    return NULL;
  }
#else
  PyObject* frame = Py_NewRef(_frame);
#endif

  PyObject* res = PyObject_CallFunction(
    callable,
    "OOO",
    frame,
    cache_entry,
    frame_state);
  Py_DECREF(frame);
  return res;
}

static PyObject* call_guard_fail_hook(
    PyObject* hook,
    CacheEntry* e,
    size_t index,
    PyObject* f_locals) {
  // call debugging logic when a guard fails
  return PyObject_CallFunction(
      hook,
      "OOOnO",
      e->check_fn,
      e->code,
      f_locals,
      (Py_ssize_t)index,
      (e->next == (CacheEntry*)Py_None ? Py_True : Py_False));
}

// Return value: borrowed reference
// Is either Py_None or a PyCodeObject
static PyObject* lookup(CacheEntry* e, THP_EVAL_API_FRAME_OBJECT *frame, CacheEntry* prev, size_t index) {
  if (e == (CacheEntry*)Py_None) {
    // NB: intentionally not using Py_RETURN_NONE, to return borrowed ref
    return Py_None;
  }
  PyObject *f_locals = frame->f_locals;
  // remember to update the type signature for GuardFn.__call__ in torch/_dynamo/types.py
  // if this calling convention changes
  PyObject* valid = PyObject_CallOneArg(e->check_fn, f_locals);
  if (unlikely(valid == NULL)) {
    if (guard_error_hook != NULL) {
      PyObject *type = NULL, *value = NULL, *traceback = NULL;
      PyErr_Fetch(&type, &value, &traceback);
      PyObject* r = call_guard_fail_hook(guard_error_hook, e, index, f_locals);
      if (r == NULL) {
        return NULL;
      }
      Py_DECREF(r);
      PyErr_Restore(type, value, traceback);
    }
    return NULL;
  }
  Py_DECREF(valid);
  if (valid == Py_True) {
    // Keep the head as the most recently used cache entry.
    // If the hit cache entry is not the head of the linked list,
    // move it to the head
    if (prev != NULL) {
        ExtraState* extra = get_extra_state(frame->f_code);
        // Override the extra state to reflect the updated cache line.
        CacheEntry* old_cache_entry = extra->cache_entry;
        prev->next = e->next;
        e->next = old_cache_entry;
        extra->cache_entry = e;
    }
    return (PyObject*)e->code;
  }
  return lookup(e->next, frame, e, index + 1);
}

inline static PyObject* eval_custom_code_impl(
    PyThreadState* tstate,
    THP_EVAL_API_FRAME_OBJECT* frame,
    PyCodeObject* code,
    int throw_flag) {

  DEBUG_NULL_CHECK(tstate);
  DEBUG_NULL_CHECK(frame);
  DEBUG_NULL_CHECK(code);

  #if IS_PYTHON_3_11_PLUS

  // Generate Python function object and _PyInterpreterFrame in a way similar to
  // https://github.com/python/cpython/blob/e715da6db1d1d70cd779dc48e1ba8110c51cc1bf/Python/ceval.c#L1130
  #if IS_PYTHON_3_12_PLUS
  // Most of these don't exist in 3.12 anymore.
  // _PyFunction_CopyWithNewCode and _PyFrame_InitializeSpecials in particular
  PyFunctionObject* func;
  PyErr_SetString(PyExc_RuntimeError, "Dynamo is not supported in Python 3.12 yet");
  return NULL;
  #else
  PyFunctionObject* func = _PyFunction_CopyWithNewCode((PyFunctionObject*) frame->f_func, code);
  if (func == NULL) {
    return NULL;
  }
  #endif

  size_t size = code->co_nlocalsplus + code->co_stacksize + FRAME_SPECIALS_SIZE;
  // THP_EVAL_API_FRAME_OBJECT (_PyInterpreterFrame) is a regular C struct, so
  // it should be safe to use system malloc over Python malloc, e.g. PyMem_Malloc
  THP_EVAL_API_FRAME_OBJECT* shadow = malloc(size * sizeof(PyObject*));
  if (shadow == NULL) {
    Py_DECREF(func);
    return NULL;
  }

  Py_INCREF(func);
  // consumes reference to func
  #if !(IS_PYTHON_3_12_PLUS)
  _PyFrame_InitializeSpecials(shadow, func, NULL, code->co_nlocalsplus);
  #endif

  PyObject** fastlocals_old = frame->localsplus;
  PyObject** fastlocals_new = shadow->localsplus;
  Py_ssize_t n_old = frame->f_code->co_nlocalsplus;
  Py_ssize_t n_new = code->co_nlocalsplus;

  // localsplus are XINCREF'd by default eval frame, so all values must be valid.
  for (int i = 0; i < code->co_nlocalsplus; i++) {
    fastlocals_new[i] = NULL;
  }

  #else

  THP_EVAL_API_FRAME_OBJECT* shadow = PyFrame_New(tstate, code, frame->f_globals, NULL);
  if (shadow == NULL) {
    return NULL;
  }

  PyObject** fastlocals_old = frame->f_localsplus;
  PyObject** fastlocals_new = shadow->f_localsplus;
  Py_ssize_t n_old = frame->f_code->co_nlocals + PyCode_GetNFreevars(frame->f_code) + PyCode_GetNCellvars(frame->f_code);
  Py_ssize_t n_new = code->co_nlocals + PyCode_GetNFreevars(code) + PyCode_GetNCellvars(code);

  #endif

  // ============== Initialize new frame from old frame ============
  // Python internal for executing a function:
  //  1. CPython interpreter first creates an empty frame according to the code object
  //  2. CPython interpreter initializes the frame by filling arguments/free variables into frame and initializing cell variables
  //  3. CPython interpreter executes the code object
  //
  // Dynamo hooks the 3th step: before executing the code object, Dynamo transforms the code object into a new code object. Then, the old frame is not suitable for executing the new code. Therefore, Dynamo needs to manually create and initialize a new frame to execute the new code.
  // The main task is to copy data in old frame to new frame, concerning a storage space named `localsplus`.
  //
  // localsplus storage is an array with the following layout:
  // |   args   |   new_locals    |    cell_variables |   free_variables    |
  // | <--- from left to right, index from 0 to n - 1 ---> |
  // code.co_varnames == args + new_locals, code.co_nlocals == len(code.co_varnames)
  // code.co_freevars == free_variables
  // In Python 3.10 and lower, `n == code.co_nlocals + len(code.co_cellvars) + len(code.co_freevars)` (Python expression)
  // In Python 3.11 and higher, `n <= code.co_nlocals + len(code.co_cellvars) + len(code.co_freevars)` (Python expression). There is an extra field in Python C-API: `n == code->co_nlocalsplus` (C expression) to retrieve the length of array.
  // The complexity happens if an argument becomes a cell variable:
  //  In Python 3.10 and lower, `code.co_cellvars == cell_variables`, and the corresponding slot in args becomes `NULL`.
  //  In Python 3.11 and higher, `code.co_cellvars > cell_variables`, that cell variable is still stored in args, with a flag set in corresponding item's `co_localspluskinds` .
  //
  // ideally, we need to look up new localsplus from old localsplus by name:
  // for i, name, value in enumerate(localsplusnames_old):
  //   if value != NULL: (NULL happens for new local variables and arguments that becomes cell variables)
  //     name_to_idx[name] = i
  // for i, name in enumerate(localsplusnames_new):
  //  if name in name_to_idx:
  //    fastlocals_new[i] = fastlocals_old[name_to_idx[name]]
  //
  // The above process of building a `name_to_idx` mapping is expensive.
  // Dynamo makes the following assumptions:
  //  1. new code has the same arguments as the old code (both the number and the order)
  //  2. new code has the same cell variables as the old code (both the number and the order)
  //  3. new code has the same free variables as the old code (both the number and the order)
  //  The only flexibility lies in new local variables: new code can introduce their own variables.
  // With these assumptions, Dynamo can copy data directly by index. Dynamo just needs to take care of copying cell variables correctly.
  // To avoid runtime cost, the assumptions are checked when we first generate the code object in pytorch/torch/_dynamo/convert_frame.py .


  // copy args
  // according to https://docs.python.org/3/library/inspect.html , `co_argcount` is the number of arguments (not including keyword only arguments, * or ** args). so we need to add `co_kwonlyargcount` and `co_flags` to get the total number of arguments.
  // !!(frame->f_code->co_flags & CO_VARARGS) is 1 if the function has *args, 0 otherwise
  // !!(frame->f_code->co_flags & CO_VARKEYWORDS) is 1 if the function has **kwargs, 0 otherwise
  // they convert bit flags to 0 or 1, and avoid branching.
  // This is performance critical code, so we really care about performance.
  Py_ssize_t total_argcount_old = frame->f_code->co_argcount + frame->f_code->co_kwonlyargcount + !!(frame->f_code->co_flags & CO_VARARGS) + !!(frame->f_code->co_flags & CO_VARKEYWORDS);

  for (Py_ssize_t i = 0; i < total_argcount_old; i++) {
    Py_XINCREF(fastlocals_old[i]);
    fastlocals_new[i] = fastlocals_old[i];
  }

  // copy free vars
  Py_ssize_t nfrees_old = PyCode_GetNFreevars(frame->f_code);

  for (Py_ssize_t i = 0; i < nfrees_old; i++) {
    Py_XINCREF(fastlocals_old[n_old - 1 - i]);
    fastlocals_new[n_new - 1 - i] = fastlocals_old[n_old - 1 - i];
  }

  // copy cell vars, from high index to low index, until it meets a variable that is not cell variable.
  for (Py_ssize_t i = n_old - nfrees_old - 1, j = n_new - nfrees_old - 1; i >= total_argcount_old; i--, j--) {

  // conditional test to tell if a variable is not a cell variable
  // this is straightforward in Python 3.11 and higher, as there are bit flags in `co_localspluskinds` to tell if a variable is a cell variable.
  // in Python 3.10 and lower, essentially we are checking if a variable is a new local variable (because of the layout mentioned above, the first variable that is not cell variable is the first new local variable). the corresponding slot in `flocalsplus` is NULL for new local variables.
  #if IS_PYTHON_3_11_PLUS
    if(!(_PyLocals_GetKind(frame->f_code->co_localspluskinds, i) & CO_FAST_CELL))
    {
      break;
    }
  #else
    if(fastlocals_old[i] == NULL)
    {
      break;
    }
  #endif

    Py_XINCREF(fastlocals_old[i]);
    fastlocals_new[j] = fastlocals_old[i];
  }

  PyObject* result = eval_frame_default(tstate, shadow, throw_flag);

  #if IS_PYTHON_3_11_PLUS

  THP_PyFrame_Clear(shadow);
  free(shadow);
  Py_DECREF(func);

  #else

  Py_DECREF(shadow);

  #endif

  return result;
}

// This wrapper function adds a profiler event
inline static PyObject* eval_custom_code(
    PyThreadState* tstate,
    THP_EVAL_API_FRAME_OBJECT* frame,
    PyCodeObject* code,
    int throw_flag) {
  _PytorchRecordFunctionState* rf = _pytorch_record_function_enter("Torch-Compiled Region");
  PyObject* result = eval_custom_code_impl(
    tstate,
    frame,
    code,
    throw_flag
  );
  _pytorch_record_function_exit(rf);
  return result;
}

static PyObject* _custom_eval_frame_shim(
    PyThreadState* tstate,
    THP_EVAL_API_FRAME_OBJECT* frame,
    int throw_flag) {
  // Shims logic into one of three states. Can probably be refactored into a
  // single func, later:
  //  - None: disables TorchDynamo
  //  - False: run-only mode (reuse existing compiles)
  //  - Python callable(): enables TorchDynamo
  PyObject* callback = eval_frame_callback_get();

  if (callback == Py_None) {
    return eval_frame_default(tstate, frame, throw_flag);
  }

  return _custom_eval_frame(tstate, frame, throw_flag, callback);
}

static PyObject* _custom_eval_frame(
    PyThreadState* tstate,
    THP_EVAL_API_FRAME_OBJECT* frame,
    int throw_flag,
    PyObject* callback) {
  #if IS_PYTHON_3_11_PLUS
  DEBUG_TRACE(
      "begin %s %s %i %i",
      get_frame_name(frame),
      PyUnicode_AsUTF8(frame->f_code->co_filename),
      frame->f_code->co_firstlineno,
      _PyInterpreterFrame_LASTI(frame));
  #else
  DEBUG_TRACE(
      "begin %s %s %i %i %i",
      get_frame_name(frame),
      PyUnicode_AsUTF8(frame->f_code->co_filename),
      frame->f_lineno,
      frame->f_lasti,
      frame->f_iblock);
  #endif

  if (throw_flag) {
    // When unwinding generators, eval frame is called with throw_flag ==
    // true.  Frame evaluation is supposed to continue unwinding by propagating
    // the exception.  Dynamo doesn't really know how to do this, nor does it
    // really want to do this, because there's unlikely any code to capture
    // (you're going to immediately quit out of the frame, perhaps running
    // some unwinding logic along the way).  So we just run the default
    // handler in this case.
    //
    // NB: A previous version of this patch returned NULL.  This is wrong,
    // because returning NULL is *different* from unwinding an exception.
    // In particular, you will not execute things like context manager
    // __exit__ if you just return NULL.
    //
    // NB: It's /conceivable/ that you might want to actually still call the
    // Dynamo callback when throw_flag == TRUE, to give Dynamo a chance to
    // do any stack unwinding code.  But this is not really useful because
    // (1) Dynamo doesn't actually know how to do stack unwinding, so it would
    // immediately skip the frame, and (2) even if it did, this would only
    // be profitable if there was tensor code in the unwinding code.  Seems
    // unlikely.
    DEBUG_TRACE("throw %s", get_frame_name(frame));
    return eval_frame_default(tstate, frame, throw_flag);
  }

  ExtraState* extra = get_extra_state(frame->f_code);
  if (extra == SKIP_CODE || (callback == Py_False && extra == NULL)) {
    DEBUG_TRACE("skip %s", get_frame_name(frame));
    return eval_frame_default(tstate, frame, throw_flag);
  }

  if (extra == NULL) {
    extra = init_and_set_extra_state(frame->f_code);
  }

  CacheEntry* cache_entry = extract_cache_entry(extra);
  FrameState* frame_state = extract_frame_state(extra);

  // TODO(jansel): investigate directly using the "fast" representation
  // TODO(alband): This is WRONG for python3.11+ we pass in a _PyInterpreterFrame
  // even though we should pass a PyFrameObject.
  if (THP_PyFrame_FastToLocalsWithError(frame) < 0) {
    DEBUG_TRACE("error %s", get_frame_name(frame));
    return NULL;
  }

  // A callback of Py_False indicates "run only" mode, the cache is checked, but
  // we never compile.
  if (callback == Py_False) {
    DEBUG_TRACE("In run only mode %s", get_frame_name(frame));
    _PytorchRecordFunctionState* rf = _pytorch_record_function_enter(cache_lookup_profiler_str);
    PyObject* maybe_cached_code = lookup(cache_entry, frame, NULL, 0);
    _pytorch_record_function_exit(rf);

    if (maybe_cached_code == NULL) {
      // guard eval failed, keep propagating
      return NULL;
    } else if (maybe_cached_code == Py_None) {
      DEBUG_TRACE("cache miss %s", get_frame_name(frame));
      return eval_frame_default(tstate, frame, throw_flag);
    }
    PyCodeObject* cached_code = (PyCodeObject*)maybe_cached_code;
    // used cached version
    DEBUG_TRACE("cache hit %s", get_frame_name(frame));
    return eval_custom_code(tstate, frame, cached_code, throw_flag);
  }
  DEBUG_CHECK(PyDict_CheckExact(frame->f_locals));
  DEBUG_CHECK(PyDict_CheckExact(frame->f_globals));
  DEBUG_CHECK(PyDict_CheckExact(frame->f_builtins));

  // We don't run the current custom_eval_frame behavior for guards.
  // So we temporarily set the callback to Py_None to drive the correct behavior
  // in the shim.
  eval_frame_callback_set(Py_None);

  _PytorchRecordFunctionState* rf = _pytorch_record_function_enter(cache_lookup_profiler_str);
  PyObject* maybe_cached_code = lookup(cache_entry, frame, NULL, 0);
  _pytorch_record_function_exit(rf);
  if (maybe_cached_code == NULL) {
    // Python error
    return NULL;
  } else if (maybe_cached_code != Py_None) {
    PyCodeObject* cached_code = (PyCodeObject*)maybe_cached_code;
    // used cached version
    DEBUG_TRACE("cache hit %s", get_frame_name(frame));
    // Re-enable custom behavior
    eval_frame_callback_set(callback);
    return eval_custom_code(tstate, frame, cached_code, throw_flag);
  }
  // cache miss
  // TODO(alband): This is WRONG for python3.11+ we pass in a _PyInterpreterFrame
  // that gets re-interpreted as a PyObject (which it is NOT!)
  PyObject* result =
      call_callback(callback, frame, cache_entry, frame_state);
  if (result == NULL) {
    // internal exception, returning here will leak the exception into user code
    // this is useful for debugging -- but we dont want it to happen outside of
    // testing
    // NB: we intentionally DO NOT re-enable custom behavior to prevent
    // cascading failure from internal exceptions.  The upshot is if
    // Dynamo barfs, that's it for Dynamo, even if you catch the exception
    // inside the torch.compile block we won't try to Dynamo anything else.
    return NULL;
  } else if (result != Py_None) {
    DEBUG_TRACE("create cache %s", get_frame_name(frame));

    // NB: We could use extract_cache_entry to get the cache_entry, but
    // extract_cache_entry returns a borrowed reference. Modifying a borrowed
    // reference seems wrong. Therefore, we directly access the
    // extra->cache_entry. extra wont be NULL here.
    extra->cache_entry = create_cache_entry(extra->cache_entry, result);
    Py_DECREF(result);
    // Update the existing cache_entry on the extra object. This extra object is
    // sitting on the extra scratch space, we are just changing the cache_entry
    // ptr. As a result, extra now becomes the owner of CacheEntry object. This
    // will be cleaned up when set_extra_state is called.
    // Re-enable custom behavior
    eval_frame_callback_set(callback);
    return eval_custom_code(tstate, frame, extra->cache_entry->code, throw_flag);
  } else {
    DEBUG_TRACE("create skip %s", get_frame_name(frame));
    Py_DECREF(result);
    set_extra_state(frame->f_code, SKIP_CODE);
    // Re-enable custom behavior
    eval_frame_callback_set(callback);
    return eval_frame_default(tstate, frame, throw_flag);
  }
}

static int active_dynamo_threads = 0;

static PyObject* increment_working_threads(PyThreadState* tstate) {
  active_dynamo_threads = active_dynamo_threads + 1;
  if (active_dynamo_threads > 0) {
    enable_eval_frame_shim(tstate);
  }
  Py_RETURN_NONE;
}

static PyObject* decrement_working_threads(PyThreadState* tstate) {
  if (active_dynamo_threads > 0) {
    active_dynamo_threads = active_dynamo_threads - 1;
    if (active_dynamo_threads == 0) {
      enable_eval_frame_default(tstate);
    }
  }
  Py_RETURN_NONE;
}

static PyObject* set_eval_frame(PyObject* new_callback, PyThreadState* tstate) {
  // Change the eval frame callback and return the old one
  //  - None: disables TorchDynamo
  //  - False: run-only mode (reuse existing compiles)
  //  - Python callable(): enables TorchDynamo
  PyObject* old_callback = eval_frame_callback_get();

  // owned by caller
  Py_INCREF(old_callback);

  if (old_callback != Py_None && new_callback == Py_None) {
    decrement_working_threads(tstate);
  } else if (old_callback == Py_None && new_callback != Py_None) {
    increment_working_threads(tstate);
  }

  Py_INCREF(new_callback);
  Py_DECREF(old_callback);

  // Set thread local callback. This will drive behavior of our shim, if/when it
  // is installed.
  eval_frame_callback_set(new_callback);

  return old_callback;
}

static PyObject* set_eval_frame_py(PyObject* dummy, PyObject* callback) {
  if (callback != Py_None && callback != Py_False &&
      !PyCallable_Check(callback)) {
    DEBUG_TRACE0("arg error");
    PyErr_SetString(PyExc_TypeError, "expected a callable");
    return NULL;
  }
  DEBUG_TRACE(
      "python enabled=%d and is run_only=%d",
      callback != Py_None,
      callback == Py_False);
  return set_eval_frame(callback, PyThreadState_GET());
}

static PyObject* reset_code(PyObject* dummy, PyObject* code) {
  if (!PyCode_Check(code)) {
    DEBUG_TRACE0("arg error");
    PyErr_SetString(PyExc_TypeError, "expected a code object");
    return NULL;
  }

  // set_extra_state destroys the existing object on extra scratch space.
  set_extra_state((PyCodeObject*)code, NULL);
  Py_RETURN_NONE;
}

static PyObject* unsupported(PyObject* dummy, PyObject* args) {
  // a dummy C function used in testing
  PyObject* obj1 = NULL;
  PyObject* obj2 = NULL;
  if (!PyArg_ParseTuple(args, "OO", &obj1, &obj2)) {
    return NULL;
  }
  Py_INCREF(obj2);
  return obj2;
}

static PyObject* skip_code(PyObject* dummy, PyObject* obj) {
  if (!PyCode_Check(obj)) {
    PyErr_SetString(PyExc_TypeError, "expected a code object");
    return NULL;
  }

  // set_extra_state destroys the existing object on extra scratch space.
  set_extra_state((PyCodeObject*)obj, SKIP_CODE);
  Py_RETURN_NONE;
}

static PyObject* set_guard_error_hook(PyObject* dummy, PyObject* obj) {
  if (obj == Py_None) {
    obj = NULL;
  }
  Py_XSETREF(guard_error_hook, Py_XNewRef(obj));
  Py_RETURN_NONE;
}

static PyMethodDef _methods[] = {
    {"set_eval_frame", set_eval_frame_py, METH_O, NULL},
    {"reset_code", reset_code, METH_O, NULL},
    {"unsupported", unsupported, METH_VARARGS, NULL},
    {"skip_code", skip_code, METH_O, NULL},
    {"set_guard_error_hook", set_guard_error_hook, METH_O, NULL},
    {"_debug_get_cache_entry_list", _debug_get_cache_entry_list, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}};

static struct PyModuleDef _module = {
    PyModuleDef_HEAD_INIT,
    "torch._C._dynamo.eval_frame",
    "Module containing hooks to override eval_frame",
    -1,
    _methods};


PyObject* torch_c_dynamo_eval_frame_init(void) {
  extra_index = _PyEval_RequestCodeExtraIndex(destroy_extra_state);
  if (extra_index < 0) {
    PyErr_SetString(PyExc_RuntimeError,
                    "dynamo: unable to register extra index");
    return NULL;
  }

  int result = PyThread_tss_create(&eval_frame_callback_key);
  CHECK(result == 0);

  Py_INCREF(Py_None);
  eval_frame_callback_set(Py_None);

  PyObject* module = PyModule_Create(&_module);
  if (module == NULL) {
    return NULL;
  }

#if IS_PYTHON_3_11_PLUS
  if (PyType_Ready(&THPPyInterpreterFrameType) < 0) {
    return NULL;
  }
  Py_INCREF(&THPPyInterpreterFrameType);
  if (PyModule_AddObject(module, "_PyInterpreterFrame", (PyObject*)&THPPyInterpreterFrameType) != 0) {
    return NULL;
  }
#endif


  if (PyType_Ready(&CacheEntryType) < 0) {
    return NULL;
  }
  Py_INCREF(&CacheEntryType);
  if (PyModule_AddObject(module, "_CacheEntry", (PyObject *) &CacheEntryType) < 0) {
      Py_DECREF(&CacheEntryType);
      return NULL;
  }

  return module;
}
