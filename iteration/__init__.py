"""
iteration — Phase 5 Iteration Loops package.

Enables LLM agents to make bounded design changes, re-run scenarios, and
compare results:

    from iteration.loop import IterationLoop
    from iteration.policy import Policy, ParameterRule

    policy = Policy([
        ParameterRule("link1_length", min_val=0.1, max_val=1.0, step=0.05, unit="m"),
        ParameterRule("link2_length", min_val=0.1, max_val=0.8, step=0.05, unit="m"),
    ])

    loop = IterationLoop(scenario_name="reach_top_shelf", policy=policy)
    result = loop.run_once({"link1_length": 0.6, "link2_length": 0.4})
    print(result.summary())

    sweep_results = loop.sweep("link1_length", [0.4, 0.5, 0.6])
    report = loop.compare(sweep_results)
    print(report)
"""
from iteration.loop import IterationLoop
from iteration.policy import Policy, ParameterRule
from iteration.parameter import set_parameter, get_parameter
from iteration.report import compare_results, summarize_failure

__all__ = [
    "IterationLoop", "Policy", "ParameterRule",
    "set_parameter", "get_parameter",
    "compare_results", "summarize_failure",
]
