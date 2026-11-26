import ast
import unittest
import tempfile
from pathlib import Path

from stubgen.class_merge import PublicClassStubTransformer, public_class_stub_source
from stubgen.cpp_properties import TypedCppProperty, _localize_expression
from stubgen.generator import property_support_names
from stubgen.model import PublicPythonType, BindingClass
from stubgen.property_contracts import AliasSpec, PropertyCatalog


class Names331Tests(unittest.TestCase):
    def test_shadowed_builtin_and_owned_type(self):
        node = ast.parse('class Sheet:\n def set(self, value: set[str]) -> Geometry: ...\n def Geometry(self): ...\n').body[0]
        transformer = PublicClassStubTransformer('Spreadsheet', 'Sheet', {}, set(), set(), {'set': 'builtins', 'Geometry': 'Part'})
        result = ast.unparse(ast.fix_missing_locations(transformer.visit(node)))
        self.assertIn("'builtins.set[str]'", result)
        self.assertIn("'Part.Geometry'", result)
        self.assertEqual(transformer.annotation_module_roots_needed, {'builtins', 'Part'})

    def test_import_owner_is_selected_by_class_assembly(self):
        source = "from Part.App.Geometry import Geometry\nclass Binding:\n def Geometry(self): ...\n def add(self, geo: Geometry) -> set[str]: ...\n def set(self): ...\n"
        klass = BindingClass(source="src/Mod/Demo/App/Binding.pyi", line=2,
                             class_name="Binding", export_name="Binding",
                             python_name=None, public_names=["Demo.Binding"],
                             base_class=None, explicit_export=False)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / klass.source
            path.parent.mkdir(parents=True)
            path.write_text(source)
            rendered = public_class_stub_source(
                root, klass, "Demo", {}, {"Binding"},
                {("Part.App.Geometry", "Geometry"): ("Part", "Geometry")}, {"Part"})
        assert rendered is not None
        self.assertIn("'Part.Geometry'", rendered.source)
        self.assertIn("'builtins.set[str]'", rendered.source)
        self.assertIn("import Part", rendered.import_lines)
        self.assertIn("import builtins", rendered.import_lines)

    def test_base_namespace_cannot_be_shadowed_by_a_class_member(self):
        self.assertEqual(_localize_expression("Base.Placement | Base.Matrix", "Robot"),
                         "FreeCAD.Base.Placement | FreeCAD.Base.Matrix")
        self.assertEqual(_localize_expression("Base.Placement", "FreeCAD"), "Base.Placement")
        self.assertEqual(_localize_expression("FreeCAD.Base.Placement", "Robot"),
                         "FreeCAD.Base.Placement")

    def test_property_names_are_limited_to_catalog_and_document_object(self):
        owner = PublicPythonType('TechDraw', 'View')
        prop = TypedCppProperty(owner, 'Links', 'list[DocumentObject]', '_LinkInput | Sequence[str]', 'a.cpp', 1)
        catalog = PropertyCatalog((AliasSpec('_LinkInput', 'str'), AliasSpec('_Other', 'str')), ())
        self.assertEqual(property_support_names([prop], catalog), {'DocumentObject', '_LinkInput'})


if __name__ == '__main__':
    unittest.main()
