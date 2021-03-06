
/* An FFI object has methods like ffi.new().  It is also a container
   for the type declarations (typedefs and structs) that you can use,
   say in ffi.new().

   CTypeDescrObjects are internally stored in the dict 'types_dict'.
   The types_dict is lazily filled with CTypeDescrObjects made from
   reading a _cffi_type_context_s structure.

   In "modern" mode, the FFI instance is made by the C extension
   module originally created by recompile().  The _cffi_type_context_s
   structure comes from global data in the C extension module.

   In "compatibility" mode, an FFI instance is created explicitly by
   the user, and its _cffi_type_context_s is initially empty.  You
   need to call ffi.cdef() to add more information to it.
*/

#define FFI_COMPLEXITY_OUTPUT   1200     /* xxx should grow as needed */

#define FFIObject_Check(op) PyObject_TypeCheck(op, &FFI_Type)
#define LibObject_Check(ob)  ((Py_TYPE(ob) == &Lib_Type))

struct FFIObject_s {
    PyObject_HEAD
    PyObject *gc_wrefs, *gc_wrefs_freelist;
    struct _cffi_parse_info_s info;
    char ctx_is_static, ctx_is_nonempty;
    builder_c_t types_builder;
};

static FFIObject *ffi_internal_new(PyTypeObject *ffitype,
                                 const struct _cffi_type_context_s *static_ctx)
{
    static _cffi_opcode_t internal_output[FFI_COMPLEXITY_OUTPUT];

    FFIObject *ffi;
    if (static_ctx != NULL) {
        ffi = (FFIObject *)PyObject_GC_New(FFIObject, ffitype);
        /* we don't call PyObject_GC_Track() here: from _cffi_init_module()
           it is not needed, because in this case the ffi object is immortal */
    }
    else {
        ffi = (FFIObject *)ffitype->tp_alloc(ffitype, 0);
    }
    if (ffi == NULL)
        return NULL;

    if (init_builder_c(&ffi->types_builder, static_ctx) < 0) {
        Py_DECREF(ffi);
        return NULL;
    }
    ffi->gc_wrefs = NULL;
    ffi->gc_wrefs_freelist = NULL;
    ffi->info.ctx = &ffi->types_builder.ctx;
    ffi->info.output = internal_output;
    ffi->info.output_size = FFI_COMPLEXITY_OUTPUT;
    ffi->ctx_is_static = (static_ctx != NULL);
    ffi->ctx_is_nonempty = (static_ctx != NULL);
    return ffi;
}

static void ffi_dealloc(FFIObject *ffi)
{
    PyObject_GC_UnTrack(ffi);
    Py_XDECREF(ffi->gc_wrefs);
    Py_XDECREF(ffi->gc_wrefs_freelist);

    free_builder_c(&ffi->types_builder, ffi->ctx_is_static);

    Py_TYPE(ffi)->tp_free((PyObject *)ffi);
}

static int ffi_traverse(FFIObject *ffi, visitproc visit, void *arg)
{
    Py_VISIT(ffi->types_builder.types_dict);
    Py_VISIT(ffi->types_builder.included_ffis);
    Py_VISIT(ffi->types_builder.included_libs);
    Py_VISIT(ffi->gc_wrefs);
    return 0;
}

static PyObject *ffiobj_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    /* user-facing initialization code, for explicit FFI() calls */
    return (PyObject *)ffi_internal_new(type, NULL);
}

/* forward, declared in cdlopen.c because it's mostly useful for this case */
static int ffiobj_init(PyObject *self, PyObject *args, PyObject *kwds);

static PyObject *ffi_fetch_int_constant(FFIObject *ffi, char *name,
                                        int recursion)
{
    int index;

    index = search_in_globals(&ffi->types_builder.ctx, name, strlen(name));
    if (index >= 0) {
        const struct _cffi_global_s *g;
        g = &ffi->types_builder.ctx.globals[index];

        switch (_CFFI_GETOP(g->type_op)) {
        case _CFFI_OP_CONSTANT_INT:
        case _CFFI_OP_ENUM:
            return realize_global_int(&ffi->types_builder, index);

        default:
            PyErr_Format(FFIError,
                         "function, global variable or non-integer constant "
                         "'%.200s' must be fetched from its original 'lib' "
                         "object", name);
            return NULL;
        }
    }

    if (ffi->types_builder.included_ffis != NULL) {
        Py_ssize_t i;
        PyObject *included_ffis = ffi->types_builder.included_ffis;

        if (recursion > 100) {
            PyErr_SetString(PyExc_RuntimeError,
                            "recursion overflow in ffi.include() delegations");
            return NULL;
        }

        for (i = 0; i < PyTuple_GET_SIZE(included_ffis); i++) {
            FFIObject *ffi1;
            PyObject *x;

            ffi1 = (FFIObject *)PyTuple_GET_ITEM(included_ffis, i);
            x = ffi_fetch_int_constant(ffi1, name, recursion + 1);
            if (x != NULL || PyErr_Occurred())
                return x;
        }
    }
    return NULL;     /* no exception set, means "not found" */
}

#define ACCEPT_STRING   1
#define ACCEPT_CTYPE    2
#define ACCEPT_CDATA    4
#define ACCEPT_ALL      (ACCEPT_STRING | ACCEPT_CTYPE | ACCEPT_CDATA)
#define CONSIDER_FN_AS_FNPTR  8

static CTypeDescrObject *_ffi_bad_type(FFIObject *ffi, char *input_text)
{
    size_t length = strlen(input_text);
    char *extra;

    if (length > 500) {
        extra = "";
    }
    else {
        char *p;
        size_t i, num_spaces = ffi->info.error_location;
        extra = alloca(length + num_spaces + 4);
        p = extra;
        *p++ = '\n';
        for (i = 0; i < length; i++) {
            if (' ' <= input_text[i] && input_text[i] < 0x7f)
                *p++ = input_text[i];
            else if (input_text[i] == '\t' || input_text[i] == '\n')
                *p++ = ' ';
            else
                *p++ = '?';
        }
        *p++ = '\n';
        memset(p, ' ', num_spaces);
        p += num_spaces;
        *p++ = '^';
        *p++ = 0;
    }
    PyErr_Format(FFIError, "%s%s", ffi->info.error_message, extra);
    return NULL;
}

static CTypeDescrObject *_ffi_type(FFIObject *ffi, PyObject *arg,
                                   int accept)
{
    /* Returns the CTypeDescrObject from the user-supplied 'arg'.
       Does not return a new reference!
    */
    if ((accept & ACCEPT_STRING) && PyText_Check(arg)) {
        PyObject *types_dict = ffi->types_builder.types_dict;
        PyObject *x = PyDict_GetItem(types_dict, arg);

        if (x == NULL) {
            char *input_text = PyText_AS_UTF8(arg);
            int err, index = parse_c_type(&ffi->info, input_text);
            if (index < 0)
                return _ffi_bad_type(ffi, input_text);

            x = realize_c_type_or_func(&ffi->types_builder,
                                       ffi->info.output, index);
            if (x == NULL)
                return NULL;

            /* Cache under the name given by 'arg', in addition to the
               fact that the same ct is probably already cached under
               its standardized name.  In a few cases, it is not, e.g.
               if it is a primitive; for the purpose of this function,
               the important point is the following line, which makes
               sure that in any case the next _ffi_type() with the same
               'arg' will succeed early, in PyDict_GetItem() above.
            */
            err = PyDict_SetItem(types_dict, arg, x);
            Py_DECREF(x); /* we know it was written in types_dict (unless out
                             of mem), so there is at least that ref left */
            if (err < 0)
                return NULL;
        }

        if (CTypeDescr_Check(x))
            return (CTypeDescrObject *)x;
        else if (accept & CONSIDER_FN_AS_FNPTR)
            return unwrap_fn_as_fnptr(x);
        else
            return unexpected_fn_type(x);
    }
    else if ((accept & ACCEPT_CTYPE) && CTypeDescr_Check(arg)) {
        return (CTypeDescrObject *)arg;
    }
    else if ((accept & ACCEPT_CDATA) && CData_Check(arg)) {
        return ((CDataObject *)arg)->c_type;
    }
#if PY_MAJOR_VERSION < 3
    else if (PyUnicode_Check(arg)) {
        CTypeDescrObject *result;
        arg = PyUnicode_AsASCIIString(arg);
        if (arg == NULL)
            return NULL;
        result = _ffi_type(ffi, arg, accept);
        Py_DECREF(arg);
        return result;
    }
#endif
    else {
        const char *m1 = (accept & ACCEPT_STRING) ? "string" : "";
        const char *m2 = (accept & ACCEPT_CTYPE) ? "ctype object" : "";
        const char *m3 = (accept & ACCEPT_CDATA) ? "cdata object" : "";
        const char *s12 = (*m1 && (*m2 || *m3)) ? " or " : "";
        const char *s23 = (*m2 && *m3) ? " or " : "";
        PyErr_Format(PyExc_TypeError, "expected a %s%s%s%s%s, got '%.200s'",
                     m1, s12, m2, s23, m3,
                     Py_TYPE(arg)->tp_name);
        return NULL;
    }
}

PyDoc_STRVAR(ffi_sizeof_doc,
"Return the size in bytes of the argument.\n"
"It can be a string naming a C type, or a 'cdata' instance.");

static PyObject *ffi_sizeof(FFIObject *self, PyObject *arg)
{
    Py_ssize_t size;
    CTypeDescrObject *ct = _ffi_type(self, arg, ACCEPT_ALL);
    if (ct == NULL)
        return NULL;

    size = ct->ct_size;

    if (CData_Check(arg)) {
        CDataObject *cd = (CDataObject *)arg;
        if (cd->c_type->ct_flags & CT_ARRAY)
            size = get_array_length(cd) * cd->c_type->ct_itemdescr->ct_size;
    }

    if (size < 0) {
        PyErr_Format(FFIError, "don't know the size of ctype '%s'",
                     ct->ct_name);
        return NULL;
    }
    return PyInt_FromSsize_t(size);
}

PyDoc_STRVAR(ffi_alignof_doc,
"Return the natural alignment size in bytes of the argument.\n"
"It can be a string naming a C type, or a 'cdata' instance.");

static PyObject *ffi_alignof(FFIObject *self, PyObject *arg)
{
    int align;
    CTypeDescrObject *ct = _ffi_type(self, arg, ACCEPT_ALL);
    if (ct == NULL)
        return NULL;

    align = get_alignment(ct);
    if (align < 0)
        return NULL;
    return PyInt_FromLong(align);
}

PyDoc_STRVAR(ffi_typeof_doc,
"Parse the C type given as a string and return the\n"
"corresponding <ctype> object.\n"
"It can also be used on 'cdata' instance to get its C type.");

static PyObject *_cpyextfunc_type_index(PyObject *x);  /* forward */

static PyObject *ffi_typeof(FFIObject *self, PyObject *arg)
{
    PyObject *x = (PyObject *)_ffi_type(self, arg, ACCEPT_STRING|ACCEPT_CDATA);
    if (x != NULL) {
        Py_INCREF(x);
    }
    else {
        x = _cpyextfunc_type_index(arg);
    }
    return x;
}

PyDoc_STRVAR(ffi_new_doc,
"Allocate an instance according to the specified C type and return a\n"
"pointer to it.  The specified C type must be either a pointer or an\n"
"array: ``new('X *')`` allocates an X and returns a pointer to it,\n"
"whereas ``new('X[n]')`` allocates an array of n X'es and returns an\n"
"array referencing it (which works mostly like a pointer, like in C).\n"
"You can also use ``new('X[]', n)`` to allocate an array of a\n"
"non-constant length n.\n"
"\n"
"The memory is initialized following the rules of declaring a global\n"
"variable in C: by default it is zero-initialized, but an explicit\n"
"initializer can be given which can be used to fill all or part of the\n"
"memory.\n"
"\n"
"When the returned <cdata> object goes out of scope, the memory is\n"
"freed.  In other words the returned <cdata> object has ownership of\n"
"the value of type 'cdecl' that it points to.  This means that the raw\n"
"data can be used as long as this object is kept alive, but must not be\n"
"used for a longer time.  Be careful about that when copying the\n"
"pointer to the memory somewhere else, e.g. into another structure.");

static PyObject *ffi_new(FFIObject *self, PyObject *args, PyObject *kwds)
{
    CTypeDescrObject *ct;
    PyObject *arg, *init = Py_None;
    static char *keywords[] = {"cdecl", "init", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O:new", keywords,
                                     &arg, &init))
        return NULL;

    ct = _ffi_type(self, arg, ACCEPT_STRING|ACCEPT_CTYPE);
    if (ct == NULL)
        return NULL;

    return direct_newp(ct, init);
}

PyDoc_STRVAR(ffi_cast_doc,
"Similar to a C cast: returns an instance of the named C\n"
"type initialized with the given 'source'.  The source is\n"
"casted between integers or pointers of any type.");

static PyObject *ffi_cast(FFIObject *self, PyObject *args)
{
    CTypeDescrObject *ct;
    PyObject *ob, *arg;
    if (!PyArg_ParseTuple(args, "OO:cast", &arg, &ob))
        return NULL;

    ct = _ffi_type(self, arg, ACCEPT_STRING|ACCEPT_CTYPE);
    if (ct == NULL)
        return NULL;

    return do_cast(ct, ob);
}

PyDoc_STRVAR(ffi_string_doc,
"Return a Python string (or unicode string) from the 'cdata'.  If\n"
"'cdata' is a pointer or array of characters or bytes, returns the\n"
"null-terminated string.  The returned string extends until the first\n"
"null character, or at most 'maxlen' characters.  If 'cdata' is an\n"
"array then 'maxlen' defaults to its length.\n"
"\n"
"If 'cdata' is a pointer or array of wchar_t, returns a unicode string\n"
"following the same rules.\n"
"\n"
"If 'cdata' is a single character or byte or a wchar_t, returns it as a\n"
"string or unicode string.\n"
"\n"
"If 'cdata' is an enum, returns the value of the enumerator as a\n"
"string, or 'NUMBER' if the value is out of range.");

#define ffi_string  b_string     /* ffi_string() => b_string()
                                    from _cffi_backend.c */

PyDoc_STRVAR(ffi_buffer_doc,
"Return a read-write buffer object that references the raw C data\n"
"pointed to by the given 'cdata'.  The 'cdata' must be a pointer or an\n"
"array.  Can be passed to functions expecting a buffer, or directly\n"
"manipulated with:\n"
"\n"
"    buf[:]          get a copy of it in a regular string, or\n"
"    buf[idx]        as a single character\n"
"    buf[:] = ...\n"
"    buf[idx] = ...  change the content");

#define ffi_buffer  b_buffer     /* ffi_buffer() => b_buffer()
                                    from _cffi_backend.c */

PyDoc_STRVAR(ffi_offsetof_doc,
"Return the offset of the named field inside the given structure or\n"
"array, which must be given as a C type name.  You can give several\n"
"field names in case of nested structures.  You can also give numeric\n"
"values which correspond to array items, in case of an array type.");

static PyObject *ffi_offsetof(FFIObject *self, PyObject *args)
{
    PyObject *arg;
    CTypeDescrObject *ct;
    Py_ssize_t i, offset;

    if (PyTuple_Size(args) < 2) {
        PyErr_SetString(PyExc_TypeError,
                        "offsetof() expects at least 2 arguments");
        return NULL;
    }

    arg = PyTuple_GET_ITEM(args, 0);
    ct = _ffi_type(self, arg, ACCEPT_STRING|ACCEPT_CTYPE);
    if (ct == NULL)
        return NULL;

    offset = 0;
    for (i = 1; i < PyTuple_GET_SIZE(args); i++) {
        Py_ssize_t ofs1;
        ct = direct_typeoffsetof(ct, PyTuple_GET_ITEM(args, i), i > 1, &ofs1);
        if (ct == NULL)
            return NULL;
        offset += ofs1;
    }
    return PyInt_FromSsize_t(offset);
}

PyDoc_STRVAR(ffi_addressof_doc,
"Limited equivalent to the '&' operator in C:\n"
"\n"
"1. ffi.addressof(<cdata 'struct-or-union'>) returns a cdata that is a\n"
"pointer to this struct or union.\n"
"\n"
"2. ffi.addressof(<cdata>, field-or-index...) returns the address of a\n"
"field or array item inside the given structure or array, recursively\n"
"in case of nested structures or arrays.\n"
"\n"
"3. ffi.addressof(<library>, \"name\") returns the address of the named\n"
"function or global variable.");

static PyObject *address_of_global_var(PyObject *args);  /* forward */

static PyObject *ffi_addressof(FFIObject *self, PyObject *args)
{
    PyObject *arg, *z, *result;
    CTypeDescrObject *ct;
    Py_ssize_t i, offset = 0;
    int accepted_flags;

    if (PyTuple_Size(args) < 1) {
        PyErr_SetString(PyExc_TypeError,
                        "addressof() expects at least 1 argument");
        return NULL;
    }

    arg = PyTuple_GET_ITEM(args, 0);
    if (LibObject_Check(arg)) {
        /* case 3 in the docstring */
        return address_of_global_var(args);
    }

    ct = _ffi_type(self, arg, ACCEPT_CDATA);
    if (ct == NULL)
        return NULL;

    if (PyTuple_GET_SIZE(args) == 1) {
        /* case 1 in the docstring */
        accepted_flags = CT_STRUCT | CT_UNION | CT_ARRAY;
        if ((ct->ct_flags & accepted_flags) == 0) {
            PyErr_SetString(PyExc_TypeError,
                            "expected a cdata struct/union/array object");
            return NULL;
        }
    }
    else {
        /* case 2 in the docstring */
        accepted_flags = CT_STRUCT | CT_UNION | CT_ARRAY | CT_POINTER;
        if ((ct->ct_flags & accepted_flags) == 0) {
            PyErr_SetString(PyExc_TypeError,
                        "expected a cdata struct/union/array/pointer object");
            return NULL;
        }
        for (i = 1; i < PyTuple_GET_SIZE(args); i++) {
            Py_ssize_t ofs1;
            ct = direct_typeoffsetof(ct, PyTuple_GET_ITEM(args, i),
                                     i > 1, &ofs1);
            if (ct == NULL)
                return NULL;
            offset += ofs1;
        }
    }

    z = new_pointer_type(ct);
    if (z == NULL)
        return NULL;

    result = new_simple_cdata(((CDataObject *)arg)->c_data + offset,
                              (CTypeDescrObject *)z);
    Py_DECREF(z);
    return result;
}

static PyObject *_combine_type_name_l(CTypeDescrObject *ct,
                                      size_t extra_text_len)
{
    size_t base_name_len;
    PyObject *result;
    char *p;

    base_name_len = strlen(ct->ct_name);
    result = PyBytes_FromStringAndSize(NULL, base_name_len + extra_text_len);
    if (result == NULL)
        return NULL;

    p = PyBytes_AS_STRING(result);
    memcpy(p, ct->ct_name, ct->ct_name_position);
    p += ct->ct_name_position;
    p += extra_text_len;
    memcpy(p, ct->ct_name + ct->ct_name_position,
           base_name_len - ct->ct_name_position);
    return result;
}

PyDoc_STRVAR(ffi_getctype_doc,
"Return a string giving the C type 'cdecl', which may be itself a\n"
"string or a <ctype> object.  If 'replace_with' is given, it gives\n"
"extra text to append (or insert for more complicated C types), like a\n"
"variable name, or '*' to get actually the C type 'pointer-to-cdecl'.");

static PyObject *ffi_getctype(FFIObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *c_decl, *res;
    char *p, *replace_with = "";
    int add_paren, add_space;
    CTypeDescrObject *ct;
    size_t replace_with_len;
    static char *keywords[] = {"cdecl", "replace_with", NULL};
#if PY_MAJOR_VERSION >= 3
    PyObject *u;
#endif

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|s:getctype", keywords,
                                     &c_decl, &replace_with))
        return NULL;

    ct = _ffi_type(self, c_decl, ACCEPT_STRING|ACCEPT_CTYPE);
    if (ct == NULL)
        return NULL;

    while (replace_with[0] != 0 && isspace(replace_with[0]))
        replace_with++;
    replace_with_len = strlen(replace_with);
    while (replace_with_len > 0 && isspace(replace_with[replace_with_len - 1]))
        replace_with_len--;

    add_paren = (replace_with[0] == '*' &&
                 ((ct->ct_flags & CT_ARRAY) != 0));
    add_space = (!add_paren && replace_with_len > 0 &&
                 replace_with[0] != '[' && replace_with[0] != '(');

    res = _combine_type_name_l(ct, replace_with_len + add_space + 2*add_paren);
    if (res == NULL)
        return NULL;

    p = PyBytes_AS_STRING(res) + ct->ct_name_position;
    if (add_paren)
        *p++ = '(';
    if (add_space)
        *p++ = ' ';
    memcpy(p, replace_with, replace_with_len);
    if (add_paren)
        p[replace_with_len] = ')';

#if PY_MAJOR_VERSION >= 3
    /* bytes -> unicode string */
    u = PyUnicode_DecodeLatin1(PyBytes_AS_STRING(res),
                               PyBytes_GET_SIZE(res),
                               NULL);
    Py_DECREF(res);
    res = u;
#endif

    return res;
}

PyDoc_STRVAR(ffi_new_handle_doc,
"Return a non-NULL cdata of type 'void *' that contains an opaque\n"
"reference to the argument, which can be any Python object.  To cast it\n"
"back to the original object, use from_handle().  You must keep alive\n"
"the cdata object returned by new_handle()!");

static PyObject *ffi_new_handle(FFIObject *self, PyObject *arg)
{
    CDataObject *cd;

    cd = (CDataObject *)PyObject_GC_New(CDataObject, &CDataOwningGC_Type);
    if (cd == NULL)
        return NULL;
    Py_INCREF(g_ct_voidp);     // <ctype 'void *'>
    cd->c_type = g_ct_voidp;
    Py_INCREF(arg);
    cd->c_data = ((char *)arg) - 42;
    cd->c_weakreflist = NULL;
    PyObject_GC_Track(cd);
    return (PyObject *)cd;
}

PyDoc_STRVAR(ffi_from_handle_doc,
"Cast a 'void *' back to a Python object.  Must be used *only* on the\n"
"pointers returned by new_handle(), and *only* as long as the exact\n"
"cdata object returned by new_handle() is still alive (somewhere else\n"
"in the program).  Failure to follow these rules will crash.");

static PyObject *ffi_from_handle(PyObject *self, PyObject *arg)
{
    CTypeDescrObject *ct;
    char *raw;
    PyObject *x;
    if (!CData_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected a 'cdata' object");
        return NULL;
    }
    ct = ((CDataObject *)arg)->c_type;
    raw = ((CDataObject *)arg)->c_data;
    if (!(ct->ct_flags & CT_CAST_ANYTHING)) {
        PyErr_Format(PyExc_TypeError,
                     "expected a 'cdata' object with a 'void *' out of "
                     "new_handle(), got '%s'", ct->ct_name);
        return NULL;
    }
    if (!raw) {
        PyErr_SetString(PyExc_RuntimeError,
                        "cannot use from_handle() on NULL pointer");
        return NULL;
    }
    x = (PyObject *)(raw + 42);
    Py_INCREF(x);
    return x;
}

PyDoc_STRVAR(ffi_from_buffer_doc,
"Return a <cdata 'char[]'> that points to the data of the given Python\n"
"object, which must support the buffer interface.  Note that this is\n"
"not meant to be used on the built-in types str, unicode, or bytearray\n"
"(you can build 'char[]' arrays explicitly) but only on objects\n"
"containing large quantities of raw data in some other format, like\n"
"'array.array' or numpy arrays.");

static PyObject *ffi_from_buffer(PyObject *self, PyObject *arg)
{
    return direct_from_buffer(g_ct_chararray, arg);
}

PyDoc_STRVAR(ffi_gc_doc,
"Return a new cdata object that points to the same data.\n"
"Later, when this new cdata object is garbage-collected,\n"
"'destructor(old_cdata_object)' will be called.");

static PyObject *gc_weakrefs_build(FFIObject *ffi, CDataObject *cd,
                                   PyObject *destructor);   /* forward */

static PyObject *ffi_gc(FFIObject *self, PyObject *args, PyObject *kwds)
{
    CDataObject *cd;
    PyObject *destructor;
    static char *keywords[] = {"cdata", "destructor", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!O:gc", keywords,
                                     &CData_Type, &cd, &destructor))
        return NULL;

    return gc_weakrefs_build(self, cd, destructor);
}

PyDoc_STRVAR(ffi_callback_doc,
"Return a callback object or a decorator making such a callback object.\n"
"'cdecl' must name a C function pointer type.  The callback invokes the\n"
"specified 'python_callable' (which may be provided either directly or\n"
"via a decorator).  Important: the callback object must be manually\n"
"kept alive for as long as the callback may be invoked from the C code.");

static PyObject *_ffi_callback_decorator(PyObject *outer_args, PyObject *fn)
{
    PyObject *res, *old;

    old = PyTuple_GET_ITEM(outer_args, 1);
    PyTuple_SET_ITEM(outer_args, 1, fn);
    res = b_callback(NULL, outer_args);
    PyTuple_SET_ITEM(outer_args, 1, old);
    return res;
}

static PyObject *ffi_callback(FFIObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *c_decl, *python_callable = Py_None, *error = Py_None;
    PyObject *res;
    static char *keywords[] = {"cdecl", "python_callable", "error", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|OO", keywords,
                                     &c_decl, &python_callable, &error))
        return NULL;

    c_decl = (PyObject *)_ffi_type(self, c_decl, ACCEPT_STRING | ACCEPT_CTYPE |
                                                 CONSIDER_FN_AS_FNPTR);
    if (c_decl == NULL)
        return NULL;

    args = Py_BuildValue("(OOO)", c_decl, python_callable, error);
    if (args == NULL)
        return NULL;

    if (python_callable != Py_None) {
        res = b_callback(NULL, args);
    }
    else {
        static PyMethodDef md = {"callback_decorator",
                                 (PyCFunction)_ffi_callback_decorator, METH_O};
        res = PyCFunction_New(&md, args);
    }
    Py_DECREF(args);
    return res;
}

#ifdef MS_WIN32
PyDoc_STRVAR(ffi_getwinerror_doc,
"Return either the GetLastError() or the error number given by the\n"
"optional 'code' argument, as a tuple '(code, message)'.");

#define ffi_getwinerror  b_getwinerror  /* ffi_getwinerror() => b_getwinerror()
                                           from misc_win32.h */
#endif

PyDoc_STRVAR(ffi_errno_doc, "the value of 'errno' from/to the C calls");

static PyObject *ffi_get_errno(PyObject *self, void *closure)
{
    /* xxx maybe think about how to make the saved errno local
       to an ffi instance */
    return b_get_errno(NULL, NULL);
}

static int ffi_set_errno(PyObject *self, PyObject *newval, void *closure)
{
    PyObject *x = b_set_errno(NULL, newval);
    if (x == NULL)
        return -1;
    Py_DECREF(x);
    return 0;
}

PyDoc_STRVAR(ffi_dlopen_doc,
"Load and return a dynamic library identified by 'name'.  The standard\n"
"C library can be loaded by passing None.\n"
"\n"
"Note that functions and types declared with 'ffi.cdef()' are not\n"
"linked to a particular library, just like C headers.  In the library\n"
"we only look for the actual (untyped) symbols at the time of their\n"
"first access.");

PyDoc_STRVAR(ffi_dlclose_doc,
"Close a library obtained with ffi.dlopen().  After this call, access to\n"
"functions or variables from the library will fail (possibly with a\n"
"segmentation fault).");

static PyObject *ffi_dlopen(PyObject *self, PyObject *args);  /* forward */
static PyObject *ffi_dlclose(PyObject *self, PyObject *args);  /* forward */

PyDoc_STRVAR(ffi_int_const_doc,
"Get the value of an integer constant.\n"
"\n"
"'ffi.integer_const(\"xxx\")' is equivalent to 'lib.xxx' if xxx names an\n"
"integer constant.  The point of this function is limited to use cases\n"
"where you have an 'ffi' object but not any associated 'lib' object.");

static PyObject *ffi_int_const(FFIObject *self, PyObject *args, PyObject *kwds)
{
    char *name;
    PyObject *x;
    static char *keywords[] = {"name", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", keywords, &name))
        return NULL;

    x = ffi_fetch_int_constant(self, name, 0);

    if (x == NULL && !PyErr_Occurred()) {
        PyErr_Format(PyExc_AttributeError,
                     "integer constant '%.200s' not found", name);
    }
    return x;
}

#define METH_VKW  (METH_VARARGS | METH_KEYWORDS)
static PyMethodDef ffi_methods[] = {
 {"addressof",  (PyCFunction)ffi_addressof,  METH_VARARGS, ffi_addressof_doc},
 {"alignof",    (PyCFunction)ffi_alignof,    METH_O,       ffi_alignof_doc},
 {"buffer",     (PyCFunction)ffi_buffer,     METH_VKW,     ffi_buffer_doc},
 {"callback",   (PyCFunction)ffi_callback,   METH_VKW,     ffi_callback_doc},
 {"cast",       (PyCFunction)ffi_cast,       METH_VARARGS, ffi_cast_doc},
 {"dlclose",    (PyCFunction)ffi_dlclose,    METH_VARARGS, ffi_dlclose_doc},
 {"dlopen",     (PyCFunction)ffi_dlopen,     METH_VARARGS, ffi_dlopen_doc},
 {"from_buffer",(PyCFunction)ffi_from_buffer,METH_O,       ffi_from_buffer_doc},
 {"from_handle",(PyCFunction)ffi_from_handle,METH_O,       ffi_from_handle_doc},
 {"gc",         (PyCFunction)ffi_gc,         METH_VKW,     ffi_gc_doc},
 {"getctype",   (PyCFunction)ffi_getctype,   METH_VKW,     ffi_getctype_doc},
#ifdef MS_WIN32
 {"getwinerror",(PyCFunction)ffi_getwinerror,METH_VKW,     ffi_getwinerror_doc},
#endif
 {"integer_const",(PyCFunction)ffi_int_const,METH_VKW,     ffi_int_const_doc},
 {"new",        (PyCFunction)ffi_new,        METH_VKW,     ffi_new_doc},
 {"new_handle", (PyCFunction)ffi_new_handle, METH_O,       ffi_new_handle_doc},
 {"offsetof",   (PyCFunction)ffi_offsetof,   METH_VARARGS, ffi_offsetof_doc},
 {"sizeof",     (PyCFunction)ffi_sizeof,     METH_O,       ffi_sizeof_doc},
 {"string",     (PyCFunction)ffi_string,     METH_VKW,     ffi_string_doc},
 {"typeof",     (PyCFunction)ffi_typeof,     METH_O,       ffi_typeof_doc},
 {NULL}
};

static PyGetSetDef ffi_getsets[] = {
    {"errno",  ffi_get_errno,  ffi_set_errno,  ffi_errno_doc},
    {NULL}
};

static PyTypeObject FFI_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "CompiledFFI",
    sizeof(FFIObject),
    0,
    (destructor)ffi_dealloc,                    /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_BASETYPE,                    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)ffi_traverse,                 /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    ffi_methods,                                /* tp_methods */
    0,                                          /* tp_members */
    ffi_getsets,                                /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    ffiobj_init,                                /* tp_init */
    0,                                          /* tp_alloc */
    ffiobj_new,                                 /* tp_new */
    PyObject_GC_Del,                            /* tp_free */
};


static PyObject *
_fetch_external_struct_or_union(const struct _cffi_struct_union_s *s,
                                PyObject *included_ffis, int recursion)
{
    Py_ssize_t i;

    if (included_ffis == NULL)
        return NULL;

    if (recursion > 100) {
        PyErr_SetString(PyExc_RuntimeError,
                        "recursion overflow in ffi.include() delegations");
        return NULL;
    }

    for (i = 0; i < PyTuple_GET_SIZE(included_ffis); i++) {
        FFIObject *ffi1;
        const struct _cffi_struct_union_s *s1;
        int sindex;
        PyObject *x;

        ffi1 = (FFIObject *)PyTuple_GET_ITEM(included_ffis, i);
        sindex = search_in_struct_unions(&ffi1->types_builder.ctx, s->name,
                                         strlen(s->name));
        if (sindex < 0)  /* not found at all */
            continue;
        s1 = &ffi1->types_builder.ctx.struct_unions[sindex];
        if ((s1->flags & (_CFFI_F_EXTERNAL | _CFFI_F_UNION))
                == (s->flags & _CFFI_F_UNION)) {
            /* s1 is not external, and the same kind (struct or union) as s */
            return _realize_c_struct_or_union(&ffi1->types_builder, sindex);
        }
        /* not found, look more recursively */
        x = _fetch_external_struct_or_union(
                s, ffi1->types_builder.included_ffis, recursion + 1);
        if (x != NULL || PyErr_Occurred())
            return x;   /* either found, or got an error */
    }
    return NULL;   /* not found at all, leave without an error */
}
