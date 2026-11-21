#!/usr/bin/env python3
"""Write a CTest JUnit report to the GitHub step summary.

    .github/scripts/test-summary.py [--title TITLE] report.xml [report.xml ...]

Missing reports are noted, not fatal: a build that failed before ctest ran
still needs a summary on the run page.
"""

import argparse
import os
import sys
import xml.etree.ElementTree as ET


def cell(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


def failure_text(case):
    problem = case.find("failure")
    if problem is None:
        problem = case.find("error")
    if problem is None:
        return ""
    detail = (problem.text or problem.get("message") or "").strip()
    return detail.splitlines()[0] if detail else ""


def summarize(path):
    """Returns the markdown for one report."""
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        return f"_{path}: no usable report ({error})_\n"

    suites = [root] if root.tag == "testsuite" else root.findall("testsuite")
    cases = [case for suite in suites for case in suite.findall("testcase")]
    failed = [case for case in cases if case.find("failure") is not None or case.find("error") is not None]
    skipped = [case for case in cases if case.find("skipped") is not None]
    total_seconds = sum(float(case.get("time") or 0) for case in cases)

    if failed:
        lines = [f"**{len(failed)} of {len(cases)} failed** ({total_seconds:.1f}s)", "",
                 "| Test | Failure |", "|---|---|"]
        for case in failed:
            lines.append(f"| {cell(case.get('name'))} | {cell(failure_text(case)[:300])} |")
        return "\n".join(lines) + "\n"

    note = f", {len(skipped)} skipped" if skipped else ""
    lines = [f"{len(cases)} passed{note} in {total_seconds:.1f}s"]

    slowest = sorted(cases, key=lambda case: float(case.get("time") or 0), reverse=True)[:5]
    if slowest and float(slowest[0].get("time") or 0) >= 1.0:
        times = ", ".join(f"{cell(case.get('name'))} {float(case.get('time')):.1f}s" for case in slowest)
        lines += ["", f"Slowest: {times}"]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--title", default="Test results")
    parser.add_argument("reports", nargs="*")
    args = parser.parse_args()

    lines = [f"### {args.title}", ""]
    if not args.reports:
        lines.append("_No report: the build failed before the tests ran._")
    for path in args.reports:
        if len(args.reports) > 1:
            lines += [f"**{os.path.basename(path)}**", ""]
        lines.append(summarize(path).rstrip())
        lines.append("")

    summary = "\n".join(lines) + "\n"
    destination = os.environ.get("GITHUB_STEP_SUMMARY")
    if destination:
        with open(destination, "a", encoding="utf-8") as handle:
            handle.write(summary)
    else:
        print(summary, end="")


if __name__ == "__main__":
    main()
