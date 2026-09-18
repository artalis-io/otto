#!/usr/bin/env python3
"""
Every symbol a CLAUDE.md names must exist, or be listed here with a reason.

These files are instructions. They are the first thing a new contributor or an
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

Two shapes are checked, because they are the two that can be decided
mechanically: a function-like `name(` whose prefix looks like a module symbol,
and an ALL_CAPS constant. Prose is left alone.

One rule exists because writing this check produced a false positive worth
remembering. A first pass reported 39 documented environment variables as
dead -- CARTA_RATE_LIMIT_RPS, LOCUS_THREADS, FUELWISE_PORT and so on -- because
it looked for getenv("CARTA_RATE_LIMIT_RPS") and found nothing. Those variables
work: sh_args_load_env() builds the name at runtime from a per-module prefix,
so the literal string is never in the source. So an ALL_CAPS name that starts
with one of those prefixes is also accepted when the remainder exists. The
prefixes are read out of sh_args.c rather than repeated here, so the two cannot
drift apart.

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
SKIP_DIRS = ('vendor/',)

SOURCE_EXT = ('.c', '.h', '.py', '.js', '.mjs', '.sh', '.mk', '.rs', '.yml')
SOURCE_NAMES = ('Makefile',)

# A function-like mention: two-to-six character prefix, underscore, more.
# Narrow on purpose -- it matches sg_solve(, lc_reverse(, mg_match(, and not
# prose like "the parser(".
FUNC = re.compile(r'\b([a-z][a-z0-9]{1,5}_[a-z0-9_]{2,})\s*\(')
CONST = re.compile(r'\b([A-Z][A-Z0-9]{1,}_[A-Z0-9_]{2,})\b')
# A documented family: `VL_TRUCK_SPEED_*`. The name keeps its trailing
# underscore so exists() can tell it apart from a plain constant.
GLOB = re.compile(r'\b([A-Z][A-Z0-9]{1,}_[A-Z0-9_]*_)\*')

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


def source_identifiers():
    """Every identifier that appears anywhere in the tree's sources."""
    seen = set()
    me = os.path.abspath(__file__)
    for root, dirs, files in os.walk('.'):
        dirs[:] = [d for d in dirs
                   if d not in ('.git', 'node_modules', 'build', 'site', 'docs')]
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


def docs():
    out = subprocess.run(['git', 'ls-files', '*CLAUDE.md'],
                         capture_output=True, text=True).stdout.split()
    return [d for d in sorted(out)
            if not any(d.startswith(p) for p in SKIP_DIRS)]


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

    files = docs()
    used_allow = set()
    failures = []

    log('')
    log('%-46s %s' % ('document', 'symbols named but absent'))
    log('-' * 92)

    for d in files:
        body = open(d, encoding='utf-8', errors='replace').read()
        missing = set()
        for name in FUNC.findall(body):
            if not exists(name):
                missing.add(name + '()')
        for name in CONST.findall(body) + GLOB.findall(body):
            if not exists(name):
                missing.add(name)

        allowed = {s for s in missing if s in ALLOWLIST}
        used_allow |= allowed
        bad = sorted(missing - allowed)
        if bad:
            failures += ['%s: %s' % (d, s) for s in bad]
        log('%-46s %s' % (d, ' '.join(bad) or '-'))

    log('-' * 92)

    # An allowlist entry nothing matches any more is stale: it would silence a
    # future mistake with the same name and nobody would know.
    stale = sorted(set(ALLOWLIST) - used_allow)
    if stale:
        print()
        print('Stale ALLOWLIST entries (nothing in any document names these):')
        for s in stale:
            print('    %-28s %s' % (s, ALLOWLIST[s]))
        print('Remove them, so the allowlist keeps meaning what it says.')

    if failures or stale:
        print()
        print('FAIL: %d symbols named by a document but absent from the tree, '
              '%d stale allowlist entries.' % (len(failures), len(stale)))
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

    print('OK: every symbol named by a CLAUDE.md exists, or is allowlisted '
          'with a reason.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
