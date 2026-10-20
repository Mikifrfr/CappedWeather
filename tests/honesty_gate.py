#!/usr/bin/env python3
"""Mechanical honesty gates for CappedWeather.

The honesty bar was stated as culture. This script makes the three
checkable parts mechanical, so a future commit cannot quietly relax them:

  frames    No synthetic radar frames driven into the map. Any test/tool
            call that hands a DecodedChunk to RadarMapCanvas::setDecodedChunk
            must carry an explicit allow-annotation naming why the chunk is
            legitimate (usually: it is the minimal spec-shaped structure a
            probe/paint assertion needs, never a fake storm).
  exitcode  No test result read through a pipe. A test binary or ctest piped
            into tail/tee/grep reports the *last stage's* status, not the
            test's; this repo has already shipped a crash that read as a pass
            that way. A pipeline is accepted only when `pipefail` is set in the
            same run block/line, or the status is read as `${PIPESTATUS[0]}`.
  claims    A diff that adds a numeric capability claim (POD/FAR/CSI/percentage/
            cm/validated) to src/app or src/core without touching docs/ is
            reported. This one is ADVISORY by design: prose comments carry
            engineering tolerances that are not public capability claims.
            --strict-claims turns it into a failure.

Allow annotation grammar (comment style is yours; any of //, #, <!-- -->):

    // honesty-allow: frames — one 1000-gate ray so the cursor probe has a
    //                        value; proves the probe path only, no morphology.

Rules for the annotation:
  * the gate id after `honesty-allow:` must match the gate that fires;
  * the separator must be an em dash, `--`, or `-`;
  * the reason must be at least 8 characters of prose (TODO/FIXME/none/empty
    are rejected -- an annotation with no reason is worse than none);
  * it must sit on the flagged line or within 4 lines above it.

The gate deliberately does not catch (documented in
docs/validation-honesty-gate.md):
  * a synthetic VolumeBrick/Level2 file written to disk by a tool;
  * overlay frames (GOES/HRRR/mosaic) -- hermetic georeference tests build
    spec-shaped subwindows on purpose and are not radar moments;
  * fixture bytes swapped for synthetic ones (a hash pin would gate other
    lanes' legitimate fixture refresh; the fixture census is reported instead);
  * prose build recipes in *.md, and PowerShell `$LASTEXITCODE` (which keeps a
    native command's code through a pipeline);
  * whether a docs/ record is actually a matching record for a given claim --
    the claims gate only checks that the diff touched docs/.

Usage:
    python tests/honesty_gate.py                     # all gates, working tree
    python tests/honesty_gate.py --base origin/main  # claims diff base
    python tests/honesty_gate.py --self-test         # mutation self-check
    python tests/honesty_gate.py --root <dir>        # scan an alternate tree
    python tests/honesty_gate.py --verbose           # fixture census
"""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

GATE_IDS = ("frames", "exitcode", "claims")

# --- shared helpers -------------------------------------------------------

ALLOW_RE = re.compile(r"honesty-allow:\s*([a-z][a-z0-9_-]*)\s*(?:\u2014|--|-)\s*(\S.*?)\s*$")
ALLOW_LOOKBACK = 4
ALLOW_MIN_REASON = 8
ALLOW_REJECTED_REASON = re.compile(r"^(todo|fixme|n/a|na|none|test|because)\b", re.IGNORECASE)

SKIP_DIRS = {".git", "build", "dist", "node_modules", "__pycache__", ".worktrees"}


@dataclass(frozen=True)
class Finding:
    gate: str
    path: str
    line: int
    message: str
    text: str = ""

    def render(self) -> str:
        return f"{self.path}:{self.line}: [{self.gate}] {self.message}" + (
            f'\n    {self.text.strip()}' if self.text.strip() else ""
        )


@dataclass(frozen=True)
class AllowNote:
    path: str
    line: int
    gate: str
    reason: str


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def iter_files(root: Path, suffixes: set[str], subdirs: tuple[str, ...] = ()) -> list[Path]:
    bases = [root / d for d in subdirs] if subdirs else [root]
    found: list[Path] = []
    for base in bases:
        if not base.exists():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for name in filenames:
                path = Path(dirpath) / name
                if path.suffix.lower() in suffixes:
                    found.append(path)
    found.sort()
    return found


def rel(root: Path, path: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def _comment_text(line: str) -> str | None:
    stripped = line.strip()
    for marker in ("//", "#", "<!--"):
        if stripped.startswith(marker):
            text = stripped[len(marker):].strip()
            if text.endswith("-->"):
                text = text[:-3].strip()
            return text
    return None


def find_allow(lines: list[str], index: int, gate: str) -> tuple[str | None, str]:
    """Return (reason, error). Reason None means no valid annotation found."""
    start = max(0, index - ALLOW_LOOKBACK)
    for i in range(index, start - 1, -1):
        match = ALLOW_RE.search(lines[i])
        if not match:
            continue
        if match.group(1) != gate:
            return None, f"annotation names gate '{match.group(1)}', expected '{gate}'"
        parts = [match.group(2).strip()]
        # A wrapped annotation continues on the comment lines between the
        # `honesty-allow:` line and the flagged code; join them for review.
        for k in range(i + 1, index):
            continuation = _comment_text(lines[k])
            if continuation:
                parts.append(continuation)
        reason = " ".join(part for part in parts if part).strip()
        if len(reason) < ALLOW_MIN_REASON:
            return None, "annotation reason is too short to review"
        if ALLOW_REJECTED_REASON.match(reason):
            return None, f"annotation reason '{reason}' does not explain anything"
        return reason, ""
    return None, ""


# --- gate: no synthetic radar frames --------------------------------------


FRAME_SUFFIXES = {".cpp", ".cc", ".cxx", ".c", ".hpp", ".hh", ".h", ".py", ".ps1", ".sh"}
FRAME_SCAN_DIRS = ("tests", "tools")
# The only canvas API that hands decoded radar moments to the paint path.
# A test that decodes a committed real volume through the normal decoder and
# then calls this is legitimate; the annotation records that provenance.
FRAME_INJECTION_RE = re.compile(r"\bsetDecodedChunk\s*\(")
# The gate's own self-test embeds literal calls as fixtures; they are strings,
# not a code path, and scanning them would make the gate fail on itself.
FRAME_SKIP_FILES = {"tests/honesty_gate.py"}


def gate_frames(root: Path) -> tuple[list[Finding], list[AllowNote]]:
    findings: list[Finding] = []
    notes: list[AllowNote] = []
    for path in iter_files(root, FRAME_SUFFIXES, FRAME_SCAN_DIRS):
        where = rel(root, path)
        if where in FRAME_SKIP_FILES:
            continue
        lines = read_text(path).splitlines()
        for index, line in enumerate(lines):
            if not FRAME_INJECTION_RE.search(line):
                continue
            reason, error = find_allow(lines, index, "frames")
            if reason is None:
                findings.append(
                    Finding(
                        gate="frames",
                        path=where,
                        line=index + 1,
                        message=(
                            "DecodedChunk handed to the map with no allow-annotation. "
                            "Drive the map from a real decoded volume, or add "
                            "'// honesty-allow: frames \u2014 <why this chunk is legitimate>'"
                            + (f" ({error})" if error else "")
                        ),
                        text=line,
                    )
                )
            else:
                notes.append(AllowNote(path=where, line=index + 1, gate="frames", reason=reason))
    return findings, notes


# --- gate: no test status read through a pipe -----------------------------

PIPE_SUFFIXES = {".yml", ".yaml", ".sh", ".bash", ".zsh"}
TEST_CMD_RE = re.compile(
    r"(?:^|[;&]|\(|\bthen\b|\bdo\b)\s*(?:\./)?(?:[\w.-]+/)*"
    r"(?:cappedweather_[a-z0-9_]*tests|ctest|run_m5_suite\.py|m5_verify\.py)\b"
)
PIPEFAIL_RE = re.compile(r"pipefail")
PIPE_STATUS_RE = re.compile(r"\$\?")


def _single_pipe(line: str) -> bool:
    """True when the line contains a shell pipe (not ||, not inside a string)."""
    # Strip the common quoted-string cases that would otherwise look like pipes.
    scrubbed = re.sub(r"'[^']*'", "''", line)
    scrubbed = re.sub(r'"[^"]*"', '""', scrubbed)
    return "|" in scrubbed.replace("||", "")


def _workflow_run_blocks(lines: list[str]) -> list[tuple[int, int]]:
    """Return (start, end) 1-based inclusive ranges of YAML `run:` scalars/blocks."""
    blocks: list[tuple[int, int]] = []
    i = 0
    while i < len(lines):
        match = re.match(r"^(\s*)(?:-\s+)?run:\s*(.*)$", lines[i])
        if not match:
            i += 1
            continue
        indent = len(match.group(1))
        rest = match.group(2).strip()
        if rest in ("|", ">", "|-", ">-", "|+", ">+"):
            j = i + 1
            while j < len(lines):
                if not lines[j].strip():
                    j += 1
                    continue
                lead = len(lines[j]) - len(lines[j].lstrip())
                if lead <= indent:
                    break
                j += 1
            # Content lives on 0-based i+1 .. j-1, i.e. 1-based i+2 .. j.
            if j > i + 1:
                blocks.append((i + 2, j))
            i = j
        else:
            blocks.append((i + 1, i + 1))
            i += 1
    return blocks


def gate_exitcode(root: Path) -> tuple[list[Finding], int]:
    findings: list[Finding] = []
    scanned = 0
    paths = iter_files(root, PIPE_SUFFIXES)
    for path in paths:
        text = read_text(path)
        scanned += 1
        lines = text.splitlines()
        where = rel(root, path)
        is_workflow = "/.github/workflows/" in f"/{where}"
        blocks = _workflow_run_blocks(lines) if is_workflow else [(1, len(lines))]

        for start, end in blocks:
            block_has_pipefail = any(
                PIPEFAIL_RE.search(lines[k - 1]) for k in range(start, end + 1)
            )
            for idx in range(start - 1, end):
                line = lines[idx]
                if line.lstrip().startswith("#"):
                    continue
                if not TEST_CMD_RE.search(line) or not _single_pipe(line):
                    continue
                if "PIPESTATUS" in line:
                    continue
                status_read = ""
                if PIPE_STATUS_RE.search(line):
                    status_read = f"{idx + 1}: {line.strip()}"
                else:
                    for j in range(idx + 1, min(idx + 4, len(lines))):
                        nxt = lines[j].strip()
                        if not nxt or nxt.startswith("#"):
                            continue
                        if PIPE_STATUS_RE.search(nxt):
                            status_read = f"{j + 1}: {lines[j].strip()}"
                        break
                if status_read:
                    findings.append(
                        Finding(
                            gate="exitcode",
                            path=where,
                            line=idx + 1,
                            message=(
                                "test result read through a pipe: `$?` after this pipeline is the "
                                f"last stage's status, not the test's (read at {status_read})"
                            ),
                            text=line,
                        )
                    )
                    continue
                # `${PIPESTATUS[0]}` keeps the test's own status and is the
                # approved way to inspect a piped run (bash only).
                if any(
                    "PIPESTATUS" in lines[k]
                    for k in range(idx + 1, min(idx + 4, len(lines)))
                ):
                    continue
                # `pipefail` only protects the shell that sets it: the same
                # `run:` block/line, never another step in the same file.
                if block_has_pipefail or PIPEFAIL_RE.search(line):
                    continue
                findings.append(
                    Finding(
                        gate="exitcode",
                        path=where,
                        line=idx + 1,
                        message=(
                            "test binary piped without `pipefail`: a failing test would be masked "
                            "by the last pipeline stage"
                        ),
                        text=line,
                    )
                )
    return findings, scanned


# --- gate: numeric capability claim without a docs record (advisory) ------

CLAIM_RE = re.compile(
    r"\b(POD|FAR|CSI|ground\s+truth|accuracy|skill\s+score|validated|verified\s+against|"
    r"scored\s+against)\b",
    re.IGNORECASE,
)
NEGATED_RE = re.compile(
    r"(not\s+validated|unvalidated|no\s+validation|never\s+validated|without\s+validation|"
    r"guidance\s+only|not\s+a\s+(?:claim|validation)|TODO|FIXME)",
    re.IGNORECASE,
)
NUMERIC_RE = re.compile(r"\d")
CLAIM_SCOPE = ("src/app", "src/core")
CLAIM_SUFFIXES = {".cpp", ".hpp", ".h", ".cc"}


def line_is_claim(text: str) -> bool:
    if "docs/" in text or NEGATED_RE.search(text):
        return False
    if not NUMERIC_RE.search(text):
        return False
    return bool(CLAIM_RE.search(text))


def _git(root: Path, *args: str) -> tuple[int, str]:
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return proc.returncode, proc.stdout
    except OSError as exc:  # git not installed
        return 127, str(exc)


def diff_added_lines(root: Path, base: str) -> tuple[list[tuple[str, int, str]], list[str]]:
    """Return ([(path, new_line_no, added_line)], docs_touched) for base..HEAD."""
    code, _ = _git(root, "rev-parse", "--verify", f"{base}^{{commit}}")
    if code != 0:
        raise ValueError(f"unknown diff base '{base}'")
    code, out = _git(root, "diff", "--unified=0", f"{base}..HEAD", "--", *CLAIM_SCOPE)
    if code != 0:
        raise ValueError(f"git diff failed for base '{base}'")
    added: list[tuple[str, int, str]] = []
    current = ""
    new_line = 0
    for line in out.splitlines():
        if line.startswith("+++ b/"):
            current = line[6:]
        elif line.startswith("@@"):
            match = re.search(r"\+(\d+)(?:,\d+)?", line)
            new_line = int(match.group(1)) if match else 0
        elif line.startswith("+") and not line.startswith("+++"):
            added.append((current, new_line, line[1:]))
            new_line += 1
        elif line.startswith(" "):
            new_line += 1
    _, docs = _git(root, "diff", "--name-only", f"{base}..HEAD", "--", "docs")
    docs_touched = [d.strip() for d in docs.splitlines() if d.strip()]
    return added, docs_touched


def gate_claims(root: Path, base: str | None) -> tuple[list[Finding], list[Finding], str]:
    if not base:
        return [], [], "skipped (no --base ref; pass a git ref to check a diff)"
    try:
        added, docs_touched = diff_added_lines(root, base)
    except ValueError as exc:
        return [], [], f"skipped ({exc})"
    findings: list[Finding] = []
    reviews: list[Finding] = []
    checked = 0
    for path, lineno, text in added:
        if not path or Path(path).suffix.lower() not in CLAIM_SUFFIXES:
            continue
        checked += 1
        if not line_is_claim(text):
            continue
        if docs_touched:
            reviews.append(
                Finding(
                    gate="claims",
                    path=path,
                    line=lineno,
                    message=(
                        "claim line meets a docs change in this diff "
                        f"({', '.join(docs_touched[:3])}); confirm the record covers it"
                    ),
                    text=text,
                )
            )
            continue
        findings.append(
            Finding(
                gate="claims",
                path=path,
                line=lineno,
                message="numeric capability claim with no docs/ record in this diff",
                text=text,
            )
        )
    scope = f"base {base}, {checked} added production lines"
    if docs_touched:
        scope += f", docs touched: {len(docs_touched)}"
    return findings, reviews, scope


# --- fixture census (report only) -----------------------------------------

FIXTURE_SUFFIXES = {
    ".nc", ".h5", ".hdf5", ".png", ".wld", ".json", ".shp", ".dbf", ".shx",
    ".bin", ".l2", ".ar2v", ".gz", ".zip", ".csv", ".txt", ".md",
}


def fixture_census(root: Path) -> tuple[int, int, list[str]]:
    directory = root / "tests" / "fixtures"
    if not directory.exists():
        return 0, 0, []
    files = sorted(p for p in directory.rglob("*") if p.is_file())
    total = sum(p.stat().st_size for p in files)
    return len(files), total, [rel(root, p) for p in files]


# --- runner ---------------------------------------------------------------


@dataclass
class GateResult:
    gate: str
    findings: list[Finding]
    scope: str
    notes: list[AllowNote]
    reviews: list[Finding] = field(default_factory=list)


def run_gates(root: Path, base: str | None, gates: list[str]) -> list[GateResult]:
    results: list[GateResult] = []
    if "frames" in gates:
        findings, notes = gate_frames(root)
        results.append(
            GateResult(
                gate="frames",
                findings=findings,
                scope=f"{len(notes)} annotated map injection(s), {len(findings)} unannotated",
                notes=notes,
            )
        )
    if "exitcode" in gates:
        findings, scanned = gate_exitcode(root)
        results.append(
            GateResult(
                gate="exitcode",
                findings=findings,
                scope=f"{scanned} workflow/script file(s) scanned",
                notes=[],
            )
        )
    if "claims" in gates:
        findings, reviews, scope = gate_claims(root, base)
        results.append(GateResult(gate="claims", findings=findings, scope=scope, notes=[], reviews=reviews))
    return results


def print_report(root: Path, results: list[GateResult], verbose: bool, strict_claims: bool) -> None:
    print(f"CappedWeather honesty gate \u2014 tree {root}")
    for result in results:
        if result.findings and result.gate == "claims" and not strict_claims:
            state = "ADVISORY"
        elif result.findings:
            state = "FAIL"
        else:
            state = "ADVISORY" if result.gate == "claims" else "ok"
        print(f"[{result.gate}] {state} \u2014 {result.scope}")
        for note in result.notes:
            print(f"  allow {note.path}:{note.line} \u2014 {note.reason}")
        for finding in result.findings:
            print("  " + finding.render().replace("\n", "\n  "))
        for review in result.reviews:
            print("  review " + review.render().replace("\n", "\n  "))
    count, total, names = fixture_census(root)
    print(f"fixtures: {count} file(s), {total:,} bytes (census only; bytes are not pinned)")
    if verbose:
        for name in names:
            print(f"  {name}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Mechanical honesty gates for CappedWeather")
    parser.add_argument("--root", default=None, help="repository root to scan (default: this repo)")
    parser.add_argument("--base", default=None, help="git ref the claims gate diffs against")
    parser.add_argument("--gate", default=",".join(GATE_IDS), help="comma-separated gate ids")
    parser.add_argument("--strict-claims", action="store_true", help="make claims warnings fail")
    parser.add_argument("--self-test", action="store_true", help="run the mutation self-check")
    parser.add_argument("--verbose", action="store_true", help="list every fixture")
    return parser.parse_args(argv)


# --- self-test ------------------------------------------------------------


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _write_commit(repo: Path, path: str, text: str, extra: tuple[str, str] | None = None) -> bool:
    """Write one (or two) files into a scratch repo and commit them."""
    _write(repo / path, text)
    if extra:
        _write(repo / extra[0], extra[1])
    for args in (("add", "-A"), ("commit", "-q", "-m", "self-test case")):
        proc = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
        if proc.returncode != 0:
            return False
    return True


def self_test() -> int:
    cases: list[tuple[str, bool]] = []
    with tempfile.TemporaryDirectory(prefix="honesty-gate-selftest-") as tmp:
        root = Path(tmp)

        # 1. Unannotated frame injection must be caught.
        _write(
            root / "tests" / "bad_map.cpp",
            "void f(RadarMapCanvas& c) {\n  c.setDecodedChunk(chunk, now);\n}\n",
        )
        findings, notes = gate_frames(root)
        cases.append(("frames: unannotated setDecodedChunk fails", len(findings) == 1 and not notes))

        # 2. A justified annotation must be accepted.
        _write(
            root / "tests" / "bad_map.cpp",
            "void f(RadarMapCanvas& c) {\n"
            "  // honesty-allow: frames \u2014 one ray so the probe has a gate; no morphology.\n"
            "  c.setDecodedChunk(chunk, now);\n}\n",
        )
        findings, notes = gate_frames(root)
        cases.append(("frames: annotated injection passes", not findings and len(notes) == 1))

        # 3. An annotation with no reviewable reason must still fail.
        _write(
            root / "tests" / "bad_map.cpp",
            "void f(RadarMapCanvas& c) {\n"
            "  // honesty-allow: frames -\n"
            "  c.setDecodedChunk(chunk, now);\n}\n",
        )
        findings, _ = gate_frames(root)
        cases.append(("frames: empty-reason annotation fails", len(findings) == 1))

        # 4. Annotation for the wrong gate must not satisfy this gate.
        _write(
            root / "tests" / "bad_map.cpp",
            "void f(RadarMapCanvas& c) {\n"
            "  // honesty-allow: claims \u2014 this is not a frames annotation.\n"
            "  c.setDecodedChunk(chunk, now);\n}\n",
        )
        findings, _ = gate_frames(root)
        cases.append(("frames: wrong-gate annotation fails", len(findings) == 1))

        # 5. Shell script that reads $? behind a test pipeline must be caught.
        case = root / "pipe_status"
        _write(
            case / "run.sh",
            "#!/bin/sh\nset -e\n./build/Release/cappedweather_core_tests | tail -5\n"
            "if [ $? -ne 0 ]; then\n  echo failed\nfi\n",
        )
        findings, _ = gate_exitcode(case)
        cases.append(("exitcode: $? behind a pipeline fails", len(findings) == 1))

        # 6. A bare pipeline with no status read still hides failure (bash -e).
        case = root / "pipe_hidden"
        _write(case / "run.sh", "#!/bin/sh\nset -e\n./build/Release/cappedweather_core_tests | tee log\n")
        findings, _ = gate_exitcode(case)
        cases.append(("exitcode: piped test without pipefail fails", len(findings) == 1))

        # 7. pipefail makes the same pipeline legitimate.
        case = root / "pipe_guarded"
        _write(
            case / "run.sh",
            "#!/bin/sh\nset -euo pipefail\n./build/Release/cappedweather_core_tests | tee log\n",
        )
        findings, _ = gate_exitcode(case)
        cases.append(("exitcode: pipefail pipeline passes", not findings))

        # 8. Unpiped test run is the approved recipe.
        case = root / "bare_run"
        _write(case / "run.sh", "#!/bin/sh\n./build/Release/cappedweather_core_tests\n")
        findings, _ = gate_exitcode(case)
        cases.append(("exitcode: bare test run passes", not findings))

        # 9. Workflow step that pipes ctest without pipefail must be caught.
        case = root / "workflow_pipe"
        _write(
            case / ".github" / "workflows" / "ci.yml",
            "jobs:\n  t:\n    steps:\n      - name: Test\n        run: |\n"
            "          ctest --test-dir build | tee test.log\n",
        )
        findings, _ = gate_exitcode(case)
        cases.append(("exitcode: piped ctest in workflow fails", len(findings) == 1))

        # 10. The gate must not fire on a grep that merely mentions a test name.
        case = root / "quoted_grep"
        _write(case / "run.sh", "#!/bin/sh\nset -e\ngrep -r 'cappedweather_core_tests' notes | head\n")
        findings, _ = gate_exitcode(case)
        cases.append(("exitcode: quoted test name in grep is ignored", not findings))

        # 11. The exact false green already seen on this machine: ui tests piped
        #     into tail, then $? read. tail's status is the one that survives.
        case = root / "tail_false_green"
        _write(
            case / "run.sh",
            "#!/bin/sh\n"
            "./build/Release/cappedweather_ui_tests -platform offscreen | tail -5\n"
            "status=$?\n",
        )
        findings, _ = gate_exitcode(case)
        cases.append(("exitcode: ui tests piped to tail with $? fails", len(findings) == 1))

        # 12. PIPESTATUS[0] is the approved way to read a piped test's status.
        case = root / "pipestatus_ok"
        _write(
            case / "run.sh",
            "#!/bin/sh\n"
            "./build/Release/cappedweather_core_tests | tee log\n"
            "status=${PIPESTATUS[0]}\n",
        )
        findings, _ = gate_exitcode(case)
        cases.append(("exitcode: PIPESTATUS[0] pipeline passes", not findings))

        # 13. pipefail must protect the shell it runs in, and the gate must not
        #     mistake the following unrelated line for a $? read.
        case = root / "pipefail_longer"
        _write(
            case / "run.sh",
            "#!/bin/sh\nset -euo pipefail\n"
            "./build/Release/cappedweather_core_tests | tee log\necho done\n",
        )
        findings, _ = gate_exitcode(case)
        cases.append(("exitcode: pipefail pipeline in a longer script passes", not findings))

        # 14. A pipefail in another workflow step must not excuse this step.
        case = root / "workflow_other_pipefail"
        _write(
            case / ".github" / "workflows" / "ci.yml",
            "jobs:\n  t:\n    steps:\n"
            "      - name: Good\n        run: |\n"
            "          set -o pipefail\n          ctest --test-dir build | tee a.log\n"
            "      - name: Bad\n        run: |\n"
            "          ctest --test-dir build | tee b.log\n",
        )
        findings, _ = gate_exitcode(case)
        cases.append(("exitcode: pipefail in another step does not excuse", len(findings) == 1))

        # 15. Claims gate end-to-end against a real git diff.
        repo = root / "claims_repo"
        repo.mkdir()

        def git_run(*args: str) -> int:
            return subprocess.run(
                ["git", "-C", str(repo), *args], capture_output=True, text=True
            ).returncode

        ready = (
            git_run("init", "-q", "-b", "main") == 0
            and git_run("config", "user.email", "gate@example.test") == 0
            and git_run("config", "user.name", "honesty-gate") == 0
        )
        ready = ready and _write_commit(repo, "README.md", "gate fixture base\n")
        ready = ready and _write_commit(repo, "src/app/claim.cpp", "// accuracy 91% verified against 40 cases\n")
        if ready:
            findings, _, _ = gate_claims(repo, "HEAD~1")
            cases.append(
                (
                    "claims: diff without a docs record warns",
                    len(findings) == 1 and findings[0].line == 1,
                )
            )
            ready = _write_commit(
                repo,
                "src/app/claim2.cpp",
                "// POD 62% over 26 reports\n",
                extra=("docs/validation-x.md", "# Validation: gate fixture\n"),
            )
            if ready:
                findings, reviews, _ = gate_claims(repo, "HEAD~1")
                cases.append(
                    (
                        "claims: same-diff docs record downgrades to review",
                        not findings and len(reviews) == 1,
                    )
                )
            else:
                print("  SKIP  claims: could not commit the docs companion")
        else:
            print("  SKIP  claims: git unavailable for the end-to-end diff case")

        # 12. Claim classifier.
        cases.append(
            (
                "claims: 'hail 2.5 cm validated' is a claim",
                line_is_claim("// Hail size validated at 2.5 cm against 41 reports"),
            )
        )
        cases.append(
            (
                "claims: 'not validated' is not a claim",
                not line_is_claim("// Hail size is not validated; guidance only for now"),
            )
        )
        cases.append(
            (
                "claims: docs reference counts as satisfied",
                not line_is_claim("// POD 62% (see docs/validation-2025-05-16.md)"),
            )
        )
        cases.append(("claims: plain number is not a claim", not line_is_claim("// 1000 gates per ray")))

        # 13. A claims-only finding must print ADVISORY while the exit stays 0,
        #     and print FAIL when --strict-claims asked it to fail.
        claims_result = [
            GateResult(
                gate="claims",
                findings=[
                    Finding(
                        gate="claims",
                        path="src/app/x.cpp",
                        line=1,
                        message="numeric capability claim",
                        text="// POD 62%",
                    )
                ],
                scope="unit fixture",
                notes=[],
            )
        ]
        advisory_text = io.StringIO()
        with contextlib.redirect_stdout(advisory_text):
            print_report(root, claims_result, False, False)
        strict_text = io.StringIO()
        with contextlib.redirect_stdout(strict_text):
            print_report(root, claims_result, False, True)
        cases.append(
            (
                "claims report: ADVISORY without strict, FAIL with strict",
                "[claims] ADVISORY" in advisory_text.getvalue()
                and "[claims] FAIL" in strict_text.getvalue(),
            )
        )

    failures = [name for name, ok in cases if not ok]
    for name, ok in cases:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    print(f"self-test: {len(cases) - len(failures)}/{len(cases)} cases behave as designed")
    return 0 if not failures else 1


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.self_test:
        return self_test()
    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parent.parent
    gates = [g.strip() for g in args.gate.split(",") if g.strip()]
    unknown = [g for g in gates if g not in GATE_IDS]
    if unknown:
        print(f"unknown gate id(s): {', '.join(unknown)}", file=sys.stderr)
        return 2
    results = run_gates(root, args.base, gates)
    print_report(root, results, args.verbose, args.strict_claims)
    failed = [r.gate for r in results if r.findings and (r.gate != "claims" or args.strict_claims)]
    if failed:
        print(f"\nFAILED: {', '.join(failed)}")
        return 1
    print("\nPASS: all gating checks clean")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
