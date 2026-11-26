import ast
import tempfile
import unittest
from pathlib import Path
from stubgen.class_merge import public_class_stub_source
from stubgen.model import BindingClass

class Correction331Tests(unittest.TestCase):
    def render(self, path, cls, source):
        klass = BindingClass(source=path, line=1, class_name=cls, export_name=cls,
                             python_name=None, public_names=['Fem.' + cls],
                             base_class=None, explicit_export=False)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dest = root / path
            dest.parent.mkdir(parents=True)
            dest.write_text(source.lstrip())
            return public_class_stub_source(root, klass, 'Fem', {}, {cls}, {}, set())

    def test_multiframe_types(self):
        result = self.render('src/Mod/Fem/App/FemPostPipeline.pyi', 'FemPostPipeline', '''
class FemPostPipeline:
 @overload
 def read(self, files: list[str] | tuple[str], values: list[int] | tuple[int], unit: Unit, frame_type: str, /) -> None: ...
 @overload
 def read(self, name: str, /) -> None: ...
 def read(self, *args) -> None: ...
 @overload
 def load(self, result: list[DocumentObject] | tuple[DocumentObject], values: list[float] | tuple[float], unit: Unit, frame_type: str, /) -> None: ...
 @overload
 def load(self, obj: DocumentObject, /) -> None: ...
 def load(self, *args) -> None: ...
''')
        self.assertIn('tuple[str, ...]', result.source)
        self.assertIn('list[float] | tuple[float, ...]', result.source)
        self.assertIn('tuple[DocumentObject, ...]', result.source)
        self.assertEqual(sum(isinstance(n, ast.FunctionDef) for n in ast.parse(result.source).body[0].body), 4)

    def test_real_sketch_overloads_use_public_geometry_alias(self):
        root = Path(__file__).resolve().parents[4]
        path = 'src/Mod/Sketcher/App/SketchObject.pyi'
        line = next(node.lineno for node in ast.parse((root / path).read_text()).body
                    if isinstance(node, ast.ClassDef) and node.name == 'SketchObject')
        klass = BindingClass(source=path, line=line, class_name='SketchObject',
                             export_name='SketchObject', python_name=None,
                             public_names=['Sketcher.SketchObject'], base_class=None,
                             explicit_export=False)
        result = public_class_stub_source(
            root, klass, 'Sketcher', {}, {'SketchObject'},
            {('Part.App.Geometry', 'Geometry'): ('Part', 'Geometry')}, {'Part'})
        self.assertIn('from Part import Geometry as PartGeometry', result.import_lines)
        methods = [node for node in ast.parse(result.source).body[0].body
                   if isinstance(node, ast.FunctionDef) and node.name == 'addGeometry']
        self.assertEqual(len(methods), 2)
        self.assertEqual([ast.unparse(node.args.posonlyargs[1].annotation) for node in methods],
                         ['PartGeometry', 'List[PartGeometry]'])
        self.assertEqual([ast.unparse(node.returns) for node in methods],
                         ['int', 'Tuple[int, ...]'])

    def test_native_alias_support(self):
        for path, name, expected in (
            ('src/Mod/Mesh/App/Mesh.pyi', '_VectorInput', 'from FreeCAD import _VectorInput'),
            ('src/Mod/TechDraw/App/CosmeticEdge.pyi', 'PyCXXVector', 'from FreeCAD import Vector as PyCXXVector'),
        ):
            with self.subTest(path=path):
                result = self.render(path, 'Thing', f'class Thing:\n value: {name}\n')
                self.assertIn(expected, result.import_lines)

if __name__ == '__main__': unittest.main()
