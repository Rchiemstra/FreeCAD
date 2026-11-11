# SPDX-License-Identifier: LGPL-2.1-or-later
"""The core catalogue must discover the shared Python diagnostic presentation."""
import ast
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_core_translation_roots_include_expression_presenters():
    tree = ast.parse((ROOT / "src/Tools/updatets.py").read_text())
    assignment = next(node for node in tree.body if isinstance(node, ast.Assign)
                      and any(isinstance(target, ast.Name) and target.id == "directories"
                              for target in node.targets))
    core = next(entry for entry in ast.literal_eval(assignment.value)
                if entry["tsname"] == "FreeCAD")
    working = ROOT / core["workingdir"]
    roots = [working.resolve()] + [(working / path).resolve()
                                  for path in core.get("extra_roots", [])]
    for name in ("expression_diagnostics_gui.py", "expression_diagnostics_text.py",
                 "expression_diagnostics_property_editor.py", "sketcher_expression_diagnostics.py"):
        source = ROOT / "src/Ext/freecad" / name
        assert any(source.is_relative_to(root) for root in roots), name
        # lupdate cannot extract a variable used as its source-text argument.
        for node in ast.walk(ast.parse(source.read_text())):
            if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                    and node.func.id == "translate"):
                assert len(node.args) >= 2
                assert all(isinstance(arg, ast.Constant) and isinstance(arg.value, str)
                           for arg in node.args[:2]), (name, node.lineno)
