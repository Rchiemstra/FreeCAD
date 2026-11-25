# SPDX-License-Identifier: LGPL-2.1-or-later
"""Preserve abstractness in generated public class stubs."""
import ast
import unittest

from stubgen.class_merge import PublicClassStubTransformer


class AbstractDecoratorTests(unittest.TestCase):
    def test_abstractmethod_survives_typing_only(self):
        tree = ast.parse('''class Example:
    @typing_only
    @abstractmethod
    def __init__(self) -> None: ...
''')
        result = PublicClassStubTransformer('Example', 'Example', {}, set(), set()).visit(tree)
        self.assertEqual([ast.unparse(d) for d in result.body[0].body[0].decorator_list], ['abstractmethod'])


if __name__ == '__main__':
    unittest.main()
