/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 saalineo */
// C++17 wrapper tests (mstr.hpp). Built with -Wall -Wextra -Wpedantic -Wconversion -Werror.
#include "mstr.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

static int g_fail, g_run;
#define CHECK(...)                                                                                \
    do {                                                                                          \
        g_run++;                                                                                  \
        if (!(__VA_ARGS__)) { g_fail++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); } \
    } while (0)

static_assert(sizeof(mstr) == 24, "24 bytes");
static_assert(std::is_trivially_copyable<mstr>::value, "still a plain value");
static_assert(std::is_standard_layout<mstr>::value, "still standard layout");
static_assert(!std::is_copy_constructible<mstr::Box>::value, "Box is move-only");
static_assert(std::is_move_constructible<mstr::Box>::value, "Box is movable");
static_assert(!std::is_copy_constructible<mstr::Scope>::value, "Scope is not copyable");

static mstr make_greeting(mstr who)
{
    mstr::Scope sc;
    mstr tmp = mstr_cat(S("Hello, dear and very honoured "), who);
    return sc.promote(mstr_upper(tmp));
}

int main()
{
    // Scope: RAII rewind and promote
    {
        std::size_t base = mstr_debug_arena_used_();
        {
            mstr::Scope sc;
            mstr big = mstr_repeat(S("0123456789"), 100);
            CHECK(big.size() == 1000 && mstr_debug_arena_used_() > base);
        }
        CHECK(mstr_debug_arena_used_() == base);
        mstr::Scope outer;
        mstr g = make_greeting(S("guest"));
        CHECK(g == S("HELLO, DEAR AND VERY HONOURED GUEST") && g.size() == 35);
        CHECK(mstr_state(&g) == MSTR_REGION);
    }
    // value conveniences
    {
        mstr::Scope sc;
        mstr s = S("héllo wörld, long enough to leave the SSO buffer");
        CHECK(s[0] == 'h' && s[s.size() - 1] == 'r');
        CHECK(s == s && !(s != s) && s != S("other"));
        CHECK(s == std::string_view("héllo wörld, long enough to leave the SSO buffer"));
        CHECK(!(s == std::string_view("nope")));

        std::string_view sv = s;                                   // implicit
        CHECK(sv.size() == s.size() && sv.substr(0, 1) == "h");
        std::string str = static_cast<std::string>(s);             // explicit
        CHECK(str.size() == s.size() && std::memcmp(str.data(), s.data(), s.size()) == 0);
        CHECK(!std::is_convertible<mstr, std::string>::value);     // std::string must stay explicit

        mstr from_sv = mstr::from(std::string_view("copied from a string_view that is quite long, yes"));
        CHECK(mstr_state(&from_sv) == MSTR_REGION && from_sv.size() == 49);
        mstr from_str = mstr::from(std::string("short"));
        CHECK(mstr_state(&from_str) == MSTR_SSO && from_str == S("short"));
        mstr emb = mstr::from(std::string_view("a\0b", 3));
        CHECK(emb.size() == 3 && emb[1] == '\0');

        // works with the C macros too
        std::size_t n = 0;
        mstr_each(tok, S("a,b,c"), S(",")) { n += tok.size(); }
        CHECK(n == 3);
        mstr r = MSTR_PIPE(S("  mixed Case value  "), mstr_trim, mstr_upper, mstr_replace_all(S(" "), S("_")));
        CHECK(r == S("MIXED_CASE_VALUE"));
        CHECK(std::strcmp(MSTR_CSTR(r), "MIXED_CASE_VALUE") == 0);
    }
    // Box: move-only ownership, outlives scopes
    {
        mstr::Box keep;
        {
            mstr::Scope sc;
            mstr tmp = mstr_repeat(S("box-me-"), 10);
            keep = mstr::Box(tmp);
        }
        CHECK(keep.view().size() == 70 && keep.view()[0] == 'b');
        mstr::Box moved(std::move(keep));
        CHECK(keep.view().size() == 0);                            // moved-from is empty
        CHECK(moved.view() == S("box-me-box-me-box-me-box-me-box-me-box-me-box-me-box-me-box-me-box-me-"));
        mstr::Box tiny{S("tiny")};                                // <= 22 bytes: no allocation
        CHECK(tiny.view() == S("tiny"));
        moved = std::move(tiny);                                   // frees the old buffer
        CHECK(moved.view() == S("tiny"));
        moved.reset();
        moved.reset();                                             // idempotent
        CHECK(moved.view().size() == 0);
    }
    mstr_thread_cleanup();
    std::printf("cpp wrapper: %d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
