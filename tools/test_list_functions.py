#!/usr/bin/env python3
"""Run with: python3 tools/test_list_functions.py"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import list_functions as tool


class ScannerTests(unittest.TestCase):
    def test_multiline_comments_literals_and_nested_bodies(self):
        source = '''/* void fake(void) { } */
// int fake2(void) { }
#define FAKE(name) int name(void) { \\
    return 1; }
static int
game_real(
    const char *s, /* ) { */
    void (*callback)(int))
{
    const char *text = "} /* fake */ \\" {";
    if (s) { callback(2); }
    return 0;
}
'''
        result = tool.scan(source, "src/game/test.c")
        self.assertEqual([f.name for f in result], ["game_real"])
        self.assertEqual((result[0].line, result[0].end_line), (5, 13))
        self.assertTrue(result[0].static)
        self.assertTrue(result[0].prefix_ok)
        self.assertEqual(result[0].signature,
                         "static int game_real( const char *s, void (*callback)(int))")

    def test_prototypes_aggregates_and_function_pointer_variables(self):
        source = '''
int prototype(void);
typedef void (*Callback)(int);
struct Record { int field; void (*callback)(void); };
static Callback handler = 0;
static void (*handler2)(int) = NULL;
static struct Record values[] = { { .field = 2 } };
static Callback factory(void) { return handler; }
int (*core_make_callback(void))(int) { return 0; }
'''
        self.assertEqual([f.name for f in tool.scan(source, "src/core/test.c")],
                         ["factory", "core_make_callback"])

    def test_attributes_and_declaration_macros(self):
        source = '''
#define API_ATTR(x) __attribute__((x))
#define INLINE_FIRST INLINE_SECOND
#define INLINE_SECOND static inline
INLINE_FIRST int core_inline(void) { return 1; }
static API_ATTR(unused) int core_macro(void) { return 1; }
[[nodiscard("two   spaces")]] int core_attr(void) { return 1; }
__attribute__((unused)) int core_gnu(void) { return 1; }
typeof(other_call()) core_typeof(void) { return 1; }
'''
        result = tool.scan(source, "src/core/test.h")
        self.assertEqual([f.name for f in result],
                         ["core_inline", "core_macro", "core_attr", "core_gnu", "core_typeof"])
        self.assertTrue(result[0].static)
        self.assertTrue(result[0].inline)
        self.assertIn('"two   spaces"', result[2].signature)

    def test_preprocessor_branches_and_prefixes(self):
        source = '''#if 0
int game_branch(void) { return 1; }
#else
int helper(void) { return 2; }
#endif
'''
        result = tool.scan(source, "src/game/test.c")
        self.assertEqual([f.prefix_ok for f in result], [True, False])
        self.assertIsNone(tool.scan("int main(void) {}", "src/main.c")[0].prefix_ok)

    def test_storage_qualifiers_only_come_from_declaration_specifiers(self):
        source = '''
#define PARAM static
#define LOCAL static inline
void game_array(int values[static 4]) {}
void game_macro_array(int values[PARAM 4]) {}
__attribute__((annotate("static"))) void game_attribute(void) {}
LOCAL void game_local(int values[static 4]) {}
#undef LOCAL
#define LOCAL
LOCAL void game_external(void) {}
void game_before_definition(void) {}
#define game_before_definition static
'''
        result = tool.scan(source, "src/game/test.c")
        self.assertEqual([f.static for f in result], [False, False, False, True, False, False])
        self.assertEqual([f.inline for f in result], [False, False, False, True, False, False])

    def test_include_macro_scope_cycles_and_single_file_scan(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src/core").mkdir(parents=True)
            (root / "src/core/types.h").write_text(
                '#include "cycle.h"\n#define CORE_INLINE static inline\n')
            (root / "src/core/cycle.h").write_text('#include "types.h"\n')
            target = root / "src/core/a.c"
            target.write_text(
                '#include "types.h"\nCORE_INLINE int core_local(void) {}\n'
                'int core_unrelated(void) {}\n')
            (root / "src/core/z.c").write_text(
                '#define CORE_INLINE\n#define core_unrelated(x) whatever\n')
            all_files = tool.inventory(root, [root / "src"])
            single_file = tool.inventory(root, [target])
            self.assertEqual(all_files, single_file)
            self.assertEqual([f.name for f in all_files], ["core_local", "core_unrelated"])
            self.assertEqual([f.static for f in all_files], [True, False])

    def test_inventory_and_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src/core").mkdir(parents=True)
            (root / "src/third_party").mkdir()
            (root / "src/core/types.h").write_text("#define CORE_INLINE static inline\n")
            (root / "src/core/test.h").write_text(
                '#include "types.h"\nCORE_INLINE int core_valid(void) { return 1; }\nint bad(void) {}\n')
            (root / "src/third_party/third.h").write_text("int vendored(void) {}\n")
            app = tool.inventory(root, [root / "src"])
            self.assertEqual([f.name for f in app], ["core_valid", "bad"])
            self.assertTrue(app[0].static)
            self.assertEqual(len(tool.inventory(root, [root / "src"], True)), 3)
            invocation = [sys.executable, str(Path(tool.__file__)), "--root", str(root),
                          "--format", "json", "--check-prefix"]
            cli = subprocess.run(invocation, capture_output=True, text=True)
            self.assertEqual(cli.returncode, 1)
            self.assertEqual(len(json.loads(cli.stdout)), 2)
            self.assertIn("1 layer-prefix violations", cli.stderr)
            cli = subprocess.run(invocation + ["--layer", "main"], capture_output=True, text=True)
            self.assertEqual(cli.returncode, 0)
            self.assertEqual(json.loads(cli.stdout), [])


if __name__ == "__main__":
    unittest.main()
