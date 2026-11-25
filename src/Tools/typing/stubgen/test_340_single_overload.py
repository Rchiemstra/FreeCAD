# SPDX-License-Identifier: LGPL-2.1-or-later
import ast
import unittest
from stubgen.class_merge import PublicClassStubTransformer


def transform(source):
    tree = ast.parse(source)
    return ast.unparse(PublicClassStubTransformer('Part', 'C', {}, set(), set()).visit(tree))


class SingleOverloadTests(unittest.TestCase):
    def test_marked_generic_keeps_typed_signature_and_docs(self):
        result = transform('''class C:
 @typing_only
 @overload
 def f(self, x: int) -> None: ...
 def f(self, **kwargs) -> None:
  """docs"""
  ...
''')
        self.assertNotIn('@overload', result)
        self.assertNotIn('**kwargs', result)
        self.assertIn('x: int', result)
        self.assertIn('docs', result)

    def test_unmarked_or_typed_implementation_is_unchanged(self):
        for decorators, implementation in [('@overload', '**kwargs'), ('@typing_only\n @overload', 'x: str')]:
            with self.subTest(implementation=implementation):
                result = transform(f'class C:\n {decorators}\n def f(self, x: int) -> None: ...\n def f(self, {implementation}) -> None: ...')
                self.assertIn('@overload', result)
                self.assertIn(implementation, result)

class CombinedOverlapTests(unittest.TestCase):
    def test_inline_partially_overlapping_variants_survive(self):
        result = transform('''class C:
 @overload
 def f(self, x: int | str, y: int) -> int: ...
 @overload
 def f(self, x: str | bytes, y: str) -> str: ...
 def f(self, *args): ...
''')
        tree = ast.parse(result).body[0]
        self.assertEqual(len(tree.body), 2)
        self.assertEqual([ast.unparse(n.args) for n in tree.body],
                         ['self, x: int | str, y: int', 'self, x: str | bytes, y: str'])
        self.assertEqual([ast.unparse(n.returns) for n in tree.body], ['int', 'str'])

    def test_marked_generic_with_conflicting_return_is_not_dropped(self):
        result = transform('''class C:
 @typing_only
 @overload
 def f(self, x: int) -> int: ...
 def f(self, **kwargs) -> str: ...
''')
        self.assertIn('-> int', result)
        self.assertIn('**kwargs) -> str', result)
        self.assertIn('@overload', result)

    def test_unannotated_returns_do_not_crash(self):
        result = transform("class C:\n @typing_only\n @overload\n def f(self, x: int): ...\n def f(self, **kwargs): ...")
        self.assertIn("x: int", result)
        self.assertNotIn("**kwargs", result)
