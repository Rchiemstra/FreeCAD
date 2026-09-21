# SPDX-License-Identifier: LGPL-2.1-or-later
"""Focused unit tests for the offline responsiveness contract."""

from __future__ import annotations

import json
import unittest

from tests.gui.nonblocking.harness import (
    ACTION_KINDS,
    ActionRecord,
    ContractError,
    DURATION_MS,
    EvidenceRecord,
    ResponsivenessScenario,
)


def _scenario(
    latencies: tuple[int, ...] = (12, 18, 25, 14, 16, 19, 20, 22, 24),
) -> ResponsivenessScenario:
    actions = tuple(
        ActionRecord(
            sequence=index,
            kind=kind,
            at_ms=(DURATION_MS * index) // (len(ACTION_KINDS) - 1),
            value=kind,
        )
        for index, kind in enumerate(ACTION_KINDS)
    )
    evidence = tuple(
        EvidenceRecord(
            sequence=index,
            action_sequence=index % len(actions),
            kind=actions[index % len(actions)].kind,
            latency_ms=value,
            outcome="accepted",
            busy=actions[index % len(actions)].kind == "busy_response",
            cancelled=actions[index % len(actions)].kind == "cancellation",
        )
        for index, value in enumerate(latencies)
    )
    return ResponsivenessScenario(actions=actions, evidence=evidence)


class HarnessTests(unittest.TestCase):
    def test_valid_evidence_is_complete_and_round_trips_without_pointers(self):
        scenario = _scenario()
        scenario.validate()
        decoded = ResponsivenessScenario.from_json(scenario.to_json())

        self.assertEqual(decoded, scenario)
        self.assertTrue(scenario.evaluate_thresholds().passed)
        self.assertEqual(json.loads(scenario.to_json())["duration_ms"], DURATION_MS)

    def test_missing_action_is_rejected(self):
        scenario = _scenario()
        incomplete = ResponsivenessScenario(
            actions=tuple(item for item in scenario.actions if item.kind != "resize"),
            evidence=scenario.evidence,
        )

        with self.assertRaisesRegex(ContractError, "missing required actions: resize"):
            incomplete.validate()

    def test_missing_evidence_is_rejected(self):
        scenario = _scenario()
        incomplete = ResponsivenessScenario(
            actions=scenario.actions,
            evidence=tuple(
                item for item in scenario.evidence if item.kind != "cancellation"
            ),
        )

        with self.assertRaisesRegex(
            ContractError, "missing required evidence: cancellation"
        ):
            incomplete.validate()

    def test_interval_and_timestamp_order_are_required(self):
        scenario = _scenario()
        self.assertEqual(scenario.actions[0].at_ms, 0)
        self.assertEqual(scenario.actions[-1].at_ms, DURATION_MS)

        out_of_order = ResponsivenessScenario(
            actions=(
                scenario.actions[0],
                ActionRecord(1, "resize", scenario.actions[2].at_ms, "resize"),
                ActionRecord(2, "navigation", scenario.actions[1].at_ms, "navigation"),
                *scenario.actions[3:],
            ),
            evidence=scenario.evidence,
        )
        with self.assertRaisesRegex(ContractError, "ordered by nondecreasing"):
            out_of_order.validate()

        incomplete_interval = ResponsivenessScenario(
            actions=scenario.actions[:-1]
            + (ActionRecord(8, "latency_sample", DURATION_MS - 1, "latency_sample"),),
            evidence=scenario.evidence,
        )
        with self.assertRaisesRegex(ContractError, "complete interval"):
            incomplete_interval.validate()

    def test_thresholds_fail_for_p99_and_maximum(self):
        scenario = _scenario((10,) * 98 + (51, 51))
        result = scenario.evaluate_thresholds()
        self.assertEqual(result.p99_ms, 51)
        self.assertEqual(result.maximum_ms, 51)
        self.assertFalse(result.passed)

        scenario = _scenario((10,) * 98 + (50, 101))
        result = scenario.evaluate_thresholds()
        self.assertEqual(result.p99_ms, 50)
        self.assertEqual(result.maximum_ms, 101)
        self.assertFalse(result.passed)


    def test_serialization_has_deterministic_ordering_and_rejects_bad_links(self):
        scenario = _scenario((11, 7, 9, 12, 13, 14, 15, 16, 17))
        first = scenario.to_json()
        second = ResponsivenessScenario.from_dict(json.loads(first)).to_json()
        self.assertEqual(first, second)
        self.assertLess(first.index('"actions"'), first.index('"evidence"'))

        bad_evidence = EvidenceRecord(0, 999, "repaint", 1, "accepted")
        broken = ResponsivenessScenario(
            actions=scenario.actions,
            evidence=(bad_evidence,) + scenario.evidence[1:],
        )
        with self.assertRaisesRegex(ContractError, "references missing action"):
            broken.validate()
