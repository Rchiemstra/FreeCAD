# SPDX-License-Identifier: LGPL-2.1-or-later
"""Focused unit tests for the offline responsiveness contract."""

from __future__ import annotations

import json
import unittest

from tests.gui.nonblocking.harness import (
    ACTION_KINDS,
    DURATION_MS,
    ActionRecord,
    ContractError,
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
            at_ms=actions[index % len(actions)].at_ms,
            kind=actions[index % len(actions)].kind,
            latency_ms=(0 if actions[index % len(actions)].at_ms == DURATION_MS else value),
            outcome=(
                "busy"
                if actions[index % len(actions)].kind == "busy_response"
                else (
                    "cancelled"
                    if actions[index % len(actions)].kind == "cancellation"
                    else "accepted"
                )
            ),
            busy=actions[index % len(actions)].kind == "busy_response",
            cancelled=actions[index % len(actions)].kind == "cancellation",
            completed_at_ms=actions[index % len(actions)].at_ms
            + (0 if actions[index % len(actions)].at_ms == DURATION_MS else value),
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
            evidence=tuple(item for item in scenario.evidence if item.kind != "cancellation"),
        )

        with self.assertRaisesRegex(ContractError, "missing required evidence: cancellation"):
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
        with self.assertRaisesRegex(ContractError, "ordered by strictly increasing"):
            out_of_order.validate()

        incomplete_interval = ResponsivenessScenario(
            actions=scenario.actions[:-1]
            + (ActionRecord(8, "latency_sample", DURATION_MS - 1, "latency_sample"),),
            evidence=scenario.evidence,
        )
        with self.assertRaisesRegex(ContractError, "complete interval"):
            incomplete_interval.validate()

        all_zero = ResponsivenessScenario(
            actions=tuple(
                ActionRecord(item.sequence, item.kind, 0, item.value) for item in scenario.actions
            ),
            evidence=scenario.evidence,
        )
        with self.assertRaisesRegex(ContractError, "complete interval"):
            all_zero.validate()

    def test_each_scheduled_action_requires_evidence(self):
        scenario = _scenario()
        evidence = scenario.evidence[:-1] + (
            EvidenceRecord(
                sequence=len(scenario.evidence) - 1,
                action_sequence=0,
                at_ms=0,
                kind="repaint",
                latency_ms=12,
                outcome="accepted",
            ),
        )
        with self.assertRaisesRegex(ContractError, "missing required evidence: latency_sample"):
            ResponsivenessScenario(scenario.actions, evidence).validate()

    def test_busy_and_cancellation_evidence_must_be_explicit(self):
        scenario = _scenario()
        bad = list(scenario.evidence)
        busy_index = next(index for index, item in enumerate(bad) if item.kind == "busy_response")
        original = bad[busy_index]
        bad[busy_index] = EvidenceRecord(
            sequence=original.sequence,
            action_sequence=original.action_sequence,
            at_ms=original.at_ms,
            kind=original.kind,
            latency_ms=original.latency_ms,
            outcome="accepted",
            completed_at_ms=original.completed_at_ms,
        )
        with self.assertRaisesRegex(ContractError, "must be marked busy"):
            ResponsivenessScenario(scenario.actions, tuple(bad)).validate()

        cancellation_index = next(
            index for index, item in enumerate(scenario.evidence) if item.kind == "cancellation"
        )
        original = scenario.evidence[cancellation_index]
        cancellation_bad = list(scenario.evidence)
        cancellation_bad[cancellation_index] = EvidenceRecord(
            sequence=original.sequence,
            action_sequence=original.action_sequence,
            at_ms=original.at_ms,
            kind=original.kind,
            latency_ms=original.latency_ms,
            outcome="cancelled",
            busy=True,
            cancelled=True,
            completed_at_ms=original.completed_at_ms,
        )
        with self.assertRaisesRegex(ContractError, "must be marked cancelled"):
            ResponsivenessScenario(scenario.actions, tuple(cancellation_bad)).validate()

    def test_thresholds_fail_for_p99_and_maximum(self):
        scenario = _scenario((10,) * 96 + (51, 51) + (10, 10))
        result = scenario.evaluate_thresholds()
        self.assertEqual(result.p99_ms, 51)
        self.assertEqual(result.maximum_ms, 51)
        self.assertFalse(result.passed)

        scenario = _scenario((10,) * 96 + (50, 101) + (10, 10))
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

        bad_evidence = EvidenceRecord(0, 999, 0, "repaint", 1, "accepted")
        broken = ResponsivenessScenario(
            actions=scenario.actions,
            evidence=(bad_evidence,) + scenario.evidence[1:],
        )
        with self.assertRaisesRegex(ContractError, "references missing action"):
            broken.validate()

        tampered = json.loads(first)
        tampered["thresholds"]["passed"] = False
        with self.assertRaisesRegex(ContractError, "thresholds do not match"):
            ResponsivenessScenario.from_dict(tampered)

        missing = json.loads(first)
        del missing["thresholds"]
        with self.assertRaisesRegex(ContractError, "malformed scenario thresholds"):
            ResponsivenessScenario.from_dict(missing)

        contradictory = json.loads(first)
        contradictory["thresholds"]["p99_limit_ms"] = 51
        with self.assertRaisesRegex(ContractError, "thresholds do not match"):
            ResponsivenessScenario.from_dict(contradictory)

        non_strict = json.loads(first)
        non_strict["thresholds"]["sample_count"] = True
        with self.assertRaisesRegex(ContractError, "thresholds do not match"):
            ResponsivenessScenario.from_dict(non_strict)

    def test_completion_timestamp_must_explain_latency(self):
        scenario = _scenario()
        original = scenario.evidence[0]
        inconsistent = (
            EvidenceRecord(
                sequence=original.sequence,
                action_sequence=original.action_sequence,
                at_ms=original.at_ms,
                kind=original.kind,
                latency_ms=original.latency_ms + 1,
                outcome=original.outcome,
                busy=original.busy,
                cancelled=original.cancelled,
                completed_at_ms=original.completed_at_ms,
            ),
        ) + scenario.evidence[1:]
        with self.assertRaisesRegex(ContractError, "latency_ms must equal"):
            ResponsivenessScenario(scenario.actions, inconsistent).validate()

        original = scenario.evidence[1]
        before_observation = (
            scenario.evidence[:1]
            + (
                EvidenceRecord(
                    sequence=original.sequence,
                    action_sequence=original.action_sequence,
                    at_ms=original.at_ms,
                    kind=original.kind,
                    latency_ms=0,
                    outcome=original.outcome,
                    busy=original.busy,
                    cancelled=original.cancelled,
                    completed_at_ms=original.at_ms - 1,
                ),
            )
            + scenario.evidence[2:]
        )
        with self.assertRaisesRegex(ContractError, "must not precede observation"):
            ResponsivenessScenario(scenario.actions, before_observation).validate()

    def test_scenario_rejects_foreign_record_types(self):
        scenario = _scenario()

        class ForeignAction:
            kind = "repaint"

        with self.assertRaisesRegex(ContractError, "only ActionRecord"):
            ResponsivenessScenario((ForeignAction(),) + scenario.actions[1:], scenario.evidence)

        class ForeignEvidence:
            kind = "repaint"

        with self.assertRaisesRegex(ContractError, "only EvidenceRecord"):
            ResponsivenessScenario(scenario.actions, (ForeignEvidence(),) + scenario.evidence[1:])
