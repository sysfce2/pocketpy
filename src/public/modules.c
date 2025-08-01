#include "pocketpy/common/str.h"
#include "pocketpy/objects/base.h"
#include "pocketpy/objects/codeobject.h"
#include "pocketpy/pocketpy.h"
#include "pocketpy/common/utils.h"
#include "pocketpy/objects/object.h"
#include "pocketpy/common/sstream.h"
#include "pocketpy/interpreter/vm.h"
#include "pocketpy/common/_generated.h"

#include <ctype.h>
#include <math.h>

py_Ref py_getmodule(const char* path) {
    VM* vm = pk_current_vm;
    return BinTree__try_get(&vm->modules, (void*)path);
}

py_Ref py_getbuiltin(py_Name name) { return py_getdict(pk_current_vm->builtins, name); }

py_Ref py_getglobal(py_Name name) { return py_getdict(pk_current_vm->main, name); }

void py_setglobal(py_Name name, py_Ref val) { py_setdict(pk_current_vm->main, name, val); }

static void py_ModuleInfo__dtor(py_ModuleInfo* mi) {
    c11_string__delete(mi->name);
    c11_string__delete(mi->package);
    c11_string__delete(mi->path);
}

py_Type pk_module__register() {
    py_Type type = pk_newtype("module", tp_object, NULL, (py_Dtor)py_ModuleInfo__dtor, false, true);
    return type;
}

py_Ref py_newmodule(const char* path) {
    int path_len = strlen(path);
    if(path_len > PK_MAX_MODULE_PATH_LEN) c11__abort("module path too long: %s", path);
    if(path_len == 0) c11__abort("module path cannot be empty");

    py_ModuleInfo* mi = py_newobject(py_retval(), tp_module, -1, sizeof(py_ModuleInfo));

    int last_dot = c11_sv__rindex((c11_sv){path, path_len}, '.');
    if(last_dot == -1) {
        mi->name = c11_string__new(path);
        mi->package = c11_string__new("");
    } else {
        const char* start = path + last_dot + 1;
        mi->name = c11_string__new(start);
        mi->package = c11_string__new2(path, last_dot);
    }

    mi->path = c11_string__new(path);
    path = mi->path->data;

    // we do not allow override in order to avoid memory leak
    // it is because Module objects are not garbage collected
    bool exists = BinTree__contains(&pk_current_vm->modules, (void*)path);
    if(exists) c11__abort("module '%s' already exists", path);

    BinTree__set(&pk_current_vm->modules, (void*)path, py_retval());
    py_GlobalRef retval = py_getmodule(path);
    mi->self = retval;

    // setup __name__
    py_newstrv(py_emplacedict(retval, __name__), c11_string__sv(mi->name));
    // setup __package__
    py_newstrv(py_emplacedict(retval, __package__), c11_string__sv(mi->package));
    // setup __path__
    py_newstrv(py_emplacedict(retval, __path__), c11_string__sv(mi->path));
    return retval;
}

int load_module_from_dll_desktop_only(const char* path) PY_RAISE PY_RETURN;

int py_import(const char* path_cstr) {
    VM* vm = pk_current_vm;
    c11_sv path = {path_cstr, strlen(path_cstr)};
    if(path.size == 0) return ValueError("empty module name");

    if(path.data[0] == '.') {
        // try relative import
        int dot_count = 1;
        while(dot_count < path.size && path.data[dot_count] == '.')
            dot_count++;

        c11_sv top_filename = c11_string__sv(vm->top_frame->co->src->filename);
        int is_init = c11_sv__endswith(top_filename, (c11_sv){"__init__.py", 11});

        py_ModuleInfo* mi = py_touserdata(vm->top_frame->module);
        c11_sv package_sv = c11_string__sv(mi->path);
        if(package_sv.size == 0) {
            return ImportError("attempted relative import with no known parent package");
        }

        c11_vector /* T=c11_sv */ cpnts = c11_sv__split(package_sv, '.');
        for(int i = is_init; i < dot_count; i++) {
            if(cpnts.length == 0)
                return ImportError("attempted relative import beyond top-level package");
            c11_vector__pop(&cpnts);
        }

        if(dot_count < path.size) {
            c11_sv last_cpnt = c11_sv__slice(path, dot_count);
            c11_vector__push(c11_sv, &cpnts, last_cpnt);
        }

        // join cpnts
        c11_sbuf buf;
        c11_sbuf__ctor(&buf);
        for(int i = 0; i < cpnts.length; i++) {
            if(i > 0) c11_sbuf__write_char(&buf, '.');
            c11_sbuf__write_sv(&buf, c11__getitem(c11_sv, &cpnts, i));
        }

        c11_vector__dtor(&cpnts);
        c11_string* new_path = c11_sbuf__submit(&buf);
        int res = py_import(new_path->data);
        c11_string__delete(new_path);
        return res;
    }

    assert(path.data[0] != '.' && path.data[path.size - 1] != '.');

    // check existing module
    py_GlobalRef ext_mod = py_getmodule(path.data);
    if(ext_mod) {
        py_assign(py_retval(), ext_mod);
        return true;
    }

    if(vm->callbacks.lazyimport) {
        py_GlobalRef lazymod = vm->callbacks.lazyimport(path_cstr);
        if(lazymod) {
            c11__rtassert(py_istype(lazymod, tp_module));
            py_assign(py_retval(), lazymod);
            return 1;
        }
    }

    // try import
    c11_string* slashed_path = c11_sv__replace(path, '.', PK_PLATFORM_SEP);
    c11_string* filename = c11_string__new3("%s.py", slashed_path->data);

    bool need_free = true;
    const char* data = load_kPythonLib(path_cstr);
    if(data != NULL) {
        need_free = false;
        goto __SUCCESS;
    }

    data = vm->callbacks.importfile(filename->data);
    if(data != NULL) goto __SUCCESS;

    c11_string__delete(filename);
    filename = c11_string__new3("%s%c__init__.py", slashed_path->data, PK_PLATFORM_SEP);
    data = vm->callbacks.importfile(filename->data);
    if(data != NULL) goto __SUCCESS;

    c11_string__delete(filename);
    c11_string__delete(slashed_path);
    // not found
    return load_module_from_dll_desktop_only(path_cstr);

__SUCCESS:
    do {
    } while(0);
    py_GlobalRef mod = py_newmodule(path_cstr);
    bool ok = py_exec((const char*)data, filename->data, EXEC_MODE, mod);
    py_assign(py_retval(), mod);

    c11_string__delete(filename);
    c11_string__delete(slashed_path);
    if(need_free) PK_FREE((void*)data);
    return ok ? 1 : -1;
}

bool py_importlib_reload(py_Ref module) {
    VM* vm = pk_current_vm;
    py_ModuleInfo* mi = py_touserdata(module);
    // We should ensure that the module is its original py_GlobalRef
    module = mi->self;
    c11_sv path = c11_string__sv(mi->path);
    c11_string* slashed_path = c11_sv__replace(path, '.', PK_PLATFORM_SEP);
    c11_string* filename = c11_string__new3("%s.py", slashed_path->data);
    char* data = vm->callbacks.importfile(filename->data);
    if(data == NULL) {
        c11_string__delete(filename);
        filename = c11_string__new3("%s%c__init__.py", slashed_path->data, PK_PLATFORM_SEP);
        data = vm->callbacks.importfile(filename->data);
    }
    c11_string__delete(slashed_path);
    if(data == NULL) return ImportError("module '%v' not found", path);
    // py_cleardict(module); BUG: removing old classes will cause RELOAD_MODE to fail
    bool ok = py_exec(data, filename->data, RELOAD_MODE, module);
    c11_string__delete(filename);
    PK_FREE(data);
    py_assign(py_retval(), module);
    return ok;
}

//////////////////////////

static bool builtins_exit(int argc, py_Ref argv) {
    int code = 0;
    if(argc > 1) return TypeError("exit() takes at most 1 argument");
    if(argc == 1) {
        PY_CHECK_ARG_TYPE(0, tp_int);
        code = py_toint(argv);
    }
    exit(code);
    return false;
}

static bool builtins_input(int argc, py_Ref argv) {
    if(argc > 1) return TypeError("input() takes at most 1 argument");
    const char* prompt = "";
    if(argc == 1) {
        if(!py_checkstr(argv)) return false;
        prompt = py_tostr(argv);
    }
    py_callbacks()->print(prompt);

    c11_sbuf buf;
    c11_sbuf__ctor(&buf);
    while(true) {
        int c = py_callbacks()->getchr();
        if(c == '\n' || c == '\r') break;
        if(c == EOF) break;
        c11_sbuf__write_char(&buf, c);
    }
    c11_sbuf__py_submit(&buf, py_retval());
    return true;
}

static bool builtins_repr(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    return py_repr(argv);
}

static bool builtins_len(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    return py_len(argv);
}

static bool builtins_hex(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    PY_CHECK_ARG_TYPE(0, tp_int);

    py_i64 val = py_toint(argv);

    if(val == 0) {
        py_newstr(py_retval(), "0x0");
        return true;
    }

    c11_sbuf ss;
    c11_sbuf__ctor(&ss);

    if(val < 0) {
        c11_sbuf__write_char(&ss, '-');
        val = -val;
    }
    c11_sbuf__write_cstr(&ss, "0x");
    bool non_zero = true;
    for(int i = 56; i >= 0; i -= 8) {
        unsigned char cpnt = (val >> i) & 0xff;
        c11_sbuf__write_hex(&ss, cpnt, non_zero);
        if(cpnt != 0) non_zero = false;
    }

    c11_sbuf__py_submit(&ss, py_retval());
    return true;
}

static bool builtins_iter(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    return py_iter(argv);
}

static bool builtins_next(int argc, py_Ref argv) {
    if(argc == 0 || argc > 2) return TypeError("next() takes 1 or 2 arguments");
    int res = py_next(argv);
    if(res == -1) return false;
    if(res) return true;
    if(argc == 1) {
        // StopIteration stored in py_retval()
        return py_raise(py_retval());
    } else {
        py_assign(py_retval(), py_arg(1));
        return true;
    }
}

static bool builtins_hash(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    py_i64 val;
    if(!py_hash(argv, &val)) return false;
    py_newint(py_retval(), val);
    return true;
}

static bool builtins_abs(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    return pk_callmagic(__abs__, 1, argv);
}

static bool builtins_divmod(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    return pk_callmagic(__divmod__, 2, argv);
}

static bool builtins_round(int argc, py_Ref argv) {
    py_i64 ndigits;

    if(argc == 1) {
        ndigits = -1;
    } else if(argc == 2) {
        PY_CHECK_ARG_TYPE(1, tp_int);
        ndigits = py_toint(py_arg(1));
        if(ndigits < 0) return ValueError("ndigits should be non-negative");
    } else {
        return TypeError("round() takes 1 or 2 arguments");
    }

    if(argv->type == tp_int) {
        py_assign(py_retval(), py_arg(0));
        return true;
    } else if(argv->type == tp_float) {
        py_f64 x = py_tofloat(py_arg(0));
        py_f64 offset = x >= 0 ? 0.5 : -0.5;
        if(ndigits == -1) {
            py_newint(py_retval(), (py_i64)(x + offset));
            return true;
        }
        py_f64 factor = pow(10, ndigits);
        py_newfloat(py_retval(), (py_i64)(x * factor + offset) / factor);
        return true;
    }

    return pk_callmagic(__round__, argc, argv);
}

static bool builtins_print(int argc, py_Ref argv) {
    // print(*args, sep=' ', end='\n', flush=False)
    py_TValue* args = py_tuple_data(argv);
    int length = py_tuple_len(argv);
    PY_CHECK_ARG_TYPE(1, tp_str);
    PY_CHECK_ARG_TYPE(2, tp_str);
    PY_CHECK_ARG_TYPE(3, tp_bool);
    c11_sv sep = py_tosv(py_arg(1));
    c11_sv end = py_tosv(py_arg(2));
    bool flush = py_tobool(py_arg(3));
    c11_sbuf buf;
    c11_sbuf__ctor(&buf);
    for(int i = 0; i < length; i++) {
        if(i > 0) c11_sbuf__write_sv(&buf, sep);
        if(!py_str(&args[i])) {
            c11_sbuf__dtor(&buf);
            return false;
        }
        c11_sbuf__write_sv(&buf, py_tosv(py_retval()));
    }
    c11_sbuf__write_sv(&buf, end);
    c11_string* res = c11_sbuf__submit(&buf);
    py_callbacks()->print(res->data);
    if(flush) py_callbacks()->flush();
    c11_string__delete(res);
    py_newnone(py_retval());
    return true;
}

static bool builtins_isinstance(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    if(py_istuple(py_arg(1))) {
        int length = py_tuple_len(py_arg(1));
        for(int i = 0; i < length; i++) {
            py_Ref item = py_tuple_getitem(py_arg(1), i);
            if(!py_checktype(item, tp_type)) return false;
            if(py_isinstance(py_arg(0), py_totype(item))) {
                py_newbool(py_retval(), true);
                return true;
            }
        }
        py_newbool(py_retval(), false);
        return true;
    }

    if(!py_checktype(py_arg(1), tp_type)) return false;
    py_newbool(py_retval(), py_isinstance(py_arg(0), py_totype(py_arg(1))));
    return true;
}

static bool builtins_issubclass(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    if(!py_checktype(py_arg(0), tp_type)) return false;
    if(!py_checktype(py_arg(1), tp_type)) return false;
    py_newbool(py_retval(), py_issubclass(py_totype(py_arg(0)), py_totype(py_arg(1))));
    return true;
}

bool py_callable(py_Ref val) {
    switch(val->type) {
        case tp_nativefunc: return true;
        case tp_function: return true;
        case tp_type: return true;
        case tp_boundmethod: return true;
        case tp_staticmethod: return true;
        case tp_classmethod: return true;
        default: return py_tpfindmagic(val->type, __call__);
    }
}

static bool builtins_callable(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    bool res = py_callable(py_arg(0));
    py_newbool(py_retval(), res);
    return true;
}

static bool builtins_getattr(int argc, py_Ref argv) {
    PY_CHECK_ARG_TYPE(1, tp_str);
    py_Name name = py_namev(py_tosv(py_arg(1)));
    if(argc == 2) {
        return py_getattr(py_arg(0), name);
    } else if(argc == 3) {
        bool ok = py_getattr(py_arg(0), name);
        if(!ok && py_matchexc(tp_AttributeError)) {
            py_clearexc(NULL);
            py_assign(py_retval(), py_arg(2));
            return true;  // default value
        }
        return ok;
    } else {
        return TypeError("getattr() expected 2 or 3 arguments");
    }
    return true;
}

static bool builtins_setattr(int argc, py_Ref argv) {
    PY_CHECK_ARGC(3);
    PY_CHECK_ARG_TYPE(1, tp_str);
    py_Name name = py_namev(py_tosv(py_arg(1)));
    py_newnone(py_retval());
    return py_setattr(py_arg(0), name, py_arg(2));
}

static bool builtins_hasattr(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    PY_CHECK_ARG_TYPE(1, tp_str);
    py_Name name = py_namev(py_tosv(py_arg(1)));
    bool ok = py_getattr(py_arg(0), name);
    if(ok) {
        py_newbool(py_retval(), true);
        return true;
    }
    if(py_matchexc(tp_AttributeError)) {
        py_clearexc(NULL);
        py_newbool(py_retval(), false);
        return true;
    }
    return false;
}

static bool builtins_delattr(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    PY_CHECK_ARG_TYPE(1, tp_str);
    py_Name name = py_namev(py_tosv(py_arg(1)));
    py_newnone(py_retval());
    return py_delattr(py_arg(0), name);
}

static bool builtins_chr(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    PY_CHECK_ARG_TYPE(0, tp_int);
    uint32_t val = py_toint(py_arg(0));
    // convert to utf-8
    char utf8[4];
    int len = c11__u32_to_u8(val, utf8);
    if(len == -1) return ValueError("invalid unicode code point: %d", val);
    py_newstrv(py_retval(), (c11_sv){utf8, len});
    return true;
}

static bool builtins_ord(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    PY_CHECK_ARG_TYPE(0, tp_str);
    c11_sv sv = py_tosv(py_arg(0));
    if(c11_sv__u8_length(sv) != 1) {
        return TypeError("ord() expected a character, but string of length %d found",
                         c11_sv__u8_length(sv));
    }
    int u8bytes = c11__u8_header(sv.data[0], true);
    if(u8bytes == 0) return ValueError("invalid utf-8 char: %c", sv.data[0]);
    int value = c11__u8_value(u8bytes, sv.data);
    py_newint(py_retval(), value);
    return true;
}

static bool builtins_id(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    if(argv->is_ptr) {
        py_newint(py_retval(), (intptr_t)argv->_obj);
    } else {
        py_newnone(py_retval());
    }
    return true;
}

static bool builtins_globals(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    py_newglobals(py_retval());
    return true;
}

static bool builtins_locals(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    py_newlocals(py_retval());
    return true;
}

void py_newglobals(py_OutRef out) {
    py_Frame* frame = pk_current_vm->top_frame;
    py_Frame_newglobals(frame, out);
}

void py_newlocals(py_OutRef out) {
    py_Frame* frame = pk_current_vm->top_frame;
    py_Frame_newlocals(frame, out);
}

static void pk_push_special_locals() {
    py_Frame* frame = pk_current_vm->top_frame;
    if(!frame) {
        py_pushnil();
        return;
    }
    if(frame->is_locals_special) {
        py_push(frame->locals);
    } else {
        py_StackRef out = py_pushtmp();
        out->type = tp_locals;
        out->is_ptr = false;
        out->extra = 0;
        // this is a weak reference
        // which will expire when the frame is destroyed
        out->_ptr = frame;
    }
}

static bool _builtins_execdyn(const char* title, int argc, py_Ref argv, enum py_CompileMode mode) {
    switch(argc) {
        case 1: {
            py_newglobals(py_pushtmp());
            pk_push_special_locals();
            break;
        }
        case 2: {
            // globals
            if(py_isnone(py_arg(1))) {
                py_newglobals(py_pushtmp());
            } else {
                py_push(py_arg(1));
            }
            // locals
            py_pushnil();
            break;
        }
        case 3: {
            // globals
            if(py_isnone(py_arg(1))) {
                py_newglobals(py_pushtmp());
            } else {
                py_push(py_arg(1));
            }
            // locals
            if(py_isnone(py_arg(2))) {
                py_pushnil();
            } else {
                py_push(py_arg(2));
            }
            break;
        }
        default: return TypeError("%s() takes at most 3 arguments", title);
    }

    if(py_isstr(argv)) {
        bool ok = py_compile(py_tostr(argv), "<string>", mode, true);
        if(!ok) return false;
        py_push(py_retval());
    } else if(py_istype(argv, tp_code)) {
        py_push(argv);
    } else {
        return TypeError("%s() expected 'str' or 'code', got '%t'", title, argv->type);
    }

    py_Frame* frame = pk_current_vm->top_frame;
    // [globals, locals, code]
    CodeObject* code = py_touserdata(py_peek(-1));
    if(code->src->is_dynamic) {
        bool ok = pk_execdyn(code, frame ? frame->module : NULL, py_peek(-3), py_peek(-2));
        py_shrink(3);
        return ok;
    } else {
        if(argc != 1) {
            return ValueError(
                "code object is not dynamic, `globals` and `locals` must not be specified");
        }
        bool ok = pk_exec(code, frame ? frame->module : NULL);
        py_shrink(3);
        return ok;
    }
}

static bool builtins_exec(int argc, py_Ref argv) {
    bool ok = _builtins_execdyn("exec", argc, argv, EXEC_MODE);
    py_newnone(py_retval());
    return ok;
}

static bool builtins_eval(int argc, py_Ref argv) {
    return _builtins_execdyn("eval", argc, argv, EVAL_MODE);
}

static bool
    pk_smartexec(const char* source, py_Ref module, enum py_CompileMode mode, va_list args) {
    if(module == NULL) module = pk_current_vm->main;
    pk_mappingproxy__namedict(py_pushtmp(), module);  // globals
    py_newdict(py_pushtmp());                         // locals
    bool ok = py_compile(source, "<string>", mode, true);
    if(!ok) return false;
    py_push(py_retval());
    // [globals, locals, code]
    CodeObject* co = py_touserdata(py_peek(-1));
    py_StackRef locals = py_peek(-2);
    int max_index = -1;
    c11__foreach(Bytecode, &co->codes, bc) {
        if(bc->op == OP_LOAD_NAME) {
            c11_sv name = py_name2sv(c11__getitem(py_Name, &co->names, bc->arg));
            if(name.data[0] != '_') continue;
            int index;
            if(name.size == 1) {
                index = 0;
            } else if(name.size == 2 && isdigit(name.data[1])) {
                index = name.data[1] - '0';
            } else {
                continue;
            }
            max_index = c11__max(max_index, index);
        }
    }

    if(max_index == -1) return ValueError("no placeholder found in the source");

    for(int i = 0; i <= max_index; i++) {
        py_Ref val = va_arg(args, py_Ref);
        char buf[3];
        buf[0] = '_';
        buf[1] = '0' + i;
        buf[2] = '\0';
        py_dict_setitem_by_str(locals, buf, val);
        if(i == 0) {
            // _ => _0
            py_dict_setitem_by_str(locals, "_", val);
        }
    }
    ok = pk_execdyn(co, module, py_peek(-3), locals);
    if(!ok) return false;
    py_shrink(3);
    return true;
}

bool py_smartexec(const char* source, py_Ref module, ...) {
    va_list args;
    va_start(args, module);
    bool ok = pk_smartexec(source, module, EXEC_MODE, args);
    va_end(args);
    return ok;
}

bool py_smarteval(const char* source, py_Ref module, ...) {
    va_list args;
    va_start(args, module);
    bool ok = pk_smartexec(source, module, EVAL_MODE, args);
    va_end(args);
    return ok;
}

static bool builtins_compile(int argc, py_Ref argv) {
    PY_CHECK_ARGC(3);
    for(int i = 0; i < 3; i++) {
        if(!py_checktype(py_arg(i), tp_str)) return false;
    }
    const char* source = py_tostr(py_arg(0));
    const char* filename = py_tostr(py_arg(1));
    const char* mode = py_tostr(py_arg(2));

    enum py_CompileMode compile_mode;
    if(strcmp(mode, "exec") == 0) {
        compile_mode = EXEC_MODE;
    } else if(strcmp(mode, "eval") == 0) {
        compile_mode = EVAL_MODE;
    } else if(strcmp(mode, "single") == 0) {
        compile_mode = SINGLE_MODE;
    } else {
        return ValueError("compile() mode must be 'exec', 'eval', or 'single'");
    }
    return py_compile(source, filename, compile_mode, true);
}

static bool builtins__import__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    PY_CHECK_ARG_TYPE(0, tp_str);
    int res = py_import(py_tostr(argv));
    if(res == -1) return false;
    if(res) return true;
    return ImportError("module '%s' not found", py_tostr(argv));
}

static bool NoneType__repr__(int argc, py_Ref argv) {
    py_newstr(py_retval(), "None");
    return true;
}

static bool ellipsis__repr__(int argc, py_Ref argv) {
    py_newstr(py_retval(), "...");
    return true;
}

static bool NotImplementedType__repr__(int argc, py_Ref argv) {
    py_newstr(py_retval(), "NotImplemented");
    return true;
}

py_GlobalRef pk_builtins__register() {
    py_GlobalRef builtins = py_newmodule("builtins");
    py_bindfunc(builtins, "exit", builtins_exit);
    py_bindfunc(builtins, "input", builtins_input);
    py_bindfunc(builtins, "repr", builtins_repr);
    py_bindfunc(builtins, "len", builtins_len);
    py_bindfunc(builtins, "hex", builtins_hex);
    py_bindfunc(builtins, "iter", builtins_iter);
    py_bindfunc(builtins, "next", builtins_next);
    py_bindfunc(builtins, "hash", builtins_hash);
    py_bindfunc(builtins, "abs", builtins_abs);
    py_bindfunc(builtins, "divmod", builtins_divmod);
    py_bindfunc(builtins, "round", builtins_round);

    py_bind(builtins, "print(*args, sep=' ', end='\\n', flush=False)", builtins_print);

    py_bindfunc(builtins, "isinstance", builtins_isinstance);
    py_bindfunc(builtins, "issubclass", builtins_issubclass);
    py_bindfunc(builtins, "callable", builtins_callable);

    py_bindfunc(builtins, "getattr", builtins_getattr);
    py_bindfunc(builtins, "setattr", builtins_setattr);
    py_bindfunc(builtins, "hasattr", builtins_hasattr);
    py_bindfunc(builtins, "delattr", builtins_delattr);

    py_bindfunc(builtins, "chr", builtins_chr);
    py_bindfunc(builtins, "ord", builtins_ord);
    py_bindfunc(builtins, "id", builtins_id);

    py_bindfunc(builtins, "globals", builtins_globals);
    py_bindfunc(builtins, "locals", builtins_locals);
    py_bindfunc(builtins, "exec", builtins_exec);
    py_bindfunc(builtins, "eval", builtins_eval);
    py_bindfunc(builtins, "compile", builtins_compile);

    py_bindfunc(builtins, "__import__", builtins__import__);

    // some patches
    py_bindmagic(tp_NoneType, __repr__, NoneType__repr__);
    py_setdict(py_tpobject(tp_NoneType), __hash__, py_None());
    py_bindmagic(tp_ellipsis, __repr__, ellipsis__repr__);
    py_setdict(py_tpobject(tp_ellipsis), __hash__, py_None());
    py_bindmagic(tp_NotImplementedType, __repr__, NotImplementedType__repr__);
    py_setdict(py_tpobject(tp_NotImplementedType), __hash__, py_None());
    return builtins;
}

void function__gc_mark(void* ud, c11_vector* p_stack) {
    Function* func = ud;
    if(func->globals) pk__mark_value(func->globals);
    if(func->closure) {
        NameDict* dict = func->closure;
        for(int i = 0; i < dict->capacity; i++) {
            NameDict_KV* kv = &dict->items[i];
            if(kv->key == NULL) continue;
            pk__mark_value(&kv->value);
        }
    }
    FuncDecl__gc_mark(func->decl, p_stack);
}

static bool function__doc__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    Function* func = py_touserdata(py_arg(0));
    if(func->decl->docstring) {
        py_newstr(py_retval(), func->decl->docstring);
    } else {
        py_newnone(py_retval());
    }
    return true;
}

static bool function__name__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    Function* func = py_touserdata(py_arg(0));
    py_newstr(py_retval(), func->decl->code.name->data);
    return true;
}

static bool function__repr__(int argc, py_Ref argv) {
    // <function f at 0x10365b9c0>
    PY_CHECK_ARGC(1);
    Function* func = py_touserdata(py_arg(0));
    c11_sbuf buf;
    c11_sbuf__ctor(&buf);
    c11_sbuf__write_cstr(&buf, "<function ");
    c11_sbuf__write_cstr(&buf, func->decl->code.name->data);
    c11_sbuf__write_cstr(&buf, " at ");
    c11_sbuf__write_ptr(&buf, func);
    c11_sbuf__write_char(&buf, '>');
    c11_sbuf__py_submit(&buf, py_retval());
    return true;
}

py_Type pk_function__register() {
    py_Type type =
        pk_newtype("function", tp_object, NULL, (void (*)(void*))Function__dtor, false, true);
    py_bindproperty(type, "__doc__", function__doc__, NULL);
    py_bindproperty(type, "__name__", function__name__, NULL);
    py_bindmagic(type, __repr__, function__repr__);
    return type;
}

static bool nativefunc__repr__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    py_newstr(py_retval(), "<nativefunc object>");
    return true;
}

py_Type pk_nativefunc__register() {
    py_Type type = pk_newtype("nativefunc", tp_object, NULL, NULL, false, true);
    py_bindmagic(type, __repr__, nativefunc__repr__);
    return type;
}

static bool super__new__(int argc, py_Ref argv) {
    py_Type class_arg = 0;
    py_Frame* frame = pk_current_vm->top_frame;
    py_Ref self_arg = NULL;
    if(argc == 1) {
        // super()
        if(!frame->is_locals_special) {
            py_TValue* callable = frame->p0;
            if(callable->type == tp_boundmethod) callable = py_getslot(frame->p0, 1);
            if(callable->type == tp_function) {
                Function* func = py_touserdata(callable);
                if(func->clazz != NULL) {
                    class_arg = ((py_TypeInfo*)PyObject__userdata(func->clazz))->index;
                    if(frame->co->nlocals > 0) { self_arg = &frame->locals[0]; }
                }
            }
        }
        if(class_arg == 0 || self_arg == NULL) return RuntimeError("super(): no arguments");
        if(self_arg->type == tp_type) {
            // f(cls, ...)
            class_arg = pk_typeinfo(class_arg)->base;
            if(class_arg == 0) return RuntimeError("super(): base class is invalid");
            py_assign(py_retval(), py_tpobject(class_arg));
            return true;
        }
    } else if(argc == 3) {
        // super(type[T], obj)
        PY_CHECK_ARG_TYPE(1, tp_type);
        class_arg = py_totype(py_arg(1));
        self_arg = py_arg(2);
        if(!py_isinstance(self_arg, class_arg)) {
            return TypeError("super(type, obj): obj must be an instance of type");
        }
    } else {
        return TypeError("super() takes 0 or 2 arguments");
    }

    class_arg = pk_typeinfo(class_arg)->base;
    if(class_arg == 0) return RuntimeError("super(): base class is invalid");

    py_Type* p_class_arg = py_newobject(py_retval(), tp_super, 1, sizeof(py_Type));
    *p_class_arg = class_arg;
    py_setslot(py_retval(), 0, self_arg);
    return true;
}

py_Type pk_super__register() {
    py_Type type = pk_newtype("super", tp_object, NULL, NULL, false, true);
    py_bindmagic(type, __new__, super__new__);
    return type;
}
