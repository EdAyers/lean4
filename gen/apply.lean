import init.lean.config system.io
open io

@[reducible] def m := reader_t handle io

def m.run (a : m unit) (out : option string := none) : io unit :=
match out with
| some out := do
   h ← mk_file_handle out io.mode.write,
   a.run h,
   fs.close h
| none := stdout >>= a.run

def emit (s : string) : m unit :=
⟨λ h, fs.write h s⟩

def mk_typedef_fn (i : nat) : m unit :=
let args := string.join $ (list.repeat "obj*" i).intersperse ", " in
do emit $ sformat! "typedef obj* (*fn{i})({args}); // NOLINT\n",
   emit $ sformat! "#define FN{i}(f) reinterpret_cast<fn{i}>(closure_fun(f))\n"

-- Make string: "obj* a1, obj* a2, ..., obj* an"
def mk_arg_decls (n : nat) : string :=
string.join $ (n.repeat (λ i r, r ++ [sformat! "obj* a{i+1}"]) []).intersperse ", "

-- Make string: "a1, a2, ..., a{n}"
def mk_args (n : nat) : string :=
string.join $ (n.repeat (λ i r, r ++ [sformat! "a{i+1}"]) []).intersperse ", "

-- Make string: "a{s}, a{s+1}, ..., a{n}"
def mk_args_from (s n : nat) : string :=
string.join $ ((n-s).repeat (λ i r, r ++ [sformat! "a{s+i+1}"]) []).intersperse ", "

-- Make string: "as[0], as[1], ..., as[n-1]"
def mk_as_args (n : nat) : string :=
string.join $ (n.repeat (λ i r, r ++ [sformat! "as[{i}]"]) []).intersperse ", "

-- Make string: "fx(0), ..., fx(n-1)"
def mk_fs_args (n : nat) : string :=
string.join $ (n.repeat (λ i r, r ++ [sformat! "fx({i})"]) []).intersperse ", "

def mk_apply_i (n : nat) (max : nat) : m unit :=
let arg_decls := mk_arg_decls n in
let args := mk_args n in
do emit $ sformat! "obj* apply_{n}(obj* f, {arg_decls}) {{\n",
   emit "unsigned arity = closure_arity(f);\n",
   emit "unsigned fixed = closure_num_fixed(f);\n",
   emit $ sformat! "if (arity == fixed + {n}) {{\n",
     emit $ sformat! "  switch (arity) {{\n",
     max.mrepeat $ λ i, do {
       let j := i + 1,
       when (j ≥ n) $
         let fs := mk_fs_args (j - n) in
         let sep := if j = n then "" else ", " in
         emit $ sformat! "  case {j}: return FN{j}(f)({fs}{sep}{args});\n"
     },
     emit "  default:\n",
       emit $ sformat! "    lean_assert(arity > {max});\n",
       n.mrepeat $ λ i, emit $ sformat! "    fx(arity-{n-i}) = a{i+1};\n",
       emit "    return FNN(f)(closure_arg_cptr(f));\n",
     emit "  }\n",
   emit $ sformat! "} else if (arity < fixed + {n}) {{\n",
     if n ≥ 2 then do
       emit $ sformat! "  obj * as[{n}] = {{ {args} };\n",
       emit $ sformat! "  if (arity > {max}) {{\n",
         emit "    for (unsigned i = 0; i < arity-fixed; i++) fx(fixed+i) = as[i];\n",
         emit $ sformat! "    return apply_nc(FNN(f)(closure_arg_cptr(f)), {n}+fixed-arity, as+arity-fixed);\n",
       emit "  } else {\n",
       emit "    obj* args[16];\n",
       emit "    for (unsigned i = 0; i < fixed; i++) args[i] = fx(i);\n",
       emit "    for (unsigned i = 0; i < arity-fixed; i++) args[fixed+i] = as[i];\n",
       emit $ sformat! "    return apply_nc(curry(f, arity, args), {n}+fixed-arity, as+arity-fixed);\n",
       emit "  }\n"
     else emit "  lean_assert(fixed < arity);\n  lean_unreachable();\n",
   emit "} else {\n",
     emit $ sformat! "  return fix_args(f, {{{args}});\n",
   emit "}\n",
   emit "}\n"

def mk_curry (max : nat) : m unit :=
do emit "static obj* curry(obj* f, unsigned n, obj** as) {\n",
   emit "switch (n) {\n",
   emit "case 0: lean_unreachable();\n",
   max.mrepeat $ λ i,
     let as := mk_as_args (i+1) in
     emit $ sformat! "case {i+1}: return FN{i+1}(f)({as});\n",
   emit "default: return FNN(f)(as);\n",
   emit "}\n",
   emit "}\n"

def mk_apply_n (max : nat) : m unit :=
do emit "obj* apply_n(obj* f, unsigned n, obj** as) {\n",
   emit "switch (n) {\n",
   emit "case 0: lean_unreachable();\n",
   max.mrepeat $ λ i,
     let as := mk_as_args (i+1) in
     emit $ sformat! "case {i+1}: return apply_{i+1}(f, {as});\n",
   emit "default: return apply_m(f, n, as);\n",
   emit "}\n",
   emit "}\n"

def mk_apply_m (max : nat) : m unit :=
do emit "obj* apply_m(obj* f, unsigned n, obj** as) {\n",
   emit $ sformat! "lean_assert(n > {max});\n",
   emit "unsigned arity = closure_arity(f);\n",
   emit "unsigned fixed = closure_num_fixed(f);\n",
   emit $ sformat! "if (arity == fixed + n) {{\n",
     emit "  for (unsigned i = 0; i < n; i++) fx(arity-n+i) = as[i];\n",
     emit "  return FNN(f)(closure_arg_cptr(f));\n",
   emit $ sformat! "} else if (arity < fixed + n) {{\n",
     emit "  for (unsigned i = 0; i < arity-fixed; i++) fx(fixed+i) = as[i];\n",
     emit "  return apply_nc(FNN(f)(closure_arg_cptr(f)), n+fixed-arity, as+arity-fixed);\n",
   emit "} else {\n",
     emit "  return fix_args(f, n, as);\n",
   emit "}\n",
   emit "}\n"

def mk_fix_args : m unit :=
emit "
static obj* fix_args(obj* f, unsigned n, obj*const* as) {
    unsigned arity = closure_arity(f);
    unsigned fixed = closure_num_fixed(f);
    unsigned new_fixed = fixed + n;
    lean_assert(new_fixed < arity);
    obj * r = alloc_closure(closure_fun(f), arity, new_fixed);
    obj ** source = closure_arg_cptr(f);
    obj ** target = closure_arg_cptr(r);
    for (unsigned i = 0; i < fixed; i++, source++, target++) {
        *target = *source;
        if (!is_scalar(*target)) inc_ref(*target);
    }
    for (unsigned i = 0; i < n; i++, as++, target++) {
        *target = *as;
        if (!is_scalar(*target)) inc_ref(*target);
    }
    inc_ref(r);
    return r;
}

static inline obj* fix_args(obj* f, std::initializer_list<obj*> const & l) {
    return fix_args(f, l.size(), l.begin());
}
"

def mk_copyright : m unit :=
emit "
/*
Copyright (c) 2018 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
"

def mk_apply_cpp (max : nat) : m unit :=
do mk_copyright,
   emit "// DO NOT EDIT, this is an automatically generated file\n",
   emit "// Generated using script: ../../gen/apply.lean\n",
   emit "#include \"util/apply.h\"\n",
   emit "namespace lean {\n",
   emit "#define obj lean_obj\n",
   emit "#define fx(i) closure_arg_cptr(f)[i]\n",
   mk_fix_args,
   max.mrepeat $ λ i, mk_typedef_fn (i+1),
   emit "typedef obj* (*fnn)(obj**); // NOLINT\n",
   emit "#define FNN(f) reinterpret_cast<fnn>(closure_fun(f))\n",
   mk_curry max,
   emit "obj* apply_n(obj*, unsigned, obj**);\n",
   emit "static inline obj* apply_nc(obj* f, unsigned n, obj** as) { obj* r = apply_n(f, n, as); dec_ref_core(f); return r; }\n",
   max.mrepeat $ λ i, do { mk_apply_i (i+1) max },
   mk_apply_m max,
   mk_apply_n max,
   emit "}\n"

-- Make string: "lean_obj* a1, lean_obj* a2, ..., lean_obj* an"
def mk_arg_decls' (n : nat) : string :=
string.join $ (n.repeat (λ i r, r ++ [sformat! "lean_obj* a{i+1}"]) []).intersperse ", "

def mk_apply_h (max : nat) : m unit :=
do mk_copyright,
   emit "// DO NOT EDIT, this is an automatically generated file\n",
   emit "// Generated using script: ../../gen/apply.lean\n",
   emit "#include \"util/lean_obj.h\"\n",
   emit "namespace lean {\n",
   max.mrepeat $ λ i,
     let args := mk_arg_decls' (i+1) in
     emit $ sformat! "lean_obj* apply_{i+1}(lean_obj* f, {args});\n",
   emit "lean_obj* apply_n(lean_obj* f, unsigned n, lean_obj** args);\n",
   emit $ sformat! "// pre: n > {max}\n",
   emit "lean_obj* apply_m(lean_obj* f, unsigned n, lean_obj** args);\n",
   emit "}\n"

-- #eval (mk_apply_cpp lean.closure_max_args).run none

#eval (mk_apply_cpp lean.closure_max_args).run "..//src//util//apply.cpp"
#eval (mk_apply_h lean.closure_max_args).run "..//src//util//apply.h"