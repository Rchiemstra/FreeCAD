# SPDX-License-Identifier: LGPL-2.1-or-later
"""Observe expression failures and their consequences without recomputing.

``diagnose(document)`` returns JSON-compatible dictionaries. ``object`` is the
stable internal ``Document#Object`` name, ``property`` includes any subpath,
``role`` is ``root`` or ``affected``, and ``chain`` runs from cause to consequence
as a list of {object, property} dictionaries. Object-only consequences have empty
property/expression strings. Multiple roots can produce multiple rows for one
property; one shortest chain per root is retained. Order is deterministic.

Evaluation uses the currently stored input values, never hypothetical repaired
values. A differing result is a stale value, not proof of a failed recompute.
Touched alone is not a failure. Hidden references propagate impact but do not
create scheduling cycles. Closed external documents are never opened.

Run on a quiescent document, as with other FreeCAD Python read APIs. No writes,
transactions, status resets, dependency rebuilds or event pumping occur here.
"""

from collections import defaultdict, deque
import math
import re

import FreeCAD as App


def _path_related(first, second):
    return (first == second or first.startswith(second + ".")
            or first.startswith(second + "[") or second.startswith(first + ".")
            or second.startswith(first + "["))


def _step(key):
    return {"object": key[0], "property": key[1]}


def _error_kind(message):
    # Expression context can contain arbitrary object names such as UnitBox;
    # classify the actual error rather than keywords in the appended formula.
    reason = message.splitlines()[0].lower() if message else ""
    if any(word in reason for word in ("not found", "no attribute", "has no",
                                       "unresolved", "invalid property", "cannot resolve")):
        return "unresolved_reference"
    if re.search(r"\b(units?|dimensions?|dimensionless)\b", reason):
        return "unit_error"
    return "evaluation_error"


def _target_value(obj, path, current, evaluated):
    """Account for native scalar conversions without assigning a property."""
    name = path.split(".")[0].split("[")[0]
    type_id = obj.getTypeIdOfProperty(name)
    quantity = isinstance(evaluated, App.Units.Quantity)
    value = evaluated.Value if quantity else evaluated
    if type_id in ("App::PropertyInteger", "App::PropertyIntegerConstraint", "App::PropertyBool"):
        if not isinstance(value, (float, int)):
            raise TypeError(f"Expression result cannot be assigned to {type_id}.")
        rounded = math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5)
        if type_id == "App::PropertyBool":
            return bool(value if quantity or isinstance(value, int) else rounded)
        return rounded
    if type_id == "App::PropertyEnumeration":
        choices = obj.getEnumerationsOfProperty(name)
        if isinstance(evaluated, str):
            if evaluated not in choices:
                raise ValueError(f"Unknown enumeration value {evaluated!r} for {name}.")
            return choices.index(evaluated)
        if isinstance(value, (int, float)) and 0 <= int(value) < len(choices):
            return int(value)
        raise ValueError(f"Invalid enumeration value for {name}.")
    if type_id == "App::PropertyString" and not isinstance(evaluated, str):
        if isinstance(evaluated, bool):
            return str(int(evaluated))
        if isinstance(evaluated, int):
            return str(evaluated)
        if isinstance(evaluated, (float, App.Units.Quantity)):
            return App.Units.Quantity(evaluated).UserString
        return str(evaluated)
    if isinstance(current, (float, int, App.Units.Quantity)) and not isinstance(
            evaluated, (float, int, App.Units.Quantity)):
        raise TypeError(f"Expression result cannot be assigned to {type_id}.")
    return evaluated


def _equal(current, evaluated):
    # Numeric expression results are accepted as values in the target's native
    # units. Conversely PropertyFloat/Integer discard a Quantity's unit.
    if isinstance(current, App.Units.Quantity):
        if isinstance(evaluated, App.Units.Quantity):
            return current.Unit == evaluated.Unit and math.isclose(
                current.Value, evaluated.Value, rel_tol=1e-12, abs_tol=1e-12)
        if isinstance(evaluated, (float, int)):
            return math.isclose(current.Value, evaluated, rel_tol=1e-12, abs_tol=1e-12)
    if isinstance(current, (float, int)):
        if isinstance(evaluated, App.Units.Quantity):
            evaluated = evaluated.Value
        if isinstance(evaluated, (float, int)):
            return math.isclose(current, evaluated, rel_tol=1e-12, abs_tol=1e-12)
    return current == evaluated


def _cycles(graph):
    """Iterative DFS: return a closed witness chain for each cyclic node."""
    # Kosaraju avoids recursion limits for large expression networks.
    reverse = defaultdict(set)
    for node, edges in graph.items():
        for other in edges:
            reverse[other].add(node)
    visited, order = set(), []
    for start in sorted(graph):
        stack = [(start, False)]
        while stack:
            node, exiting = stack.pop()
            if exiting:
                order.append(node)
            elif node not in visited:
                visited.add(node)
                stack.append((node, True))
                stack.extend((n, False) for n in sorted(graph.get(node, ()), reverse=True)
                             if n not in visited)
    visited, witnesses = set(), {}
    for start in reversed(order):
        if start in visited:
            continue
        component, stack = set(), [start]
        while stack:
            node = stack.pop()
            if node in visited:
                continue
            visited.add(node)
            component.add(node)
            stack.extend(reverse[node] - visited)
        if len(component) == 1 and start not in graph.get(start, ()):
            continue
        for node in sorted(component):
            # Find a real cycle through this node; a sorted SCC is not a chain.
            queue = deque((n, [node, n]) for n in sorted(graph[node] & component))
            seen = set()
            while queue:
                current, chain = queue.popleft()
                if current == node:
                    witnesses[node] = chain
                    break
                if current in seen:
                    continue
                seen.add(current)
                queue.extend((n, chain + [n]) for n in sorted(graph[current] & component))
    return witnesses


def diagnose(document):
    """Return root failures and downstream impact in ``document`` as plain data.

    References into open external documents are followed to identify their roots;
    those roots are included only if a property in the requested document depends
    on them. Affected means dependency exposure, not an attempted recompute.
    """
    objects = {}
    records, references, failures = {}, {}, {}
    pending = deque(document.Objects)
    inspected = set()
    while pending:
        obj = pending.popleft()
        if obj.FullName in inspected:
            continue
        inspected.add(obj.FullName)
        objects[obj.FullName] = obj
        # Follow existing structural links as well: an open external feature can
        # be an upstream root even when it is consumed without an expression.
        for edge in obj.OutListProp:
            if edge.FromProp != "ExpressionEngine" and edge.ToObj is not None:
                pending.append(edge.ToObj)
        for path, expression in obj.ExpressionEngine:
            if expression is None:
                continue
            key = (obj.FullName, path.lstrip("."))
            records[key] = expression
            try:
                refs = obj.getExpressionReferences(expression)
            except Exception as exc:
                references[key] = []
                failures[key] = (_error_kind(str(exc)), str(exc))
                continue
            references[key] = refs
            for ref in refs:
                dependency = ref["object"]
                if dependency is not None:
                    pending.append(dependency)
            # Unresolved identifiers in an inactive conditional branch do not
            # make a successfully evaluated expression fail.
            try:
                evaluated = obj.evalExpression(expression)
            except Exception as exc:
                message = str(exc)
                kind = _error_kind(message)
                if kind == "evaluation_error" and any(
                        ref["error"] and ref["error"] in message for ref in refs):
                    kind = "unresolved_reference"
                if kind == "unresolved_reference" and any(
                        ref["external_document"] and ref["error"]
                        and ref["error"] in message for ref in refs):
                    kind = "external_document"
                failures[key] = (kind, message)
                continue
            try:
                current = obj.evalExpression("." + path.lstrip("."))
                evaluated = _target_value(obj, key[1], current, evaluated)
                if not _equal(current, evaluated):
                    failures[key] = ("stale_value", "Stored value differs from the expression evaluated with current inputs.")
            except Exception as exc:
                failures[key] = (_error_kind(str(exc)), str(exc))

    fine_grained = App.ParamGet("User parameter:BaseApp/Preferences/General").GetBool(
        "FineGrainedRecompute", False)

    def requires_object(ref):
        if not fine_grained or not ref["property"]:
            return True
        name = ref["property"].split(".")[0].split("[")[0]
        try:
            return not ("Input" in ref["object"].getPropertyStatus(name)
                        or "Input" in ref["object"].getTypeOfProperty(name))
        except AttributeError:
            return True

    # Edges point from a cause to its consumers. Match full property subpaths,
    # keeping unrelated expression bindings on the same object independent.
    by_object = defaultdict(list)
    for key in records:
        by_object[key[0]].append(key)
    consumers, scheduled = defaultdict(set), defaultdict(set)
    for key, refs in references.items():
        for ref in refs:
            obj = ref["object"]
            if obj is None:
                continue
            sources = [source for source in by_object[obj.FullName]
                       if ref["property"] and _path_related(source[1], ref["property"])]
            for source in sources:
                consumers[source].add(key)
                if not ref["hidden"]:
                    scheduled[source].add(key)
            if not sources and obj.FullName != key[0] and requires_object(ref):
                # Unbound outputs (e.g. Shape.Volume) are produced by execution
                # of the source object. Its failed expressions or structural
                # inputs can prevent that execution, even without a binding on
                # the referenced output itself. Keep that object step explicit.
                object_key = (obj.FullName, "")
                for source in by_object[obj.FullName]:
                    consumers[source].add(object_key)
                consumers[object_key].add(key)
    cycles = _cycles(scheduled)
    # Object scheduling dependencies also matter. In fine-grained mode an
    # input can be read without executing its owner; other references and
    # structural links still require the producer object to execute.
    object_graph = defaultdict(set)
    for obj in objects.values():
        owner = (obj.FullName, "")
        for edge in obj.OutListProp:
            if edge.FromProp != "ExpressionEngine" and edge.ToObj is not None:
                object_graph[(edge.ToObj.FullName, "")].add(owner)
    for key, refs in references.items():
        for ref in refs:
            dependency = ref["object"]
            if (dependency is not None and not ref["hidden"]
                    and dependency.FullName != key[0] and requires_object(ref)):
                object_graph[(dependency.FullName, "")].add((key[0], ""))
    object_cycles = _cycles(object_graph)
    for key, refs in references.items():
        owner = (key[0], "")
        if key in cycles or owner not in object_cycles:
            continue
        for ref in refs:
            dependency = ref["object"]
            if (dependency is None or ref["hidden"] or dependency.FullName == key[0]
                    or not requires_object(ref)):
                continue
            target = (dependency.FullName, "")
            queue, paths = deque([owner]), {owner: [owner]}
            while queue and target not in paths:
                current = queue.popleft()
                for other in sorted(object_graph[current]):
                    if other not in paths:
                        paths[other] = paths[current] + [other]
                        queue.append(other)
            if target in paths:
                cycles[key] = [key] + paths[target][1:] + [key]
                break
    for key in cycles:
        failures[key] = ("dependency_cycle", "Expression dependency cycle: " + " -> ".join(
            f"{obj}.{path}" if path else obj for obj, path in cycles[key]))

    # Structural links also carry consequences into features without expression
    # bindings. The object-level row deliberately makes no claim about outputs.
    for obj in objects.values():
        target = (obj.FullName, "")
        for edge in obj.OutListProp:
            if edge.FromProp == "ExpressionEngine" or edge.ToObj is None:
                continue
            source_name = edge.ToObj.FullName
            for source in by_object[source_name] + [(source_name, "")]:
                consumers[source].add(target)

    # A stale result downstream of another failure is a consequence. An actual
    # independent evaluation/reference/unit failure remains a root of its own.
    roots = {k for k, (kind, _) in failures.items() if kind != "stale_value"}
    exposed = set()
    queue = deque(roots)
    while queue:
        node = queue.popleft()
        for consumer in consumers[node]:
            if consumer not in exposed:
                exposed.add(consumer)
                queue.append(consumer)
    stale = {k for k, (kind, _) in failures.items() if kind == "stale_value"}
    # Process stale sources before their stale consumers, even when no hard
    # error exists (e.g. a changed parameter awaiting a recompute).
    reverse = defaultdict(set)
    for source, targets in consumers.items():
        for target in targets:
            reverse[target].add(source)
    stale_ancestors = {}
    for key in sorted(stale):
        ancestors, queue = set(), deque([key])
        while queue:
            for source in reverse[queue.popleft()]:
                if source not in ancestors:
                    ancestors.add(source)
                    queue.append(source)
        stale_ancestors[key] = (ancestors & stale) | {key}
    for key in sorted(stale):
        ancestors = stale_ancestors[key]
        # Hidden-reference loops are not scheduling cycles. If their values
        # differ, retain a stale root rather than dropping every member.
        peers = {other for other in ancestors if key in stale_ancestors[other]}
        if key not in exposed and ancestors == peers and key == min(peers):
            roots.add(key)

    result = []
    document_names = {obj.FullName for obj in document.Objects}
    for root in sorted(roots):
        paths, queue = {root: [root]}, deque([root])
        while queue:
            source = queue.popleft()
            for target in sorted(consumers[source]):
                if target not in paths:
                    paths[target] = paths[source] + [target]
                    queue.append(target)
        owner = (root[0], "")
        object_paths, queue = {owner: [root]}, deque([owner])
        while queue:
            current = queue.popleft()
            for other in sorted(object_graph[current]):
                if other not in object_paths:
                    object_paths[other] = object_paths[current] + [other]
                    queue.append(other)
        for key, chain in object_paths.items():
            if key != owner and (key not in paths or len(chain) < len(paths[key])):
                paths[key] = chain
        property_objects = {key[0] for key in paths if key[1]}
        if not any(k[0] in document_names for k in paths):
            continue
        kind, message = failures[root]
        for key, chain in sorted(paths.items()):
            if key != root and (key in roots or key[0] not in document_names
                                or (not key[1] and key[0] in property_objects)):
                continue
            is_root = key == root
            result.append({
                "object": key[0], "property": key[1],
                "expression": records.get(key, ""),
                "kind": kind if is_root else "dependency",
                "message": message if is_root else f"Depends on {root[0]}.{root[1]} ({kind}).",
                "role": "root" if is_root else "affected",
                "chain": [_step(k) for k in (cycles.get(key, chain) if is_root else chain)],
            })
    return result
