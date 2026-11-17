# SPDX-License-Identifier: LGPL-2.1-or-later
"""Which pull request events start the Linux Woodpecker pipeline.

python3 -m pytest -q tests/architecture/test_ci_pr_triggers.py

Draft pull requests start no pipeline. Marking a pull request "Ready for
review" arrives as a pull_request_metadata event and runs the full pipeline on
the same head, so every step that runs for pull_request must run for it too.
"""
from pathlib import Path
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
PR_TARGETS = ["FreeCAD-start", "fix/*", "chore/*", "integrate/*"]
NOT_DRAFT = 'CI_COMMIT_PULL_REQUEST_DRAFT != "true"'
READY_FOR_REVIEW = 'CI_PIPELINE_EVENT_REASON == "ready_for_review"'


def events(entry):
    event = entry.get("event", [])
    return [event] if isinstance(event, str) else list(event)


class LinuxPipelineTriggers(unittest.TestCase):
    def setUp(self):
        self.workflow = yaml.safe_load((ROOT / ".woodpecker/ci.yml").read_text())

    def test_drafts_are_skipped_and_ready_for_review_runs(self):
        self.assertEqual(self.workflow["when"], [
            {"event": ["manual", "tag"]},
            {"event": "pull_request", "branch": PR_TARGETS, "evaluate": NOT_DRAFT},
            {"event": "pull_request_metadata", "branch": PR_TARGETS,
             "evaluate": READY_FOR_REVIEW},
        ])

    def test_main_is_not_a_pull_request_target(self):
        for entry in self.workflow["when"]:
            self.assertNotIn("main", entry.get("branch", []))

    def test_ready_for_review_runs_every_pull_request_step(self):
        for step in self.workflow["steps"]:
            for entry in step.get("when", []):
                if "pull_request" in events(entry):
                    self.assertIn("pull_request_metadata", events(entry), step["name"])


if __name__ == "__main__":
    unittest.main()
