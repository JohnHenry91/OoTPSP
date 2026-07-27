#ifndef ASSERT_H
#define ASSERT_H

#if !defined(__GNUC__) && !defined(__attribute__)
#define __attribute__(x)
#endif

// Runtime assertions

#if !TARGET_PSP
/* PSPSDK's own <assert.h> (pulled in transitively by some pspsdk headers)
 * declares __assert with a different, incompatible signature. ASSERT is a
 * no-op on PSP anyway (DEBUG_FEATURES=0), so nothing ever calls this --
 * just skip the conflicting declaration rather than chase the exact
 * PSPSDK include path that pulls its version in. */
__attribute__((noreturn)) void __assert(const char* assertion, const char* file, int line);
#endif

// assert for matching
#ifndef NDEBUG
# ifndef NON_MATCHING
#  define ASSERT(cond, msg, file, line) ((cond) ? ((void)0) : __assert(msg, file, line))
# else
#  define ASSERT(cond, msg, file, line) ((cond) ? ((void)0) : __assert(#cond, __FILE__, __LINE__))
# endif
#else
# define ASSERT(cond, msg, file, line) ((void)0)
#endif

// standard assert macro
#ifndef NDEBUG
# define assert(cond) ASSERT(cond, #cond, __FILE__, __LINE__)
#else
# define assert(cond) ((void)0)
#endif

// Static/compile-time assertions

#if !defined(__sgi) && (__GNUC__ >= 5 || __STDC_VERSION__ >= 201112L)
# define static_assert(cond, msg) _Static_assert(cond, msg)
#else
# ifndef GLUE
#  define GLUE(a, b) a##b
# endif
# ifndef GLUE2
#  define GLUE2(a, b) GLUE(a, b)
# endif

# define static_assert(cond, msg) typedef char GLUE2(static_assertion_failed, __LINE__)[(cond) ? 1 : -1]
#endif

#endif
