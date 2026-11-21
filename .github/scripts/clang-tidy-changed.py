#!/usr/bin/env python3
"""Run clang-tidy on the lines a change adds, not the whole tree.

    .github/scripts/clang-tidy-changed.py --binary clang-tidy --build-dir build-tidy --base origin/main

The tree predates clang-tidy, so a whole-file run reports years of findings
nobody is going to fix. Filtering to added lines means a PR only has to keep
its own lines clean, and the check becomes blocking without demanding a
cleanup first.

Requires a CMake configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON. Files
without a compile command (for example a test target excluded from that
configure) are skipped with a notice.

Exit status: 1 when an added line has a finding, 0 otherwise.
"""

import argparse
import json
import os
import re
import subprocess
import sys

DIAGNOSTIC = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?P<severity>warning|error): (?P<message>.*)$")
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(?P<start>\d+)(?:,(?P<count>\d+))? @@")

CHECKS = ",".join(
    [
        "-*",
        "clang-analyzer-*",
        "bugprone-dangling-handle",
        "bugprone-sizeof-expression",
        "bugprone-string-constructor",
        "bugprone-use-after-move",
    ]
)


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def merge_base(base):
    try:
        return git("merge-base", base, "HEAD").strip()
    except subprocess.CalledProcessError:
        return base


def added_lines(base):
    """Maps repository-relative .cpp path to the set of added line numbers."""
    diff = git("diff", "--unified=0", "--no-color", f"{merge_base(base)}..HEAD", "--", "*.cpp", "*.cc")
    changed = {}
    path = None
    line = 0
    for entry in diff.splitlines():
        if entry.startswith("+++ b/"):
            path = entry[6:]
            changed.setdefault(path, set())
            continue
        hunk = HUNK.match(entry)
        if hunk:
            line = int(hunk.group("start"))
            continue
        if path is None:
            continue
        if entry.startswith("+"):
            changed[path].add(line)
            line += 1
        elif entry.startswith(" "):
            line += 1
    return {path: lines for path, lines in changed.items() if lines}


def compile_database(build_dir):
    path = os.path.join(build_dir, "compile_commands.json")
    try:
        with open(path, encoding="utf-8") as handle:
            return {os.path.realpath(entry["file"]) for entry in json.load(handle)}
    except (OSError, ValueError, KeyError) as error:
        sys.exit(f"cannot read {path}: {error}. Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON.")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="clang-tidy")
    parser.add_argument("--build-dir", default="build-tidy")
    parser.add_argument("--base", default="origin/main")
    args = parser.parse_args()

    if not os.access(args.binary, os.X_OK):
        sys.exit(f"clang-tidy not found at {args.binary}")

    root = os.path.realpath(git("rev-parse", "--show-toplevel").strip())
    sources = compile_database(args.build_dir)
    findings = []
    checked = 0

    for path, lines in sorted(added_lines(args.base).items()):
        absolute = os.path.realpath(os.path.join(root, path))
        if absolute not in sources:
            print(f"skip {path}: no compile command in {args.build_dir}/compile_commands.json")
            continue
        checked += 1
        compiled = subprocess.run(
            [
                args.binary,
                "-p", args.build_dir,
                "--quiet",
                "--checks=" + CHECKS,
                "--header-filter=^$",
                absolute,
            ],
            capture_output=True,
            text=True,
        )
        for entry in compiled.stderr.splitlines():
            match = DIAGNOSTIC.match(entry)
            if not match:
                continue
            diagnostic_file = os.path.realpath(os.path.join(root, match.group("file")))
            if diagnostic_file != absolute:
                continue
            if int(match.group("line")) in lines:
                findings.append(
                    f"{path}:{match.group('line')}:{match.group('col')}: "
                    f"{match.group('severity')}: {match.group('message')}"
                )

    summary = [f"### clang-tidy ({checked} changed translation unit{'s' if checked != 1 else ''})", ""]
    if findings:
        summary += [f"{len(findings)} finding(s) on changed lines:", ""]
        summary += [f"- `{finding}`" for finding in findings]
    else:
        summary.append("No findings on changed lines.")
    destination = os.environ.get("GITHUB_STEP_SUMMARY")
    if destination:
        with open(destination, "a", encoding="utf-8") as handle:
            handle.write("\n".join(summary) + "\n")
    print("\n".join(findings) if findings else "clang-tidy: no findings on changed lines")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
