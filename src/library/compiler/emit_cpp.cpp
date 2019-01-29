/*
Copyright (c) 2019 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <iostream>
#include "runtime/utf8.h"
#include "kernel/instantiate.h"
#include "library/attribute_manager.h"
#include "library/module.h"
#include "library/compiler/llnf.h"
#include "library/compiler/name_mangling.h"
#include "library/compiler/emit_cpp.h"
#include "library/compiler/builtin.h"

#include "library/trace.h"

namespace lean {
struct cppname_attr_data : public attr_data {
    name m_id;
    cppname_attr_data(name const & id): m_id(id) {}
    cppname_attr_data() {}

    virtual unsigned hash() const override { return m_id.hash(); }
    virtual void parse(abstract_parser & p) override { m_id = p.parse_name(); }
    virtual void print(std::ostream & out) override { out << " " << m_id; }
    void write(serializer & s) const { s << m_id; }
    void read(deserializer & d) { m_id = read_name(d); }
};

typedef typed_attribute<cppname_attr_data> cppname_attr;

static cppname_attr const & get_cppname_attr() {
    return static_cast<cppname_attr const &>(get_system_attribute("cppname"));
}

optional<name> get_cppname_for(environment const & env, name const & n) {
    if (auto const & data = get_cppname_attr().get(env, n)) {
        return optional<name>(data->m_id);
    } else {
        return optional<name>();
    }
}

struct emit_cpp_ext : public environment_extension {
    comp_decls m_code;
};

struct emit_cpp_ext_reg {
    unsigned m_ext_id;
    emit_cpp_ext_reg() { m_ext_id = environment::register_extension(std::make_shared<emit_cpp_ext>()); }
};

static emit_cpp_ext_reg * g_ext = nullptr;
static emit_cpp_ext const & get_extension(environment const & env) {
    return static_cast<emit_cpp_ext const &>(env.get_extension(g_ext->m_ext_id));
}
static environment update(environment const & env, emit_cpp_ext const & ext) {
    return env.update(g_ext->m_ext_id, std::make_shared<emit_cpp_ext>(ext));
}

environment emit_cpp(environment const & env, comp_decls const & ds) {
    /* Remark: we don't generate C++ code here, but at `print_cpp_code`. In this function
       we simply save the LLNF expression in the emit_cpp_ext.
       Reason: the attributes for `ds` are only applied after the compiler is executed,
       and we need to be able to access the `[cppname]` attribute. */
    emit_cpp_ext ext = get_extension(env);
    ext.m_code = append(ext.m_code, ds);
    return update(env, ext);
}

static std::string to_cpp_type(expr const & e) {
    if (is_constant(e)) {
        if (e == mk_enf_object_type() || e == mk_enf_neutral_type()) {
            return "obj*";
        } else if (const_name(e) == get_uint8_name()) {
            return "unsigned char";
        } else if (const_name(e) == get_uint16_name()) {
            return "unsigned short";
        } else if (const_name(e) == get_uint32_name()) {
            return "unsigned";
        } else if (const_name(e) == get_uint64_name()) {
            return "unsigned long long";
        } else if (const_name(e) == get_usize_name()) {
            return "size_t";
        }
    } else if (is_pi(e)) {
        return "obj*";
    }
    throw exception("unknown type");
}

static void open_namespaces_core(std::ostream & out, name const & p) {
    if (p.is_anonymous()) return;
    open_namespaces_core(out, p.get_prefix());
    lean_assert(p.is_string());
    out << "namespace " << p.get_string().to_std_string() << " {\n";
}

/* If `n` has the attribute [cppname], and the "cppname" is hierarchical, then
   we must put `n` code inside of a namespace. */
static void open_namespaces_for(std::ostream & out, environment const & env, name const & n) {
    optional<name> c = get_cppname_for(env, n);
    if (!c || c->is_atomic()) return;
    open_namespaces_core(out, c->get_prefix());
}

static void close_namespaces_for(std::ostream & out, environment const & env, name const & n) {
    optional<name> c = get_cppname_for(env, n);
    if (!c || c->is_atomic()) return;
    name p = c->get_prefix();
    while (!p.is_anonymous()) {
        out << "}";
        p = p.get_prefix();
    }
    out << "\n";
}

static std::string to_base_cpp_name(environment const & env, name const & n) {
    if (optional<name> c = get_cppname_for(env, n)) {
        lean_assert(c->is_string());
        return c->get_string().to_std_string();
    } else {
        return mangle(n);
    }
}

static std::string to_cpp_name(environment const & env, name const & n) {
    if (optional<name> c = get_cppname_for(env, n)) {
        lean_assert(c->is_string());
        return c->to_string("::");
    } else {
        return mangle(n);
    }
}

static std::string to_cpp_init_name(environment const & env, name const & n) {
    if (optional<name> c = get_cppname_for(env, n)) {
        name init_c(c->get_prefix(), (std::string("_init_") + c->get_string().to_std_string()).c_str());
        return init_c.to_string("::");
    } else {
        return std::string("_init_") + mangle(n);
    }
}

static expr get_result_type(expr type) {
    while (is_pi(type)) {
        type = binding_body(type);
    }
    return type;
}

static void emit_fn_decl(std::ostream & out, environment const & env, name const & n) {
    open_namespaces_for(out, env, n);
    expr type = get_constant_ll_type(env, n);
    out << to_cpp_type(get_result_type(type)) << " " << to_base_cpp_name(env, n);
    if (is_pi(type)) {
        out << "(";
        bool first = true;
        while (is_pi(type)) {
            if (first) first = false; else out << ", ";
            out << to_cpp_type(binding_domain(type));
            type = binding_body(type);
        }
        out << ")";
    }
    out << ";\n";
    close_namespaces_for(out, env, n);
}

/* Auxiliary function for `collect_dependencies`. */
static void collect_constant(expr const & e, name_set & deps) {
    lean_assert(is_constant(e));
    if (!is_llnf_op(e) && !is_builtin_constant(const_name(e)) && e != mk_enf_neutral()) {
        deps.insert(const_name(e));
    }
}

/* Collect declarations used by `e` in `deps`. */
static void collect_dependencies(environment const & env, expr e, name_set & deps) {
    while (true) {
        switch (e.kind()) {
        case expr_kind::Lambda:
            e = binding_body(e);
            break;
        case expr_kind::Let:
            collect_dependencies(env, let_value(e), deps);
            e = let_body(e);
            break;
        case expr_kind::App:
            if (is_cases_on_app(env, e)) {
                buffer<expr> args;
                get_app_args(e, args);
                for (expr const & arg : args)
                    collect_dependencies(env, arg, deps);
            } else {
                collect_constant(get_app_fn(e), deps);
            }
            return;
        case expr_kind::Const:
            collect_constant(e, deps);
            return;
        default:
            return;
        }
    }
}

/* Emit C++ function declaration for all functions/constants declared in the current module,
   and their direct dependencies. */
static void emit_fn_decls(std::ostream & out, environment const & env) {
    comp_decls ds = get_extension(env).m_code;
    name_set todo;
    for (comp_decl const & d : ds) {
        if (!todo.contains(d.fst())) {
            todo.insert(d.fst());
            collect_dependencies(env, d.snd(), todo);
        }
    }
    todo.for_each([&](name const & n) { emit_fn_decl(out, env, n); });
}

static void emit_file_header(std::ostream & out, module_name const & m, list<module_name> const & deps) {
    out << "// Lean compiler output\n";
    out << "// Module: " << m << "\n";
    out << "// Imports:"; for (module_name const & d : deps) out << " " << d; out << "\n";
    out << "#include \"runtime/object.h\"\n";
    out << "#include \"runtime/apply.h\"\n";
    out << "typedef lean::object obj;\n";
}

struct emit_fn_fn {
    std::ostream &  m_out;
    name_generator  m_ngen;
    environment     m_env;
    local_ctx       m_lctx;
    name_map<exprs> m_jp_vars;

    static bool is_jmp(expr const & e) {
        return is_llnf_jmp(get_app_fn(e));
    }

    void emit_fvar(expr const & x) {
        lean_assert(is_fvar(x));
        name const & id = fvar_name(x);
        lean_assert(id.is_numeral());
        m_out << "x_" << id.get_numeral();
    }

    void emit_lbl(expr const & jp) {
        lean_assert(is_fvar(jp));
        name const & id = fvar_name(jp);
        lean_assert(id.is_numeral());
        m_out << "lbl_" << id.get_numeral();
    }

    /* Emit instructions that return void. */
    void emit_instr(expr const & e) {
        expr const & f = get_app_fn(e);
        lean_assert(is_llnf_inc(f) || is_llnf_dec(f));
        if (is_llnf_inc(f)) {
            m_out << "lean::inc(";
        } else {
            m_out << "lean::dec(";
        }
        emit_fvar(app_arg(e));
        m_out << ");\n";
    }

    void emit_instr(local_decl const &) {
        // TODO(Leo)
    }

    void emit_cases(expr const & e) {
        lean_assert(is_cases_on_app(m_env, e));
        buffer<expr> args;
        get_app_args(e, args);
        expr const & x = args[0];
        if (m_lctx.get_local_decl(x).get_type() == mk_enf_object_type()) {
            m_out << "switch (lean::obj_tag("; emit_fvar(x); m_out << ")) {\n";
        } else {
            m_out << "switch ("; emit_fvar(x); m_out << ") {\n";
        }
        for (unsigned i = 1; i < args.size(); i++) {
            m_out << "case " << (i-1) << ":\n";
            emit(args[i]);
        }
        m_out << "}\n";
    }

    void emit_jmp(expr const & e) {
        lean_assert(is_jmp(e));
        buffer<expr> args;
        get_app_args(e, args);
        expr const & jp = args[0];
        lean_assert(is_fvar(jp));
        lean_assert(m_jp_vars.contains(fvar_name(jp)));
        exprs params = *m_jp_vars.find(fvar_name(jp));
        lean_assert(length(params) == args.size() - 1);
        for (unsigned i = 1; i < args.size(); i++) {
            emit_fvar(head(params)); m_out << " = "; emit_fvar(args[i]); m_out << ";\n";
            params = tail(params);
        }
        m_out << "goto "; emit_lbl(jp); m_out << ";\n";
    }

    void emit_terminal(expr const & e) {
        if (is_cases_on_app(m_env, e)) {
            emit_cases(e);
        } else if (is_jmp(e)) {
            emit_jmp(e);
        } else if (is_fvar(e)) {
            m_out << "return "; emit_fvar(e); m_out << ";\n";
        } else {
            lean_unreachable();
        }
    }

    void emit(expr e) {
        m_out << "{\n";
        buffer<expr> jps;
        buffer<expr> locals;
        buffer<expr> instrs;
        while (is_let(e)) {
            expr v = instantiate_rev(let_value(e), locals.size(), locals.data());
            if (is_join_point_name(let_name(e))) {
                buffer<expr> jp_vars;
                while (is_lambda(v)) {
                    expr y = m_lctx.mk_local_decl(m_ngen, binding_name(v), binding_domain(v));
                    jp_vars.push_back(y);
                    /* Declare join point parameter, we need them to implement join point calls in this block. */
                    m_out << to_cpp_type(binding_domain(v)) << " "; emit_fvar(y); m_out << ";\n";
                    v = binding_body(v);
                }
                v = instantiate_rev(v, jp_vars.size(), jp_vars.data());
                expr x = m_lctx.mk_local_decl(m_ngen, let_name(e), let_type(e), v);
                locals.push_back(x);
                jps.push_back(x);
                m_jp_vars.insert(fvar_name(x), to_list_ref(jp_vars));
            } else {
                expr x = m_lctx.mk_local_decl(m_ngen, let_name(e), let_type(e), v);
                locals.push_back(x);
                if (!is_llnf_void_type(let_type(e))) {
                    /* Declare local variable.
                       Remark: variables of type `_void` are used to store instructions that do
                       not return any value.  */
                    m_out << to_cpp_type(let_type(e)) << " "; emit_fvar(x); m_out << ";\n";
                }
                instrs.push_back(x);
            }
            e = let_body(e);
        }
        /* emit instructions */
        for (expr const & x : instrs) {
            local_decl d = m_lctx.get_local_decl(x);
            if (is_llnf_void_type(d.get_type())) {
                emit_instr(*d.get_value());
            } else {
                emit_instr(d);
            }
        }
        e = instantiate_rev(e, locals.size(), locals.data());
        emit_terminal(e);
        for (expr const & jp : jps) {
            emit_lbl(jp); m_out << ":\n";
            emit(*m_lctx.get_local_decl(jp).get_value());
        }
        m_out << "}\n";
    }

public:
    emit_fn_fn(std::ostream & out, environment const & env):
        m_out(out), m_env(env) {
    }

    void operator()(comp_decl const & d) {
        name n    = d.fst();
        expr type = get_constant_ll_type(m_env, n);
        expr e    = d.snd();
        m_out << to_cpp_type(get_result_type(type)) << " ";
        if (is_lambda(e)) {
            buffer<expr> locals;
            m_out << to_base_cpp_name(m_env, n);
            m_out << "(";
            bool first = true;
            while (is_lambda(e)) {
                if (first) first = false; else m_out << ", ";
                expr x = m_lctx.mk_local_decl(m_ngen, binding_name(e), binding_domain(e));
                locals.push_back(x);
                m_out << to_cpp_type(binding_domain(e));
                m_out << " ";
                emit_fvar(x);
                e = binding_body(e);
            }
            m_out << ")";
            e = instantiate_rev(e, locals.size(), locals.data());
        } else {
            m_out << "_init_" << to_base_cpp_name(m_env, n) << "()";
        }
        m_out << " {\n";
        emit(e);
        m_out << "}\n";
    }
};

static void emit_fn(std::ostream & out, environment const & env, comp_decl const & d) {
    name const & n = d.fst();
    expr code = d.snd();
    open_namespaces_for(out, env, n);
    emit_fn_fn(out, env)(d);
    close_namespaces_for(out, env, n);
}

static void emit_fns(std::ostream & out, environment const & env) {
    comp_decls ds = get_extension(env).m_code;
    for (comp_decl const & d : ds) {
        emit_fn(out, env, d);
    }
}

static void emit_initialize(std::ostream & out, environment const & env, module_name const & m, list<module_name> const & deps) {
    for (module_name const & d : deps) {
        out << "void _l_initialize_" << mangle(d) << "();\n";
    }
    out << "static bool _G_initialized = false;\n";
    out << "void _l_initialize_" << mangle(m) << "() {\n";
    out << " if (_G_initialized) return;\n";
    out << " _G_initialized = true;\n";
    for (module_name const & d : deps) {
        out << " _l_initialize_" << mangle(d) << "();\n";
    }
    comp_decls ds = get_extension(env).m_code;
    for (comp_decl const & d : ds) {
        name const & n    = d.fst();
        expr const & code = d.snd();
        if (!is_lambda(code)) {
            out << " " << to_cpp_name(env, n) << " = " << to_cpp_init_name(env, n) << "();\n";
        }
    }
    out << "}\n";
}

void print_cpp_code(std::ostream & out, environment const & env, module_name const & m, list<module_name> const & deps) {
    emit_file_header(out, m, deps);
    emit_fn_decls(out, env);
    emit_fns(out, env);
    emit_initialize(out, env, m, deps);
}

void initialize_emit_cpp() {
    g_ext = new emit_cpp_ext_reg();
    register_system_attribute(cppname_attr("cppname", "name to be used by C++ code generator",
                                           [](environment const & env, io_state const &, name const & n, unsigned, bool persistent) {
                                               if (!persistent) throw exception("invalid [cppname] attribute, must be persistent");
                                               if (n.is_anonymous()) throw exception("invalid [cppname] attribute, argument is missing");
                                               name it = n;
                                               while (!it.is_anonymous()) {
                                                   if (!it.is_string()) throw exception("invalid [cppname] attribute, identifier cannot be numeric");
                                                   it = it.get_prefix();
                                               }
                                               return env;
                                           }));
}

void finalize_emit_cpp() {
    delete g_ext;
}
}