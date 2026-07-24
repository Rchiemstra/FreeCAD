"""Pytest configuration and fixture setup."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

FIXTURES_DIR = Path(__file__).parent / "fixtures"


@pytest.fixture(scope="session", autouse=True)
def build_fixtures():
    """Build test fixtures once per session."""
    sys.path.insert(0, str(Path(__file__).parent))
    from fixtures.builder import build_all_fixtures

    build_all_fixtures(FIXTURES_DIR)


@pytest.fixture
def fixtures_dir() -> Path:
    return FIXTURES_DIR


@pytest.fixture
def config():
    from freecad_git.config import Config

    return Config()
