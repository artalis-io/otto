/*
 * test_regex_differential.c - the vendored regex engine against the system one.
 *
 * Nexus compiles validation rules and transform patterns with the engine in
 * vendor/tre on every platform, including the ones whose libc has a perfectly
 * good <regex.h>. That is a deliberate choice -- a rule written by a customer
 * has to mean the same thing wherever the pipeline runs -- but it is only worth
 * anything if the vendored engine actually agrees with the POSIX one.
 *
 * So: run both over the same corpus and compare. Not just match/no-match, which
 * is the easy half, but every capture offset, because nx_xform.c slices virtual
 * columns out of those offsets and an off-by-one there is a silently wrong field
 * rather than a failure.
 *
 * On platforms with no POSIX <regex.h> -- Windows, which is why the vendoring
 * happened -- there is nothing to compare against and the test reports a skip.
 * That is not a gap: the vendored engine is the only engine there, and the
 * other suites cover it.
 */

#include <stdio.h>
#include <string.h>

#include "tre_regex.h"

/* The system engine exists on Linux and macOS; on Windows it does not. */
#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
  #define HAVE_POSIX_REGEX 1
  #include <regex.h>
#else
  #define HAVE_POSIX_REGEX 0
#endif

#define MAX_GROUPS 10

static int checks = 0;
static int failures = 0;

#if HAVE_POSIX_REGEX

struct case_t {
    const char *pattern;
    int         cflags_extended;  /* every rule nexus compiles is an ERE */
    int         icase;
    const char *input;
};

/*
 * Patterns nexus actually compiles come from two places: validation rules
 * ("^[1-9][0-9]{3}$" and friends, anchored and whole-string) and transform
 * match rules, which capture ("^([0-9]{4}) (.+)$"). The rest of this corpus is
 * the parts of ERE those two shapes are built from, plus the cases where
 * implementations traditionally disagree: empty matches, greedy vs leftmost
 * alternation, backtracking into a bounded repeat, and anchors that can match
 * an empty span.
 * Deliberately absent: (a*)*, (a*)+ against an empty match, and a.*?b. POSIX
 * leaves the captured span of a sub-expression that matched empty on a repeat
 * unspecified, and it leaves a repetition applied to a repetition undefined
 * outright. Engines differ there legitimately, so asserting on them would make
 * this a musl-versus-glibc conformance suite rather than a check that nexus's
 * patterns mean one thing. Every case below has one correct answer.
 *
 * Also absent, and for a different reason: \d and \D. Both engines carry
 * extensions beyond POSIX and mostly the same ones -- \w, \s, \S, \b and
 * \< behave identically -- but GNU has no \d, so there it is just a literal
 * 'd'. TRE has it. That divergence is real and known, and pinning the vendored
 * behaviour is what run_intrinsic() below does; comparing it here would only
 * assert that the two engines differ, which they do.
 */
static const struct case_t cases[] = {
    /* Real nexus patterns, real-shaped inputs. */
    { "^[1-9][0-9]{3}$",            1, 0, "1094" },
    { "^[1-9][0-9]{3}$",            1, 0, "0094" },
    { "^[1-9][0-9]{3}$",            1, 0, "10945" },
    { "^[1-9][0-9]{3}$",            1, 0, "" },
    { "^([0-9]{4}) (.+)$",          1, 0, "1094 Budapest, Ferenc korut 1." },
    { "^([0-9]{4}) (.+)$",          1, 0, "1094 " },
    { "^([0-9.]+) ([0-9.]+)$",      1, 0, "47.4979 19.0402" },
    { "^([0-9.]+) ([0-9.]+)$",      1, 0, "47.4979  19.0402" },
    { "GLS CsomagPontok",           1, 0, "depot: GLS CsomagPontok (HU)" },
    { "Page Header",                1, 0, "no such thing here" },

    /* Anchoring. */
    { "^abc$",                      1, 0, "abc" },
    { "^abc$",                      1, 0, "xabc" },
    { "^",                          1, 0, "abc" },
    { "$",                          1, 0, "abc" },
    { "^$",                         1, 0, "" },
    { "^$",                         1, 0, "a" },

    /* Alternation: POSIX wants leftmost-longest, not first-match. */
    { "(a|ab)",                     1, 0, "abc" },
    { "(ab|a)",                     1, 0, "abc" },
    { "^(a|ab)(c|bcd)$",            1, 0, "abcd" },
    { "(foo|foobar)baz",            1, 0, "foobarbaz" },

    /* Repetition, including the empty-match corner. */
    { "a*",                         1, 0, "" },
    { "a*",                         1, 0, "aaa" },
    { "a+",                         1, 0, "baaa" },
    { "a?b",                        1, 0, "b" },
    { "^(a{2,3})(a*)$",             1, 0, "aaaaa" },
    { "^a{0}b$",                    1, 0, "b" },
    { "x{1,255}",                   1, 0, "xxxx" },

    /* Groups that do not participate must report -1, not 0. */
    { "^(a)|(b)$",                  1, 0, "b" },
    { "^(x)?(y)$",                  1, 0, "y" },
    { "((a)(b))(c)",                1, 0, "abc" },
    { "(a)(b)(c)(d)(e)(f)(g)(h)(i)",1, 0, "abcdefghi" },

    /* Bracket expressions, ranges, negation, and the literal-] rule. */
    { "[abc]+",                     1, 0, "cabbage" },
    { "[^abc]+",                    1, 0, "cabbage" },
    { "[]a]+",                      1, 0, "]a]b" },
    { "[a-]+",                      1, 0, "-a-" },
    { "[0-9]{2,}",                  1, 0, "a1234b" },
    { "[[:digit:]]+",               1, 0, "abc123" },
    { "[[:alpha:]][[:alnum:]]*",    1, 0, "9lives" },
    { "[[:space:]]+",               1, 0, "a \t b" },

    /* Dot, escapes, and metacharacters made literal. */
    { ".",                          1, 0, "\n" },
    { "a.c",                        1, 0, "abc" },
    { "a\\.c",                      1, 0, "abc" },
    { "a\\.c",                      1, 0, "a.c" },
    { "\\$[0-9]+",                  1, 0, "cost $42" },
    { "\\(x\\)",                    1, 0, "(x)" },

    /* Case-insensitive, which validation rules can ask for. */
    { "^hello$",                    1, 1, "HeLLo" },
    { "[a-f]+",                     1, 1, "XYZabcDEF" },

    /* Longest-match rules on overlapping candidates. */
    { "a.*b",                       1, 0, "axbxb" },
    { "(a+)(a+)",                   1, 0, "aaaa" },
};

/* One comparison. Returns 1 if the two engines agreed. */
static int compare_one(const struct case_t *c)
{
    tre_regex_t vre;
    regex_t     sre;
    tre_regmatch_t vm[MAX_GROUPS];
    regmatch_t     sm[MAX_GROUPS];
    int vc_flags = 0, sc_flags = 0;
    int vrc, src_, vex, sex;
    int i, ok = 1;

    if (c->cflags_extended) { vc_flags |= TRE_REG_EXTENDED; sc_flags |= REG_EXTENDED; }
    if (c->icase)           { vc_flags |= TRE_REG_ICASE;    sc_flags |= REG_ICASE; }

    vrc  = tre_regcomp(&vre, c->pattern, vc_flags);
    src_ = regcomp(&sre, c->pattern, sc_flags);

    /* Both must agree on whether the pattern is even valid. */
    if ((vrc == 0) != (src_ == 0)) {
        printf("  FAIL compile /%s/: vendored=%d system=%d\n",
               c->pattern, vrc, src_);
        if (vrc == 0) tre_regfree(&vre);
        if (src_ == 0) regfree(&sre);
        return 0;
    }
    if (vrc != 0) return 1;  /* both rejected it; nothing more to compare */

    /* And on how many groups the pattern has. */
    if (vre.re_nsub != sre.re_nsub) {
        printf("  FAIL nsub /%s/: vendored=%zu system=%zu\n",
               c->pattern, vre.re_nsub, sre.re_nsub);
        ok = 0;
    }

    for (i = 0; i < MAX_GROUPS; i++) {
        vm[i].rm_so = vm[i].rm_eo = -2;
        sm[i].rm_so = sm[i].rm_eo = -2;
    }

    vex = tre_regexec(&vre, c->input, MAX_GROUPS, vm, 0);
    sex = regexec(&sre, c->input, MAX_GROUPS, sm, 0);

    if ((vex == 0) != (sex == 0)) {
        printf("  FAIL match /%s/ on \"%s\": vendored=%s system=%s\n",
               c->pattern, c->input,
               vex == 0 ? "match" : "no-match",
               sex == 0 ? "match" : "no-match");
        ok = 0;
    } else if (vex == 0) {
        /* Compare every span, including the ones that did not participate. */
        size_t ngroups = vre.re_nsub + 1;
        if (ngroups > MAX_GROUPS) ngroups = MAX_GROUPS;

        for (i = 0; i < (int)ngroups; i++) {
            if (vm[i].rm_so != (tre_regoff_t)sm[i].rm_so ||
                vm[i].rm_eo != (tre_regoff_t)sm[i].rm_eo) {
                printf("  FAIL group %d /%s/ on \"%s\": "
                       "vendored=[%ld,%ld) system=[%ld,%ld)\n",
                       i, c->pattern, c->input,
                       (long)vm[i].rm_so, (long)vm[i].rm_eo,
                       (long)sm[i].rm_so, (long)sm[i].rm_eo);
                ok = 0;
            }
        }
    }

    tre_regfree(&vre);
    regfree(&sre);
    return ok;
}

static void run_differential(void)
{
    size_t n = sizeof(cases) / sizeof(cases[0]);
    size_t i;

    printf("\nVendored vs system POSIX regex (%zu cases):\n", n);
    for (i = 0; i < n; i++) {
        checks++;
        if (!compare_one(&cases[i])) failures++;
    }
    printf("  %d/%d cases agreed\n", checks - failures, checks);
}

#endif /* HAVE_POSIX_REGEX */

/*
 * A handful of assertions that hold regardless of what the platform provides,
 * so this file still tests something where there is no system engine to
 * compare against.
 */
static void check(int cond, const char *what)
{
    checks++;
    if (!cond) { printf("  FAIL %s\n", what); failures++; }
}

static void run_intrinsic(void)
{
    tre_regex_t re;
    tre_regmatch_t m[4];
    char buf[64];

    printf("\nVendored engine, self-contained checks:\n");

    check(tre_regcomp(&re, "^([0-9]{4}) (.+)$", TRE_REG_EXTENDED) == TRE_REG_OK,
          "compiles a transform pattern");
    check(re.re_nsub == 2, "reports two groups");
    check(tre_regexec(&re, "1094 Budapest", 4, m, 0) == TRE_REG_OK, "matches");
    check(m[1].rm_so == 0 && m[1].rm_eo == 4, "group 1 spans the postcode");
    check(m[2].rm_so == 5 && m[2].rm_eo == 13, "group 2 spans the rest");
    check(tre_regexec(&re, "94 Budapest", 4, m, 0) == TRE_REG_NOMATCH,
          "rejects a three-digit postcode");
    tre_regfree(&re);

    /* NOSUB is what nx_validate.c compiles with; it must still match. */
    check(tre_regcomp(&re, "^[1-9][0-9]{3}$",
                      TRE_REG_EXTENDED | TRE_REG_NOSUB) == TRE_REG_OK,
          "compiles a validation rule with NOSUB");
    check(tre_regexec(&re, "1094", 0, NULL, 0) == TRE_REG_OK, "NOSUB matches");
    check(tre_regexec(&re, "0094", 0, NULL, 0) == TRE_REG_NOMATCH,
          "NOSUB rejects");
    tre_regfree(&re);

    /*
     * nexus/CLAUDE.md documents "^(\d{4}) (.+)$" as a transform pattern, and
     * TRE reads \d as a digit class. GNU's engine does not have \d at all and
     * reads it as a literal 'd', so on Linux that documented pattern used to
     * match nothing useful. Vendoring makes the documented meaning the real one
     * on every platform; these pin it.
     */
    check(tre_regcomp(&re, "^(\\d{4}) (.+)$", TRE_REG_EXTENDED) == TRE_REG_OK,
          "compiles the documented backslash-d pattern");
    check(tre_regexec(&re, "1094 Budapest", 4, m, 0) == TRE_REG_OK,
          "backslash-d matches digits");
    check(tre_regexec(&re, "dddd Budapest", 4, m, 0) == TRE_REG_NOMATCH,
          "backslash-d is not a literal d");
    tre_regfree(&re);

    check(tre_regcomp(&re, "a(", TRE_REG_EXTENDED) == TRE_REG_EPAREN,
          "unbalanced paren is EPAREN");
    check(tre_regcomp(&re, "[z-a]", TRE_REG_EXTENDED) == TRE_REG_ERANGE,
          "inverted range is ERANGE");

    check(tre_regerror(TRE_REG_NOMATCH, NULL, buf, sizeof buf)
              == strlen("No match") + 1,
          "regerror reports the length it needs");
    check(strcmp(buf, "No match") == 0, "regerror writes the message");

    /* Truncation must still NUL-terminate. */
    check(tre_regerror(TRE_REG_ESPACE, NULL, buf, 4) == strlen("Out of memory") + 1,
          "regerror reports the untruncated length");
    check(strcmp(buf, "Out") == 0, "regerror truncates and terminates");
}

int main(void)
{
    printf("\nRegex Engine Tests:\n");

    run_intrinsic();

#if HAVE_POSIX_REGEX
    run_differential();
#else
    printf("\nVendored vs system POSIX regex:\n");
    printf("  SKIP - this platform has no <regex.h> to compare against,\n");
    printf("         which is the reason the engine is vendored.\n");
#endif

    printf("\n  %d/%d checks passed\n\n", checks - failures, checks);
    return failures != 0;
}
