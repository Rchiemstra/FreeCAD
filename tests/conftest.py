"""
Pytest hooks for robot-simulation CI.

- Live Gazebo/FreeCAD tests require ``RUN_GAZEBO_LIVE=1`` (opt-in).
- ``CI=true`` forbids ``RUN_GAZEBO_LIVE=1`` to prevent accidental live gates in automation.
"""
from __future__ import annotations

import os

import pytest


def _truthy(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in ("1", "true", "yes")


def pytest_configure(config: pytest.Config) -> None:
    if _truthy("CI") and _truthy("RUN_GAZEBO_LIVE"):
        raise pytest.UsageError(
            "RUN_GAZEBO_LIVE must not be enabled when CI=true. "
            "Live host tests are opt-in on developer machines only."
        )


@pytest.fixture(autouse=True)
def _reset_run_context():
    """Prevent leaked sim run contexts between tests."""
    from bridge.run_context import finalize_run

    finalize_run()
    yield
    finalize_run()


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    if _truthy("RUN_GAZEBO_LIVE"):
        return
    skip_live = pytest.mark.skip(
        reason="Live tests opt-in (set RUN_GAZEBO_LIVE=1 locally; never in CI)"
    )
    live_keywords = {"gazebo", "freecad", "needs_freecad"}
    for item in items:
        if live_keywords.intersection(item.keywords):
            item.add_marker(skip_live)
