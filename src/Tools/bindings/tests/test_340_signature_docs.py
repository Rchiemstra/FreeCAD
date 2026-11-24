# SPDX-License-Identifier: LGPL-2.1-or-later
import ast
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from model.generateModel_Python import Function

class SignatureDocs(unittest.TestCase):
    def group(self, source):
        nodes = [n for n in ast.parse(source).body if isinstance(n, ast.FunctionDef)]
        f = Function(nodes[0])
        for node in nodes[1:]: f.update(node)
        return f

    def test_typing_overload_documents_generic_implementation(self):
        f = self.group('@typing_only\n@overload\ndef f(self, x: int) -> None: ...\ndef f(self, **kwargs) -> None: ...')
        self.assertIn('x: int', f.annotated_signatures[0].annotated_text)
        self.assertIn('**kwargs', f.doc_signatures[0].text)
        self.assertTrue(f.has_keywords)
        self.assertTrue(f.parse_signature.generic_kwargs)

    def test_typing_only_and_ordinary_cases(self):
        f = self.group('@typing_only\n@overload\ndef f(self, x: int): ...')
        self.assertTrue(f.typing_only_flag)
        f = self.group('@typing_only\n@overload\ndef f(self, x: int): ...\ndef f(self, x: str): ...')
        self.assertIn('x: str', f.annotated_signatures[0].annotated_text)
        f = self.group('@overload\ndef f(self, x: int): ...\ndef f(self, **kwargs): ...')
        self.assertIn('x: int', f.annotated_signatures[0].annotated_text)

if __name__ == '__main__': unittest.main()
