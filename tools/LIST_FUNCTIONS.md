# Function inventory

Requires Python 3.10+ and its standard library; no compiler or parser packages.
Run from any directory:

```sh
python3 tools/list_functions.py
python3 tools/list_functions.py --format json > /tmp/offbeat-functions.json
python3 tools/list_functions.py --layer core
python3 tools/list_functions.py --check-prefix
python3 tools/list_functions.py --include-third-party
python3 tools/list_functions.py src/game/player.c src/core
python3 tools/test_list_functions.py
```

By default this scans every `.c` and `.h` under `src`, excluding `third_party`.
Paths are relative to the repository root, which can be overridden with `--root`.
Output is ordered by file and source position. Headers are included so inline
definitions are counted, and prototypes are omitted. JSON is an array with
`path`, `line`, `end_line`, `layer`, `name`, `signature`, `static`, `inline`, and
`prefix_ok` fields. The text format ends with a count per layer. Signatures retain
their source spelling with whitespace/comments normalized.

`--check-prefix` applies the repository's `core_`, `game_`, and `platform_` rule
to **every** definition in those layers, including static helpers. It exits 1
if any violate the rule; JSON always includes `prefix_ok` for filtering.
`main` and third-party definitions have no required prefix.

The scanner balances braces and declarators, ignores comments and strings,
and handles multiline functions, callback arguments, pointer-return declarators,
attributes, and object macros such as `CORE_INLINE`. Storage qualifiers come
from declaration specifiers; `void f(int array[static 4])` remains external.
Macro context follows literal quoted includes relative to the source file or
`src`, in source order, including `#undef`. Unrelated files do not contribute
macros. Includes contribute macro context even when scanning a single file;
their function definitions are listed only if those files were requested.

This is a **source inventory**, not a C preprocessor: independently balanced
definitions on both sides of `#if` are listed. It does not evaluate conditions,
expand function-like macros, resolve macro-generated includes, read system
includes or use compiler include paths. Macro qualifiers reflect the last source
definition encountered through quoted includes, not a selected build
configuration. Branches that split one declaration/body across alternatives,
generated functions, K&R definitions, GNU nested functions, and nonstandard
declarators beyond the supported forms require a compiler-based inventory.
No build files are generated or inspected.
