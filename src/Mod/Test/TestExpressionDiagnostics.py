# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression tests for observational, property-level expression diagnostics."""

import json
import os
import tempfile
import unittest

import FreeCAD as App
try:
    from Test.GuiRecompute import close_document, recompute_document, save_document_as
except ImportError:
    # Upstream uses synchronous document operations; this fork also supports
    # running tests through the asynchronous GUI document helpers.
    def close_document(doc):
        App.closeDocument(doc.Name)

    def recompute_document(doc):
        doc.recompute()

    def save_document_as(doc, path):
        doc.saveAs(path)



class ExpressionDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("ExpressionDiagnosticsTest")
        self.documents = [self.doc.Name]
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        for name in self.documents:
            doc = App.listDocuments().get(name)
            if doc is not None:
                close_document(doc)
        self.tmp.cleanup()

    def obj(self, name, doc=None):
        obj = (doc if doc is not None else self.doc).addObject("App::FeaturePython", name)
        obj.addProperty("App::PropertyFloat", "Value")
        obj.addProperty("App::PropertyFloat", "Other")
        return obj

    def diagnose(self):
        from freecad.expression_diagnostics import diagnose

        result = diagnose(self.doc)
        # Public data must be usable outside FreeCAD, e.g. by report consumers.
        self.assertEqual(json.loads(json.dumps(result)), result)
        for row in result:
            self.assertTrue({"object", "property", "expression", "kind", "message",
                             "role", "chain"}.issubset(row))
            self.assertIn(row["role"], ("root", "affected"))
            self.assertTrue(row["message"])
        return result

    def test_healthy(self):
        a = self.obj("A")
        a.setExpression("Value", "2 * 3")
        recompute_document(self.doc)
        self.assertEqual(self.diagnose(), [])

    def test_inactive_missing_reference_is_healthy(self):
        a = self.obj("A")
        a.setExpression("Value", "0 ? Absent.Value : 42")
        recompute_document(self.doc)
        self.assertEqual(a.Value, 42)
        self.assertTrue(a.isValid())
        self.assertEqual(self.diagnose(), [])

    def test_missing_object(self):
        a = self.obj("A")
        a.setExpression("Value", "<<Absent>>.Value + 1")
        rows = self.diagnose()
        self.assertEqual([(r["object"], r["property"], r["kind"], r["role"])
                          for r in rows], [(a.FullName, "Value", "unresolved_reference", "root")])
        self.assertIn("Absent", rows[0]["message"])

    def test_missing_property(self):
        a, b = self.obj("A"), self.obj("B")
        a.setExpression("Value", "<<B>>.Absent + 1")
        rows = self.diagnose()
        self.assertEqual(rows[0]["kind"], "unresolved_reference")
        self.assertIn("Absent", rows[0]["message"])

    def test_unit_mismatch(self):
        a = self.obj("A")
        a.setExpression("Value", "1 mm + 1 s")
        self.assertEqual(self.diagnose()[0]["kind"], "unit_error")

    def test_evaluation_error(self):
        a = self.obj("A")
        a.setExpression("Value", "1 / 0")
        self.assertEqual(self.diagnose()[0]["kind"], "evaluation_error")

    def test_error_kind_does_not_depend_on_object_name(self):
        source, driven = self.obj("UnitObject"), self.obj("Driven")
        driven.setExpression("Value", "1 / UnitObject.Value")
        self.assertEqual(self.diagnose()[0]["kind"], "evaluation_error")

    def test_stale_value(self):
        a = self.obj("A")
        a.setExpression("Value", "42")
        rows = self.diagnose()
        self.assertEqual(rows[0]["kind"], "stale_value")
        self.assertEqual(a.Value, 0)

    def test_healthy_units_and_subpaths(self):
        a = self.obj("A")
        a.addProperty("App::PropertyLength", "Length")
        a.addProperty("App::PropertyPlacement", "Placement")
        a.setExpression("Length", "10")
        a.setExpression("Placement.Base.x", "5 mm")
        recompute_document(self.doc)
        self.assertEqual(self.diagnose(), [])

    def test_branching_downstream(self):
        a, b, c, d = [self.obj(n) for n in "ABCD"]
        a.setExpression("Value", "Absent.Value")
        b.setExpression("Value", "<<A>>.Value + 1")
        c.setExpression("Value", "<<A>>.Value + 2")
        d.setExpression("Value", "<<B>>.Value + <<C>>.Value")
        # An unrelated property on the same object must not be blamed.
        b.Other = 5
        b.setExpression("Other", "5")
        rows = self.diagnose()
        self.assertEqual({r["object"] for r in rows}, {o.FullName for o in (a, b, c, d)})
        roots = [r for r in rows if r["role"] == "root"]
        self.assertEqual([(r["object"], r["property"]) for r in roots], [(a.FullName, "Value")])
        self.assertFalse(any(r["property"] == "Other" for r in rows))
        end = next(r for r in rows if r["object"] == d.FullName)
        self.assertEqual(end["chain"][0], {"object": a.FullName, "property": "Value"})
        self.assertEqual(end["chain"][-1], {"object": d.FullName, "property": "Value"})
        self.assertEqual(len(end["chain"]), 3)

    def test_structural_downstream_object(self):
        a, b, c = [self.obj(n) for n in "ABC"]
        a.setExpression("Value", "Absent.Value")
        b.addProperty("App::PropertyLink", "Base")
        c.addProperty("App::PropertyLink", "Base")
        b.Base = a
        c.Base = b
        rows = self.diagnose()
        self.assertEqual({(r["object"], r["property"], r["role"]) for r in rows},
                         {(a.FullName, "Value", "root"), (b.FullName, "", "affected"),
                          (c.FullName, "", "affected")})
        self.assertEqual(len(next(r for r in rows if r["object"] == c.FullName)["chain"]), 3)

    def test_expression_reading_unbound_output(self):
        a, b = self.obj("A"), self.obj("B")
        a.addProperty("App::PropertyVector", "Output")
        a.Output = App.Vector(10, 0, 0)
        a.setExpression("Value", "Absent.Value")
        b.setExpression("Value", "<<A>>.Output.x + 1")
        rows = self.diagnose()
        self.assertEqual([(r["object"], r["role"]) for r in rows],
                         [(a.FullName, "root"), (b.FullName, "affected")])
        self.assertEqual(rows[1]["chain"], [
            {"object": a.FullName, "property": "Value"},
            {"object": a.FullName, "property": ""},
            {"object": b.FullName, "property": "Value"}])

    def test_expression_after_structural_dependency(self):
        a, b, c = [self.obj(n) for n in "ABC"]
        a.setExpression("Value", "Absent.Value")
        b.addProperty("App::PropertyLink", "Base")
        b.Base = a
        c.setExpression("Value", "<<B>>.Other + 1")
        rows = self.diagnose()
        self.assertEqual({(r["object"], r["property"], r["role"]) for r in rows},
                         {(a.FullName, "Value", "root"), (b.FullName, "", "affected"),
                          (c.FullName, "Value", "affected")})

    def test_multiple_failures_on_one_object(self):
        a = self.obj("A")
        a.setExpression("Value", "Absent.Value")
        a.setExpression("Other", "1 mm + 1 s")
        rows = self.diagnose()
        self.assertEqual({r["kind"] for r in rows}, {"unresolved_reference", "unit_error"})
        self.assertEqual(len(rows), 2)

    def test_stale_downstream_has_one_root(self):
        a, b = self.obj("A"), self.obj("B")
        a.setExpression("Value", "42")
        b.setExpression("Value", "<<A>>.Value + 1")
        rows = self.diagnose()
        self.assertEqual([(r["object"], r["role"]) for r in rows],
                         [(a.FullName, "root"), (b.FullName, "affected")])

    def test_healthy_integer_conversion(self):
        a = self.obj("A")
        a.addProperty("App::PropertyInteger", "Count")
        a.setExpression("Count", "1.6")
        recompute_document(self.doc)
        self.assertEqual(a.Count, 2)
        self.assertEqual(self.diagnose(), [])

    def test_healthy_percent_conversion(self):
        a = self.obj("A")
        a.addProperty("App::PropertyPercent", "Percent")
        a.setExpression("Percent", "1.6")
        recompute_document(self.doc)
        self.assertEqual(a.Percent, 2)
        self.assertEqual(self.diagnose(), [])

    def test_placement_rotation_range(self):
        a = self.obj("A")
        a.addProperty("App::PropertyPlacement", "Placement")
        for angle, value in (("Yaw", 200), ("Pitch", 100), ("Roll", -200)):
            with self.subTest(angle=angle):
                path = "Placement.Rotation." + angle
                a.setExpression(path, str(value))
                recompute_document(self.doc)
                self.assertFalse(a.isValid())
                rows = self.diagnose()
                self.assertEqual(rows[0]["kind"], "evaluation_error")
                self.assertIn("out of range", rows[0]["message"])
                a.setExpression(path, None)

    def callback_state(self):
        return (self.doc.UndoCount, self.doc.RedoCount, self.doc.isTouched(),
                getattr(self.doc, "getFileChangeState", lambda: None)(),
                [(o.Name, list(o.State), list(o.ExpressionEngine), o.Value, o.Other,
                  [o.getPropertyStatus(p) for p in o.PropertiesList]) for o in self.doc.Objects])

    def test_proxy_reference_is_not_evaluated(self):
        source, driven = self.obj("Source"), self.obj("Driven")

        class Proxy:
            @property
            def Value(proxy):
                source.Other += 1
                return 42

        source.Proxy = Proxy()
        driven.setExpression("Value", "Source.Proxy.Value")
        self.doc.UndoMode = 1
        self.doc.openTransaction("Observe proxy")
        before = self.callback_state()
        rows = self.diagnose()
        self.assertEqual(self.callback_state(), before)
        self.assertEqual(rows[0]["kind"], "not_evaluated")
        self.assertIn("Python", rows[0]["message"])
        self.doc.abortTransaction()

    def test_python_destination_is_not_read(self):
        a = self.obj("A")

        class Proxy:
            @property
            def Value(proxy):
                a.Other += 1
                return 42

        a.Proxy = Proxy()
        a.setExpression("Proxy.Value", "42")
        before = self.callback_state()
        rows = self.diagnose()
        self.assertEqual(self.callback_state(), before)
        self.assertEqual(rows[0]["kind"], "not_evaluated")

    def test_python_object_property_is_not_evaluated(self):
        source, driven = self.obj("Source"), self.obj("Driven")

        class Payload:
            @property
            def Value(payload):
                source.Other += 1
                return 42

        source.addProperty("App::PropertyPythonObject", "Payload")
        source.Payload = Payload()
        driven.setExpression("Value", "Source.Payload.Value")
        before = self.callback_state()
        rows = self.diagnose()
        self.assertEqual(self.callback_state(), before)
        self.assertEqual(rows[0]["kind"], "not_evaluated")
        self.assertIn("Python", rows[0]["message"])

    def test_subobject_metadata_does_not_call_proxy(self):
        source, driven = self.obj("Source"), self.obj("Driven")
        calls = []

        class Proxy:
            def getSubObject(proxy, *args):
                calls.append(args)
                source.Other += 1
                return source, App.Matrix(), None

        source.Proxy = Proxy()
        expression = "Source.<<Child.>>.Value"
        driven.setExpression("Value", expression)
        # Native engine serialization can itself resolve an uncached subobject
        # path; prepare the snapshot before observing the parser binding.
        list(driven.ExpressionEngine)
        calls.clear()
        self.doc.UndoMode = 1
        self.doc.openTransaction("Observe subobject")
        before = self.callback_state()
        refs = driven.getExpressionReferences(expression)
        self.assertEqual(calls, [])
        self.assertEqual(self.callback_state(), before)
        self.assertTrue(refs[0]["unresolved_metadata"])
        self.assertTrue(refs[0]["error"])
        rows = self.diagnose()
        self.assertEqual(calls, [])
        self.assertEqual(self.callback_state(), before)
        self.assertEqual(rows[0]["kind"], "not_evaluated")
        self.doc.abortTransaction()

    def test_healthy_scalar_conversions(self):
        a = self.obj("A")
        a.addProperty("App::PropertyBool", "Enabled")
        a.addProperty("App::PropertyString", "Text")
        a.addProperty("App::PropertyString", "FlagText")
        a.addProperty("App::PropertyEnumeration", "Choice")
        a.Choice = ["First", "Second"]
        a.setExpression("Enabled", "2")
        a.setExpression("Text", "42")
        a.setExpression("FlagText", "True")
        a.setExpression("Choice", "<<Second>>")
        recompute_document(self.doc)
        self.assertEqual(self.diagnose(), [])

    def test_invalid_result_type(self):
        a = self.obj("A")
        a.setExpression("Value", "<<text>>")
        self.assertEqual(self.diagnose()[0]["kind"], "evaluation_error")

    def test_named_constraint_dependency(self):
        try:
            import Part
            import Sketcher
        except ImportError:
            self.skipTest("Sketcher is not built")
        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        sketch.addGeometry(Part.LineSegment(App.Vector(0, 0, 0), App.Vector(10, 0, 0)))
        index = sketch.addConstraint(Sketcher.Constraint("Distance", 0, 10.0))
        sketch.renameConstraint(index, "Width")
        b = self.obj("B")
        b.setExpression("Value", "Sketch.Constraints.Width + 1 mm")
        sketch.setExpression("Constraints.Width", "Absent.Value")
        rows = self.diagnose()
        self.assertEqual({(r["object"], r["role"]) for r in rows},
                         {(sketch.FullName, "root"), (b.FullName, "affected")})

    def test_hidden_reference_loop_is_stale_not_cycle(self):
        a, b = self.obj("A"), self.obj("B")
        a.setExpression("Value", "href(<<B>>.Value) + 1")
        b.setExpression("Value", "href(<<A>>.Value) + 1")
        rows = self.diagnose()
        self.assertEqual({r["object"] for r in rows}, {a.FullName, b.FullName})
        self.assertEqual([r["kind"] for r in rows if r["role"] == "root"], ["stale_value"])

    def test_hidden_structural_link_does_not_create_cycle_or_impact(self):
        a, b = self.obj("A"), self.obj("B")
        b.addProperty("App::PropertyLinkHidden", "BackReference")
        b.BackReference = a
        a.Value = 1
        a.setExpression("Value", "<<B>>.Other + 1")
        a.setExpression("Other", "Absent.Value")
        rows = self.diagnose()
        self.assertEqual([(r["object"], r["property"], r["kind"]) for r in rows],
                         [(a.FullName, "Other", "unresolved_reference")])

    def test_independent_error_is_still_a_root(self):
        a, b = self.obj("A"), self.obj("B")
        a.setExpression("Value", "Absent.Value")
        b.setExpression("Value", "<<A>>.Value + (1 mm + 1 s)")
        rows = self.diagnose()
        self.assertEqual({r["kind"] for r in rows if r["role"] == "root"},
                         {"unresolved_reference", "unit_error"})

    def test_labels_and_strings_are_not_dependencies(self):
        a, b = self.obj("A"), self.obj("B")
        a.Label = "Parameters with spaces"
        a.Value = 3
        b.setExpression("Value", "<<Parameters with spaces>>.Value + 2")
        b.addProperty("App::PropertyString", "Text")
        b.setExpression("Text", '<<Missing.Value>>')
        recompute_document(self.doc)
        self.assertEqual(self.diagnose(), [])
        a.setExpression("Value", "Absent.Value")
        rows = self.diagnose()
        self.assertEqual({r["property"] for r in rows}, {"Value"})
        self.assertEqual(len(rows), 2)

    def test_closed_external_document(self):
        external = App.newDocument("ExpressionDiagnosticsExternal")
        self.documents.append(external.Name)
        source = self.obj("Source", external)
        source.Value = 7
        save_document_as(external, os.path.join(self.tmp.name, "external.FCStd"))
        a = self.obj("A")
        save_document_as(self.doc, os.path.join(self.tmp.name, "owner.FCStd"))
        a.setExpression("Value", external.Name + "#Source.Value + 1")
        recompute_document(self.doc)
        self.assertEqual(self.diagnose(), [])
        external_name = external.Name
        close_document(external)
        self.documents.remove(external_name)
        rows = self.diagnose()
        self.assertEqual(rows[0]["kind"], "external_document")
        self.assertNotIn("ExpressionDiagnosticsExternal", App.listDocuments())

    def test_cycle_between_expression_bound_inputs(self):
        preferences = App.ParamGet("User parameter:BaseApp/Preferences/General")
        original = preferences.GetBool("FineGrainedRecompute", False)
        try:
            preferences.SetBool("FineGrainedRecompute", True)
            a, b = self.obj("A"), self.obj("B")
            # Fine-grained mode permits references to input properties in a
            # dependent object. Binding both inputs can still close a cycle.
            a.setPropertyStatus("Value", "Input")
            b.setPropertyStatus("Value", "Input")
            a.setExpression("Value", "<<B>>.Value + 1")
            b.setExpression("Value", "<<A>>.Value + 1")
            rows = self.diagnose()
            self.assertEqual({r["object"] for r in rows}, {a.FullName, b.FullName})
            self.assertTrue(all(r["kind"] == "dependency_cycle" and r["role"] == "root"
                                for r in rows))
            self.assertTrue(all(len(r["chain"]) >= 3 for r in rows))
        finally:
            preferences.SetBool("FineGrainedRecompute", original)

    def test_cycle_through_structural_link(self):
        preferences = App.ParamGet("User parameter:BaseApp/Preferences/General")
        if preferences.GetBool("FineGrainedRecompute", False):
            self.skipTest("Object scheduling cycles apply to ordinary recompute")
        a, b = self.obj("A"), self.obj("B")
        a.setExpression("Value", "<<B>>.Other + 1")
        b.Value = 2
        b.addProperty("App::PropertyLink", "Base")
        recompute_document(self.doc)
        # Link assignment can close a document scheduling cycle after the
        # expression setter has performed its own validation.
        b.Base = a
        rows = self.diagnose()
        roots = [r for r in rows if r["role"] == "root"]
        self.assertEqual([(r["object"], r["kind"]) for r in roots],
                         [(self.doc.A.FullName, "dependency_cycle")])

    def test_non_input_cycle_with_fine_grained_recompute(self):
        preferences = App.ParamGet("User parameter:BaseApp/Preferences/General")
        original = preferences.GetBool("FineGrainedRecompute", False)
        try:
            preferences.SetBool("FineGrainedRecompute", True)
            a, b = self.obj("A"), self.obj("B")
            a.setExpression("Value", "<<B>>.Other + 1")
            b.addProperty("App::PropertyLink", "Base")
            recompute_document(self.doc)
            b.Base = a
            roots = [r for r in self.diagnose() if r["role"] == "root"]
            self.assertEqual([(r["object"], r["kind"]) for r in roots],
                             [(a.FullName, "dependency_cycle")])
        finally:
            preferences.SetBool("FineGrainedRecompute", original)

    def test_fine_grained_input_breaks_object_cycle(self):
        preferences = App.ParamGet("User parameter:BaseApp/Preferences/General")
        original = preferences.GetBool("FineGrainedRecompute", False)
        try:
            preferences.SetBool("FineGrainedRecompute", True)
            a, b = self.obj("A"), self.obj("B")
            b.setPropertyStatus("Other", "Input")
            a.setExpression("Value", "<<B>>.Other + 1")
            b.addProperty("App::PropertyLink", "Base")
            recompute_document(self.doc)
            b.Base = a
            self.assertEqual(self.diagnose(), [])
        finally:
            preferences.SetBool("FineGrainedRecompute", original)

    def test_object_scheduling_impact_through_healthy_binding(self):
        a, b = self.obj("A"), self.obj("B")
        a.Other = 5
        a.setExpression("Other", "5")
        a.setExpression("Value", "Absent.Value")
        b.Value = 6
        b.setExpression("Value", "<<A>>.Other + 1")
        rows = self.diagnose()
        self.assertEqual({(r["object"], r["property"], r["role"]) for r in rows},
                         {(a.FullName, "Value", "root"), (b.FullName, "", "affected")})

    def test_link_instance_output_depends_on_instance_expressions(self):
        try:
            import Part
        except ImportError:
            self.skipTest("Part is not built")
        source = self.doc.addObject("Part::Box", "Source")
        instance = self.doc.addObject("App::Link", "Instance")
        instance.LinkedObject = source
        driven = self.obj("Driven")
        driven.setExpression("Value", "Instance._shape.Volume")
        recompute_document(self.doc)
        self.assertEqual(self.diagnose(), [])
        self.assertGreater(driven.Value, 0)
        self.assertTrue(driven.isValid())
        instance.setExpression("Scale", "Absent.Value")
        rows = self.diagnose()
        self.assertTrue(any(r["object"] == instance.FullName and r["property"] == "Scale"
                            and r["role"] == "root" for r in rows))
        self.assertTrue(any(r["object"] == driven.FullName and r["role"] == "affected"
                            for r in rows))

    def test_document_state_unchanged(self):
        a, b = self.obj("A"), self.obj("B")
        self.doc.UndoMode = 1
        self.doc.openTransaction("Expressions")
        a.setExpression("Value", "Absent.Value")
        b.setExpression("Value", "<<A>>.Value + 1")
        self.doc.commitTransaction()

        def state():
            return (self.doc.UndoCount, self.doc.RedoCount, self.doc.isTouched(),
                    getattr(self.doc, "getFileChangeState", lambda: None)(),
                    [(o.Name, list(o.State), list(o.ExpressionEngine), o.Value, o.Other,
                      [o.getPropertyStatus(p) for p in o.PropertiesList]) for o in self.doc.Objects])

        events = []

        class Observer:
            def slotChangedObject(self, obj, prop):
                events.append((obj.Name, prop))

            def slotRecomputedObject(self, obj):
                events.append((obj.Name, "recomputed"))

            def slotRecomputedDocument(self, doc):
                events.append((doc.Name, "recomputed"))

        observer = Observer()
        before = state()
        App.addDocumentObserver(observer)
        try:
            self.assertTrue(self.diagnose())
            self.assertEqual(state(), before)
            self.assertEqual(self.diagnose(), self.diagnose())
            self.assertEqual(events, [])
        finally:
            App.removeDocumentObserver(observer)
