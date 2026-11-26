# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression coverage for public class overload emission."""
import ast
import unittest

from stubgen.class_merge import PublicClassStubTransformer


def transform(source):
    tree = ast.parse(source)
    transformer = PublicClassStubTransformer('Example', 'Example', {}, set(), set())
    return transformer.visit(tree)


class OverloadEmissionTests(unittest.TestCase):
    def test_flatten_multiple_and_keep_documentation_and_decorators(self):
        tree = transform('''
class Example:
    if TYPE_CHECKING:
        @overload
        @staticmethod
        def f(x: int) -> int: ...
        @overload
        @staticmethod
        def f(x: str) -> str: ...
    @staticmethod
    def f(*args):
        """Method documentation."""
        ...
    def untouched(self, x): ...
''')
        body = tree.body[0].body
        self.assertEqual([n.name for n in body], ['f', 'f', 'untouched'])
        self.assertEqual(ast.get_docstring(body[0]), 'Method documentation.')
        self.assertEqual([ast.unparse(d) for d in body[0].decorator_list], ['overload', 'staticmethod'])
        self.assertEqual(ast.unparse(body[1].args), 'x: str')

    def test_single_identical_collapses_and_async_groups_work(self):
        tree = transform('''
class Example:
    @overload
    def f(self, x: int = 0) -> int: ...
    def f(self, x: int = 0) -> int:
        """kept"""
        ...
    @overload
    async def g(self, x: int) -> int: ...
    @overload
    async def g(self, x: str) -> str: ...
    async def g(self, x): ...
''')
        f, a, b = tree.body[0].body
        self.assertEqual(f.decorator_list, [])
        self.assertEqual(ast.get_docstring(f), 'kept')
        self.assertIsInstance(a, ast.AsyncFunctionDef)
        self.assertIsInstance(b, ast.AsyncFunctionDef)

    def test_ambiguous_single_and_different_decorators_remain(self):
        for source in ('''class C:
    @overload
    def f(self, x: int) -> int: ...
    def f(self, x: str) -> str: ...
''', '''class C:
    @overload
    @staticmethod
    def f(x: int) -> int: ...
    @overload
    @staticmethod
    def f(x: str) -> str: ...
    def f(self, x): ...
'''):
            with self.subTest(source=source):
                original = ast.parse(source).body[0].body
                result = transform(source).body[0].body
                self.assertEqual(len(result), len(original))
                self.assertTrue(any(n.decorator_list for n in result))

    def test_nested_and_separate_groups_do_not_cross_scope(self):
        tree = transform('''
class Outer:
    class Inner:
        @overload
        def f(self, x: int) -> int: ...
        @overload
        def f(self, x: str) -> str: ...
        def f(self, x): ...
    def f(self): ...
''')
        outer = tree.body[0]
        self.assertEqual(len(outer.body[0].body), 2)
        self.assertEqual(len(outer.body), 2)

if __name__ == '__main__':
    unittest.main()
