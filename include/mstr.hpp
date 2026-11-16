/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
// mstr.hpp: C++17 RAII wrappers for mstr (header-only; no exceptions needed).
//
// The value type itself gained C++ members in mstr.h (size, data, operator[],
// operator==, conversion to/from std::string_view, explicit std::string).
// This header adds the lifetime classes:
//
//   mstr::Scope  RAII scope. Everything allocated inside is freed at the closing
//                brace. promote(v) ends the scope early and returns v in the
//                parent. Non-copyable, non-movable (it is a stack bookmark).
//   mstr::Box    the only owning type, move-only. Frees on destruction.
//
// Not thread-safe by design: a Scope must be destroyed on the thread that made it.
#ifndef MSTR_HPP
#define MSTR_HPP

#include "mstr.h"

#include <utility>

#ifndef MSTR_CXX17_
#error "mstr.hpp needs C++17"
#endif

class mstr::Scope {
public:
#if defined(__GNUC__) || defined(__clang__)
    // File and line of the caller, so checked-mode messages point at the user's scope.
    explicit Scope(const char *file = __builtin_FILE(), int line = __builtin_LINE())
        : s_(mstr_scope_enter_(file, line)) {}
#else
    Scope() : s_(mstr_scope_enter_(nullptr, 0)) {}
#endif
    ~Scope() { mstr_scope_leave(&s_); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

    // Ends this scope now and returns `v` valid in the parent scope.
    mstr promote(mstr v) { return mstr_promote_(&s_, v, nullptr, 0); }

private:
    mstr_scope_t s_;
};

class mstr::Box {
public:
    Box() noexcept : b_{} {}
    explicit Box(mstr s) : b_(mstr_own(s)) {}
    Box(Box &&o) noexcept : b_(o.b_) { o.b_ = mstr_box{}; }
    Box &operator=(Box &&o) noexcept
    {
        if (this != &o) {
            mstr_box_free(&b_);
            b_ = o.b_;
            o.b_ = mstr_box{};
        }
        return *this;
    }
    Box(const Box &) = delete;
    Box &operator=(const Box &) = delete;
    ~Box() { mstr_box_free(&b_); }

    // Borrowed view: valid until this Box is freed, moved from, or destroyed.
    mstr view() const { return mstr_unbox(&b_); }
    // Frees the buffer and leaves a valid, empty Box (not the C "freed" state, which
    // checked builds treat as use-after-free).
    void reset() noexcept
    {
        mstr_box_free(&b_);
        b_ = mstr_box{};
    }

private:
    mstr_box b_;
};

#endif // MSTR_HPP
