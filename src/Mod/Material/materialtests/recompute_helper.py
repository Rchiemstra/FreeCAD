# SPDX-License-Identifier: LGPL-2.1-or-later

"""GUI-safe document recompute for Material unittest modules."""

from __future__ import annotations

import importlib


def _helpers():
    return importlib.import_module("Test.GuiRecompute")


def recompute_document(document, timeout_seconds: float = 120.0) -> None:
    return _helpers().recompute_document(document, timeout_seconds=timeout_seconds)


def close_document(document, timeout_seconds: float = 120.0) -> None:
    return _helpers().close_document(document, timeout_seconds=timeout_seconds)


