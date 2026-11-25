# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

import ast
import tempfile
from pathlib import Path
import sys
import unittest

TYPING_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TYPING_DIR))

from stubgen.class_merge import (append_class_stubs, public_class_stub_source, public_import_target_index)  # noqa: E402
from stubgen.module_merge import merge_type_class_support_nodes, module_support_source  # noqa: E402
from stubgen.source_inputs import parse_binding_class_file  # noqa: E402
from stubgen.model import BindingClass  # noqa: E402


class ClassMergeTests(unittest.TestCase):
    def test_module_support_import_does_not_hide_real_sketch_binding(self):
        root = TYPING_DIR.parents[2]
        binding = root / "src/Mod/Sketcher/App/SketchObject.pyi"
        klass = parse_binding_class_file(root, binding, {})[0]
        targets = public_import_target_index([klass])
        source = (root / "src/Mod/Sketcher/App/Sketcher.module.pyi").read_text(
            encoding="utf-8"
        )
        support = module_support_source(source, "Sketcher", targets)
        self.assertNotIn("from Sketcher.App.SketchObject import SketchObject", support)
        self.assertIn("sketch: SketchObject", support)
        with tempfile.TemporaryDirectory() as temp_dir:
            out = Path(temp_dir)
            target = out / "Sketcher" / "__init__.pyi"
            target.parent.mkdir()
            target.write_text(support, encoding="utf-8")
            append_class_stubs(out, root, [klass], {"Sketcher"})
            result = target.read_text(encoding="utf-8")
        tree = ast.parse(result)
        sketch = next(node for node in tree.body
                      if isinstance(node, ast.ClassDef) and node.name == "SketchObject")
        self.assertTrue(any(isinstance(node, ast.FunctionDef) and node.name == "addGeometry"
                            for node in sketch.body))

    def test_module_support_keeps_other_imports_and_aliases(self):
        source = "from Sketcher.App.SketchObject import SketchObject, Other\n" \
                 "from Sketcher.App.SketchObject import SketchObject as Alias\n"
        targets = {("Sketcher.App.SketchObject", "SketchObject"): ("Sketcher", "SketchObject")}
        result = module_support_source(source, "Sketcher", targets)
        self.assertIn("import Other", result)
        self.assertIn("import SketchObject as Alias", result)
        self.assertNotIn("import SketchObject,", result)

    def test_merges_source_adjacent_bases_without_class_members(self):
        target = """\
class Derived:
    ...
"""
        support = """\
class Derived(Base):
    ...
"""
        support_class = ast.parse(support).body[0]
        self.assertIsInstance(support_class, ast.ClassDef)

        merged = merge_type_class_support_nodes(
            target,
            "Derived",
            "",
            class_bases=support_class.bases,
        )

        self.assertIn("class Derived(Base):", merged)

    def test_preserves_property_getters_and_setters(self):
        source = """\
from typing import Sequence

class Binding:
    @property
    def A(self) -> tuple[float, ...]: ...

    @A.setter
    def A(self, value: Sequence[float]) -> None: ...
"""
        klass = BindingClass(
            source="src/Base/Binding.pyi",
            line=3,
            class_name="Binding",
            export_name="BindingPy",
            python_name=None,
            public_names=["FreeCAD.Base.Binding"],
            base_class=None,
            explicit_export=False,
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            path = root / klass.source
            path.parent.mkdir(parents=True)
            path.write_text(source, encoding="utf-8")

            rendered = public_class_stub_source(
                root,
                klass,
                "FreeCAD.Base",
                {},
                {"Binding"},
                {},
                set(),
            )

        assert rendered is not None
        self.assertIn("@property", rendered.source)
        self.assertIn("def A(self) -> tuple[float, ...]", rendered.source)
        self.assertIn("@A.setter", rendered.source)
        self.assertIn("def A(self, value: Sequence[float]) -> None", rendered.source)


if __name__ == "__main__":
    unittest.main()
