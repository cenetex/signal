#!/usr/bin/env python3
"""Keep production-container workflow filters aligned with Docker COPY inputs."""

from __future__ import annotations

import json
import re
import shlex
import sys
from pathlib import Path

import classify_ci_paths


ROOT = Path(__file__).resolve().parents[1]
DOCKERFILE = ROOT / "server" / "Dockerfile"
CI_PATH_POLICY = ROOT / ".github" / "ci-paths.json"
WORKFLOWS = [
    ROOT / ".github" / "workflows" / "ci.yml",
    ROOT / ".github" / "workflows" / "deploy-fly.yml",
]
IMPLICIT_DOCKER_INPUTS = {
    ".dockerignore",
    "server/Dockerfile",
}


def production_dependency_contract_failures(source: str) -> list[str]:
    """Return violations of the lock-enforced runtime dependency layer."""
    logical_lines = _logical_lines(source)
    package_copy_index = next((
        index for index, line in enumerate(logical_lines)
        if re.fullmatch(
            r"COPY\s+package\.json\s+package-lock\.json\s+\./",
            line,
            re.IGNORECASE,
        )
    ), None)
    install_index = next((
        index for index, line in enumerate(logical_lines)
        if re.search(r"\bnpm\s+ci\b", line)
        and "--omit=dev" in line
    ), None)

    failures = []
    if package_copy_index is None:
        failures.append(
            "server/Dockerfile: production dependency layer must COPY "
            "package.json and package-lock.json together"
        )
    if install_index is None:
        failures.append(
            "server/Dockerfile: production dependencies must use "
            "'npm ci --omit=dev'"
        )
    if (package_copy_index is not None
            and install_index is not None
            and package_copy_index > install_index):
        failures.append(
            "server/Dockerfile: package manifests must be copied before "
            "the lock-enforced production install"
        )

    install_line = (
        logical_lines[install_index] if install_index is not None else ""
    )
    if "npm cache clean --force" not in install_line:
        failures.append(
            "server/Dockerfile: production install must clean the npm cache"
        )
    if not re.search(r"\brm\s+-rf\s+/root/\.npm\b", install_line):
        failures.append(
            "server/Dockerfile: production install must remove /root/.npm"
        )
    return failures


def _logical_lines(source: str) -> list[str]:
    lines: list[str] = []
    pending = ""
    for raw in source.splitlines():
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        pending = f"{pending} {stripped}".strip()
        if pending.endswith("\\"):
            pending = pending[:-1].rstrip()
            continue
        lines.append(pending)
        pending = ""
    if pending:
        lines.append(pending)
    return lines


def docker_context_inputs(source: str) -> set[str]:
    """Return repository paths consumed by non-stage Docker COPY commands."""
    inputs: set[str] = set(IMPLICIT_DOCKER_INPUTS)
    for line in _logical_lines(source):
        match = re.match(r"(?i)^COPY\s+(.+)$", line)
        if not match:
            continue
        body = match.group(1).strip()
        if body.startswith("["):
            tokens = json.loads(body)
        else:
            tokens = shlex.split(body)
        if any(token == "--from" or token.startswith("--from=")
               for token in tokens):
            continue
        while tokens and tokens[0].startswith("--"):
            option = tokens.pop(0)
            if option in {"--chown", "--chmod"} and tokens:
                tokens.pop(0)
        if len(tokens) < 2:
            raise ValueError(f"unsupported COPY instruction: {line}")
        for token in tokens[:-1]:
            normalized = token.removeprefix("./")
            if not normalized or normalized.startswith(("http://", "https://")):
                continue
            if normalized.endswith("/"):
                inputs.add(f"{normalized}**")
            else:
                inputs.add(normalized)
    return inputs


def workflow_path_filters(source: str) -> set[str]:
    """Extract values from YAML `paths:` lists without a YAML dependency."""
    filters: set[str] = set()
    paths_indent: int | None = None
    for raw in source.splitlines():
        stripped = raw.strip()
        indent = len(raw) - len(raw.lstrip())
        if paths_indent is None:
            if stripped == "paths:":
                paths_indent = indent
            continue
        if not stripped or stripped.startswith("#"):
            continue
        if indent <= paths_indent:
            paths_indent = None
            if stripped == "paths:":
                paths_indent = indent
            continue
        match = re.match(r"^-\s+(.+?)\s*$", stripped)
        if not match:
            continue
        value = match.group(1).strip().strip("'\"")
        if value and not value.startswith("!"):
            filters.add(value)
    return filters


def pull_request_is_unfiltered(source: str) -> bool:
    """Return true when a workflow runs for every pull request."""
    trigger = re.search(
        r"^on:\s*$\n(?P<body>(?:^[ \t].*$\n|^\s*$\n)*)",
        source,
        re.MULTILINE,
    )
    if not trigger:
        return False
    body = trigger.group("body")
    pull_request = re.search(
        r"^  pull_request:\s*$\n"
        r"(?P<body>(?:^ {4,}.*$\n|^\s*$\n)*)",
        body,
        re.MULTILINE,
    )
    if not pull_request:
        return False
    return not re.search(
        r"^\s+paths(?:-ignore)?:\s*$",
        pull_request.group("body"),
        re.MULTILINE,
    )


def _filter_covers(required: str, candidate: str) -> bool:
    if candidate == "**" or candidate == required:
        return True
    if required.endswith("/**"):
        return False
    if candidate.endswith("/**"):
        return required.startswith(candidate[:-2])
    return False


def container_workflow_input_failures(
    dockerfile_source: str,
    workflow_source: str,
    workflow_name: str,
) -> list[str]:
    """Return Docker inputs missing from a workflow-level event filter.

    An unfiltered pull-request trigger covers the workflow event, but callers
    must still use ``container_ci_lane_failures`` to validate any conditional
    container job selected inside that workflow.
    """
    if pull_request_is_unfiltered(workflow_source):
        return []
    required = docker_context_inputs(dockerfile_source)
    filters = workflow_path_filters(workflow_source)
    return [
        f"{workflow_name}: Docker build input {path!r} is not covered by "
        "an event path filter"
        for path in sorted(required)
        if not any(_filter_covers(path, candidate) for candidate in filters)
    ]


def container_ci_lane_failures(
    dockerfile_source: str,
    policy: dict,
) -> list[str]:
    """Return Docker inputs that do not select the required container lane."""
    failures = []
    for path in sorted(docker_context_inputs(dockerfile_source)):
        classification = classify_ci_paths.classify_paths([path], policy)
        if "container" in classification.lanes:
            continue
        selected = ", ".join(classification.lanes) or "none"
        failures.append(
            ".github/ci-paths.json: Docker build input "
            f"{path!r} does not select the required 'container' lane "
            f"(selected: {selected})"
        )
    return failures


def main() -> int:
    dockerfile_source = DOCKERFILE.read_text(encoding="utf-8")
    policy = classify_ci_paths.load_policy(CI_PATH_POLICY)
    failures = production_dependency_contract_failures(dockerfile_source)
    failures.extend(container_ci_lane_failures(dockerfile_source, policy))
    for workflow in WORKFLOWS:
        failures.extend(container_workflow_input_failures(
            dockerfile_source,
            workflow.read_text(encoding="utf-8"),
            str(workflow.relative_to(ROOT)),
        ))
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        print(
            "container contract check FAILED — keep the lock-enforced "
            "dependency layer, Dockerfile COPY inputs, CI path lanes, and "
            "workflow path filters synchronized",
            file=sys.stderr,
        )
        return 1
    print(
        "container contract check passed "
        f"({len(docker_context_inputs(dockerfile_source))} build inputs)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
