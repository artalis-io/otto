#!/usr/bin/env python3
"""
Every symbol a descriptive document names must exist, or be listed here with
a reason.

The CLAUDE.md files are instructions. They are the first thing a new contributor or an
agent session reads, and unlike the code they describe, nothing compiles them --
so they rot silently and keep giving confident directions to a tree that has
moved on.

Three that were live when this check was written:

  - locus/CLAUDE.md documented a feature-class table naming LC_CLASS_PLACE,
    LC_CLASS_ADMIN, LC_CLASS_NATURAL and LC_CLASS_TRANSPORT. None of the four
    were ever in the enum, which has sixteen other classes. The same file's
    search example read entity->coord, a field called centroid.

  - fuelwise/api/CLAUDE.md's "how to add an endpoint" section was written
    against Mongoose -- ev_handler(), mg_match(), mg_str(). Mongoose was
    replaced by Keel; following those steps produces code that will not
    compile.

  - ralph/CLAUDE.md said models are freed with ralph_free(). The function is
    ralph_core_free().

Two shapes are checked for existence, because they are the two that can be
decided mechanically: a function-like `name(` whose prefix looks like a module
symbol, and an ALL_CAPS constant. Prose is left alone.

A name that exists is then checked for arity: a document that calls or declares
a function with the wrong number of arguments is wrong in a way a reader only
discovers at the compiler. Names alone were not enough -- twice in a row a
correction to a name left a call that could not compile. The MANIFESTO's
`sh_arena_create(&arena, buffer, size)` passed the name check while the
function takes a capacity and returns the arena, and
`cs_tui_render_clay_commands(r, commands)` was introduced by a rename in the
very commit that added this check's first half, against a function taking
three arguments.

Only calls that pass at least one argument are compared. `foo()` in a document
names a function rather than calling it with nothing, and that idiom is most of
what a document does: requiring it to match dropped the reading from 195
complaints to 7, of which 6 were real.

One rule exists because writing this check produced a false positive worth
remembering. A first pass reported 39 documented environment variables as
dead -- CARTA_RATE_LIMIT_RPS, LOCUS_THREADS, FUELWISE_PORT and so on -- because
it looked for getenv("CARTA_RATE_LIMIT_RPS") and found nothing. Those variables
work: sh_args_load_env() builds the name at runtime from a per-module prefix,
so the literal string is never in the source. So an ALL_CAPS name that starts
with one of those prefixes is also accepted when the remainder exists. The
prefixes are read out of sh_args.c rather than repeated here, so the two cannot
drift apart.

What is covered is every CLAUDE.md plus the documents under docs/ that
describe the system as it is. Three directories are left out on principle,
because holding them to it would be wrong rather than merely noisy:

  - docs/archive/ is a frozen record of superseded plans. It is supposed to
    name things that no longer exist.
  - docs/roadmaps/ proposes work not yet done. hose.md describes a module
    that does not exist; that is the point of it.
  - docs/business/ is strategy, and carries redaction markers.

Measured before choosing that line: over everything, 1576 symbols across 111
files, of which docs/archive/carta/TODO_FEATURES.md alone accounts for 104.
Over the descriptive set, 49 across 10 files -- a set small enough to read,
which is what makes the difference between a gate and a wall of noise.

Individual files can be excluded too, with the reason next to them.

Exclusions are not forbidden; they have to be written down. Usage:

    python3 scripts/check_doc_symbols.py [--quiet]
"""
import os
import re
import sys
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

QUIET = '--quiet' in sys.argv

# Third-party docs describe third-party trees; they are not ours to police.
# The docs/ entries are explained in the module docstring above.
SKIP_DIRS = (
    'vendor/',
    'docs/archive/',
    'docs/roadmaps/',
    'docs/business/',
)

# Documents that are plans rather than descriptions. Keep the reason.
SKIP_FILES = {
    'docs/TOOLING.md':
        'a phased plan -- "Add to root Makefile", "Create shared/include/sh_log.h"',
}

SOURCE_EXT = ('.c', '.h', '.py', '.js', '.mjs', '.sh', '.mk', '.rs', '.yml')
SOURCE_NAMES = ('Makefile',)

# A function-like mention: two-to-six character prefix, underscore, more.
# Narrow on purpose -- it matches sg_solve(, lc_reverse(, mg_match(, and not
# prose like "the parser(".
FUNC = re.compile(r'\b([a-z][a-z0-9]{1,5}_[a-z0-9_]{2,})\s*\(')
CONST = re.compile(r'\b([A-Z][A-Z0-9]{1,}_[A-Z0-9_]{2,})\b')
# A backtick-quoted identifier, which is how prose names a function it is not
# calling: "`ralph_optimize_lp` remains as a compatibility dispatcher". Needed
# because FUNC wants a '(' -- without this, two dead names in
# docs/internals/ralph-architecture.md passed the gate.
CODE = re.compile(r'`([a-z][a-z0-9]{1,5}_[a-z0-9_]{2,})`')

# A documented family: `VL_TRUCK_SPEED_*`. The name keeps its trailing
# underscore so exists() can tell it apart from a plain constant.
GLOB = re.compile(r'\b([A-Z][A-Z0-9]{1,}_[A-Z0-9_]*_)\*')

# A declaration or definition at the start of a line: a return type, then the
# name, then the parameter list. Used to learn how many arguments each function
# takes.
DECL = re.compile(r'(?m)^[A-Za-z_][A-Za-z0-9_\s\*]*?\b([a-z][a-z0-9_]{2,})\s*\(')

# Words that are followed by a parenthesis without being a function.
NOT_CALLS = frozenset((
    'if', 'for', 'while', 'switch', 'return', 'sizeof', 'defined', 'do',
    'else', 'case', 'typedef', 'struct', 'union', 'enum', 'static', 'inline',
    'const', 'void', 'int', 'char', 'float', 'double', 'long', 'short',
    'unsigned', 'signed', 'extern', 'register', 'volatile', 'goto', 'break',
    'continue', 'default',
))

# Symbols that are deliberately not real: placeholders in "here is the shape of
# a thing you would write" examples, and words that merely look like constants.
# Keep the reason with the entry.
ALLOWLIST = {
    'my_alloc()': 'placeholder in the custom-allocator example',
    'my_free()': 'placeholder in the custom-allocator example',
    'my_realloc()': 'placeholder in the custom-allocator example',
    'app_init()': 'placeholder in the embedding example',
    'submit_form()': 'placeholder in the event-handler example',
    'wasm_function_name()': 'literally a placeholder for the reader to fill in',
    'select_entering_variable()': 'names a simplex step in prose, not a function',
    'SCREAMING_CASE': 'names a naming convention',
    'CODE_QUALITY': 'section label',
    'CLAY_LAYOUT': 'names Clay upstream macro family in prose',
    'CARTA_LABELS_PLAN': 'names a planning document, not a symbol',
    'UPPER_SNAKE': 'names a naming convention',
    'graph_destroy()': 'illustrates the _create()/_destroy() pairing rule',
    'arena_alloc()': 'stand-in body for the custom-allocator example',
    'render_commands()': 'a method in an ASCII diagram of the JS renderer',
    'handle_new_endpoint()': 'the handler a reader is being told to write',
    'VITE_TILE_SERVER': 'Vite variable for the React frontend, which lives outside this repo',
    'ct_font_load()': 'under a heading marked "(planned)" -- the MSDF C API is unwritten',
    'ct_font_text_width()': 'under a heading marked "(planned)"',
    # Named precisely because they are gone -- the sentence is about the removal.
    'node_queue_free()': 'KNOWN_ISSUES records that it was deleted',
    'RALPH_BIG_M': 'the simplex review records it as removed from the codebase',
    # Pseudocode and notation in explanatory sketches.
    'swap_rows()': 'pseudocode in the LU pivoting sketch',
    'cut_value()': 'mathematical notation in a Benders assertion, not a function',
    'render_track_and_thumb()': 'pseudocode in the widget input-ordering example',
    'output_char()': 'pseudocode in the terminal differential-update example',
    'do_something()': 'placeholder in the error-handling example',
    'log_error()': 'placeholder in the error-handling example',
    # Proposed, deliberately unwritten.
    'dual_simplex_solve_v2_lightweight()':
        'proposed in the review\'s recommendations; the doc says "Add"',
    # Not ours.
    'AF_INET6': 'POSIX, from <sys/socket.h>',
}

# Calls whose argument count is deliberately not the real one. Same rule as
# above: the reason lives with the entry, and an entry that matches nothing
# fails the run.
ARITY_ALLOWLIST = {
    'make_dual_feasible()':
        'a call-graph sketch in the simplex review, written with the tableau alone',
}


def log(*a):
    if not QUIET:
        print(*a)


def api_prefixes():
    """Module prefixes sh_args_load_env() builds environment names from.

    Read from the source rather than listed here: a seventh server would
    otherwise have its documented variables reported as dead.
    """
    path = os.path.join('shared', 'src', 'sh_args.c')
    if not os.path.exists(path):
        return set()
    body = open(path, encoding='utf-8', errors='replace').read()
    m = re.search(r'api_prefixes\[\]\s*=\s*\{(.*?)\}', body, re.S)
    if not m:
        return set()
    return set(re.findall(r'"([A-Z][A-Z0-9_]*)"', m.group(1)))


def submodule_paths():
    """Paths listed in .gitmodules, which may or may not be checked out."""
    out = set()
    if os.path.exists('.gitmodules'):
        body = open('.gitmodules', encoding='utf-8', errors='replace').read()
        out = set(re.findall(r'^\s*path\s*=\s*(\S+)', body, re.M))
    return out


def source_identifiers():
    """Every identifier that appears anywhere in the tree's sources."""
    submodules = submodule_paths()
    seen = set()
    me = os.path.abspath(__file__)
    for root, dirs, files in os.walk('.'):
        dirs[:] = [d for d in dirs
                   if d not in ('.git', 'node_modules', 'build', 'site', 'docs')]
        # Submodules are skipped so the answer cannot depend on whether they
        # are checked out. That cost a red CI run: locally, with vendor/keel
        # absent, AF_INET6 was correctly missing and its allowlist entry was
        # in use; on CI, which checks out submodules recursively, Keel's
        # sockets brought AF_INET6 into the corpus, the entry matched nothing,
        # and a stale entry fails the run. Vendored trees that are checked in
        # -- miniz, TRE, Clay -- stay, because they are always there and the
        # docs cite them (TINFL_STATUS_DONE, RE_DUP_MAX).
        root_rel = os.path.relpath(root, '.').replace(os.sep, '/')
        dirs[:] = [d for d in dirs
                   if ('%s/%s' % (root_rel, d)).lstrip('./') not in submodules]
        for f in files:
            path = os.path.join(root, f)
            # This file names every allowlisted symbol, so counting it as
            # evidence would make each entry prove itself real -- and then
            # report itself stale for never matching anything.
            if os.path.abspath(path) == me:
                continue
            if f.endswith(SOURCE_EXT) or f in SOURCE_NAMES:
                try:
                    body = open(path, encoding='utf-8', errors='ignore').read()
                except OSError:
                    continue
                seen.update(re.findall(r'[A-Za-z_][A-Za-z0-9_]{2,}', body))
    return seen


def balanced(text, open_idx):
    """The text between text[open_idx] == '(' and its matching ')'.

    Counting parentheses rather than matching a regex, because both sides need
    it: an argument can be a call, and a parameter can be a function pointer
    (`void (*cb)(SHLogLevel, const char *, void *)`).
    """
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == '\\' else 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:i]
        i += 1
    return None


def split_args(text):
    """Split an argument or parameter list on its top-level commas."""
    out, depth, cur = [], 0, ''
    for c in text:
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += c
    out.append(cur)
    return [x.strip() for x in out if x.strip()]


def arity_of(params):
    """(count, variadic) for a parameter list. `void` alone means none."""
    parts = split_args(params)
    if parts == ['void']:
        return 0, False
    return len([x for x in parts if x != '...']), any(x == '...' for x in parts)


def declared_arities():
    """name -> {(count, variadic)}. More than one entry means ambiguous.

    Ambiguity is real -- a static helper can share a name across files -- and
    those names are skipped rather than guessed at.
    """
    subs = submodule_paths()
    out = {}
    for root, dirs, files in os.walk('.'):
        dirs[:] = [d for d in dirs
                   if d not in ('.git', 'node_modules', 'build', 'site', 'docs')]
        rel = os.path.relpath(root, '.').replace(os.sep, '/').lstrip('./')
        dirs[:] = [d for d in dirs
                   if ('%s/%s' % (rel, d)).lstrip('/') not in subs]
        for f in files:
            if not f.endswith(('.c', '.h')):
                continue
            try:
                body = open(os.path.join(root, f), encoding='utf-8',
                            errors='ignore').read()
            except OSError:
                continue
            for m in DECL.finditer(body):
                name = m.group(1)
                if name in NOT_CALLS:
                    continue
                inner = balanced(body, m.end() - 1)
                if inner is None:
                    continue
                after = body[m.end() + len(inner) + 1:m.end() + len(inner) + 40]
                if not re.match(r'\s*[;{]', after):
                    continue          # a call or an expression, not a signature
                out.setdefault(name, set()).add(arity_of(inner))
    return out


def docs():
    out = subprocess.run(['git', 'ls-files', '*CLAUDE.md', 'docs/*.md'],
                         capture_output=True, text=True).stdout.split()
    return [d for d in sorted(set(out))
            if not any(d.startswith(p) for p in SKIP_DIRS)
            and d not in SKIP_FILES]


def main():
    known = source_identifiers()
    prefixes = api_prefixes()
    log('%d identifiers in the sources; %d runtime env prefixes (%s)'
        % (len(known), len(prefixes), ', '.join(sorted(prefixes)) or 'none'))

    def exists(sym):
        """Is this name real?

        A prefixed constant counts when the part after a known module prefix
        exists, because sh_args_load_env() assembles those at runtime and the
        whole name is never a literal in the source.
        """
        if sym in known:
            return True
        # `VL_TRUCK_SPEED_*` names a family; the trailing underscore is the
        # glob, not part of a name. Accept it when anything extends it.
        if sym.endswith('_'):
            return any(k.startswith(sym) for k in known)
        head, _, tail = sym.partition('_')
        return bool(tail) and head in prefixes and tail in known

    arities = declared_arities()
    files = docs()
    used_allow = set()
    used_arity_allow = set()
    failures = []
    wrong_arity = []

    log('')
    log('%-46s %s' % ('document', 'symbols named but absent'))
    log('-' * 92)

    for d in files:
        body = open(d, encoding='utf-8', errors='replace').read()
        missing = set()
        for name in FUNC.findall(body) + CODE.findall(body):
            if not exists(name):
                missing.add(name + '()')
        for name in CONST.findall(body) + GLOB.findall(body):
            if not exists(name):
                missing.add(name)

        # Arity, for the names that do exist.
        for m in FUNC.finditer(body):
            name = m.group(1)
            sigs = arities.get(name)
            if not sigs or len(sigs) != 1:
                continue              # unknown, or ambiguous across files
            want, variadic = next(iter(sigs))
            inner = balanced(body, m.end() - 1)
            if inner is None:
                continue
            args = split_args(inner)
            if args == ['void']:
                args = []
            # `foo()` names the function rather than calling it with nothing,
            # and `foo(...)` is an explicit placeholder.
            if not args or any(a == '...' for a in args):
                continue
            if name + '()' in ARITY_ALLOWLIST:
                used_arity_allow.add(name + '()')
                continue
            got = len(args)
            if (got < want) if variadic else (got != want):
                wrong_arity.append(
                    '%s:%d: %s takes %d argument%s%s, given %d'
                    % (d, body[:m.start()].count(chr(10)) + 1, name, want,
                       '' if want == 1 else 's',
                       ' or more' if variadic else '', got))

        allowed = {s for s in missing if s in ALLOWLIST}
        used_allow |= allowed
        bad = sorted(missing - allowed)
        if bad:
            failures += ['%s: %s' % (d, s) for s in bad]
        log('%-46s %s' % (d, ' '.join(bad) or '-'))

    log('-' * 92)

    # An allowlist entry nothing matches any more is stale: it would silence a
    # future mistake with the same name and nobody would know.
    stale = sorted((set(ALLOWLIST) - used_allow)
                   | (set(ARITY_ALLOWLIST) - used_arity_allow))
    if stale:
        print()
        print('Stale ALLOWLIST entries (nothing in any document names these):')
        for s in stale:
            print('    %-28s %s'
                  % (s, ALLOWLIST.get(s) or ARITY_ALLOWLIST.get(s)))
        print('Remove them, so the allowlist keeps meaning what it says.')

    if wrong_arity:
        print()
        print('Called with the wrong number of arguments:')
        for w in wrong_arity:
            print('    %s' % w)
        print('A document that will not compile is worse than one that is')
        print('merely out of date: it looks like something you can paste.')

    if failures or stale or wrong_arity:
        print()
        print('FAIL: %d symbols named by a document but absent from the tree, '
              '%d called with the wrong arity, %d stale allowlist entries.'
              % (len(failures), len(wrong_arity), len(stale)))
        if failures:
            print()
            for f in failures:
                print('    %s' % f)
            print()
            print('Correct the document, or add the symbol to ALLOWLIST in this')
            print('file with the reason it is not real. Instructions that name')
            print('things which do not exist are worse than no instructions:')
            print('they are followed.')
        return 1

    print('OK: every symbol named by a checked document exists and is called '
          'with the right number of arguments, or is allowlisted with a reason.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
