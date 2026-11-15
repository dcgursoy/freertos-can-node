/* SPDX-License-Identifier: MIT */
/*
 * Deliberately wrong code. It is never compiled into anything.
 *
 * scripts/static-analysis.sh runs each analyser on this file and requires it to report the
 * defect planted for it. An analysis that says "no findings" proves nothing unless the same
 * invocation is known to find something; this file is that proof, checked on every run.
 */
#include <stddef.h>

/* cppcheck must report: arrayIndexOutOfBounds */
static int planted_out_of_bounds(void)
{
    int values[4] = {0, 1, 2, 3};

    return values[4];
}

/* the MISRA addon must report: misra-c2012-15.6 (the body of an if is not a compound statement) */
static int planted_missing_braces(int x)
{
    if (x > 0)
        return 1;

    return 0;
}

/* clang-tidy must report: clang-analyzer-core.NullDereference */
static int planted_null_dereference(void)
{
    const int *pointer = NULL;

    return *pointer;
}

int planted_entry(int x);
int planted_entry(int x)
{
    return planted_out_of_bounds() + planted_missing_braces(x) + planted_null_dereference();
}
