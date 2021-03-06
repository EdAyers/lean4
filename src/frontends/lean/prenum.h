/*
Copyright (c) 2015 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include "runtime/mpz.h"
#include "kernel/expr.h"

namespace lean {
/** \brief Create a pre-numeral. We create pre-numerals at parsing time. The elaborator is responsible for
    encoding them using the polymorphic operations zero, one, bit0 and bit1
    \remark Fully elaborated terms do not contain pre-numerals */
expr mk_prenum(mpz const & v);
/** \brief Return true iff \c e is a pre-numeral */
bool is_prenum(expr const & e);
/** \brief Return the value stored in the pre-numeral
    \pre is_prenum(e) */
mpz prenum_value(expr const & e);
void initialize_prenum();
void finalize_prenum();
}
