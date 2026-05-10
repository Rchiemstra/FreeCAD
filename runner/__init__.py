"""
runner — Phase 4 Test Runner package.

Public API::

    from runner.runner import list_tests, run_test, run_all_tests

    tests = list_tests()           # -> list[str]
    result = run_test("reach_top_shelf")  # -> RunResult
    results = run_all_tests()      # -> list[RunResult]
"""
from runner.runner import list_tests, run_test, run_all_tests

__all__ = ["list_tests", "run_test", "run_all_tests"]
