# Validation: mechanical honesty gates

Date: 2026-10-20
Owner lane: qa
Gate: `tests/honesty_gate.py` + the `honesty` job in `.github/workflows/ci.yml`
Status: gating for `frames` and `exitcode`; advisory for `claims`

This record covers the checks, not the weather. It exists because the honesty
bar was culture: three of its rules are mechanically checkable,
and an unchecked rule is a rule that rots. The gate is static and hermetic --
no build, no Qt, no network -- so it cannot be skipped for being expensive.

## What runs

```
python tests/honesty_gate.py --self-test     # proves each gate can go red
python tests/honesty_gate.py --base <ref>    # the gates themselves
```

CI runs both on `ubuntu-latest` in a dedicated `honesty` job. `--base` is the
PR base sha on pull requests, the previous commit on pushes, and an unknown/first
commit is reported as `skipped`, never as a pass.

### Gate `frames` (gating)

The only canvas API that hands decoded radar moments to the paint path is
`RadarMapCanvas::setDecodedChunk`. In `tests/` and `tools/`, every call site must
carry an allow-annotation:

```
// honesty-allow: frames — one 1000-gate ray so the cursor probe has a
// finite dBZ readout; proves the hover/debounce path, no storm structure.
```

A missing annotation fails the gate. So does an annotation with no reviewable
reason, and an annotation naming a different gate. The gate prints every allow
annotation it accepted on every run, so an allow-list cannot grow quietly:
adding one is a visible diff line and a CI log line.

Current allow-list (2 sites, both in `tests/ui_smoke_test.cpp`): a single
1000-gate ray with uniform raw value 86, used so the cursor probe has a finite
dBZ readout. Both sites assert probe/hover/palette behaviour; neither asserts
storm structure. The chunk is minimal structure, not a fake volume.

### Gate `exitcode` (gating)

Scans `.github/workflows/*.yml`, `*.sh`, `*.bash`, `*.zsh`. Two patterns fail:

1. A pipeline containing a test binary or `ctest` whose status is read as `$?`
   on the next executed lines. `$?` is the last pipeline stage's status
   (`tail`'s), not the test's. This repo has already reported a crashing exe as
   a pass this way.
2. A pipeline containing a test binary with no `pipefail` in effect in that
   workflow step or script. Default `bash -e` and plain `/bin/sh` do not
   propagate a failing left-hand stage.

`${PIPESTATUS[...]}` on the pipeline line or in the next three lines is
accepted (bash only), and `pipefail` in the same `run:` block or script makes
the pipeline legitimate. A `pipefail` in a different workflow step does not
excuse the step under test. A grep line that merely quotes a test binary's name
does not fire; the detector requires the test command at a command position.

### Gate `claims` (advisory)

Diffs the added lines of `src/app` and `src/core` against `--base`. A line with
a numeric claim vocabulary (`POD`, `FAR`, `CSI`, `ground truth`, `accuracy`,
`skill score`, `validated`, `verified against`, `scored against`) plus a digit,
no negation, and no `docs/` reference is reported. If the same diff touches
`docs/`, the line is downgraded to a `review` note: the gate can see that a
record changed, not that the record covers the claim. `--strict-claims` turns
warnings into failures; CI does not pass that flag, deliberately.

Negations (`not validated`, `guidance only`, `TODO`) and plain numbers without
claim vocabulary are ignored.

## Failure demonstrated first

Reproducible pair, run on copies rather than on the working tree. (`--root`
scans an alternate tree; the gate is the same code path CI runs.)

Before the annotations, a copy of `tests/ui_smoke_test.cpp` with the two
`honesty-allow` lines removed:

```
[frames] FAIL — 0 annotated map injection(s), 2 unannotated
  tests/ui_smoke_test.cpp:2273 ... no allow-annotation
  tests/ui_smoke_test.cpp:4944 ... no allow-annotation
BEFORE_EXIT=1
```

With the annotations in place: `[frames] ok — 2 annotated map injection(s), 0
unannotated`, exit 0. The gate also caught its own self-test literals on the
first run (six findings, four of them strings inside the detector); the detector
file is now excluded from its own scan, which is why the before-pair above has
exactly two findings.

For the exit-code gate, a copy of the real `ci.yml` with the Test step replaced
by `ctest ... | tail -5` and `echo "exit=$?"` on the next line:

```
[exitcode] FAIL — .github/workflows/ci.yml:24: test result read through a pipe:
  `$?` after this pipeline is the last stage's status, not the test's
  (read at 25: echo "exit=$?")
BEFORE_EXIT=1
```

The unmutated workflow passes. The self-test is the standing version of these
mutation checks: 21/21 cases. It covers an unannotated injection, a justified
annotation, a reason-less annotation, a wrong-gate annotation, `$?` behind a
pipe, a piped test with no `pipefail`, a `pipefail` pipeline, a bare test run, a
piped `ctest` in a workflow `run:` block, a quoted test name in a grep, the
exact `ui_tests | tail` false green, `PIPESTATUS[0]`, `pipefail` in a longer
script, `pipefail` in another step not excusing this one, the claims diff with
and without a `docs/` change, the claim classifier, and the advisory-vs-fail
report state.

## What this gate deliberately does not catch

Broad gates get deleted; these omissions are on purpose, not oversights.

- **Fixture bytes are not pinned.** `tests/fixtures/` could be swapped for
  synthetic data without the gate noticing. A hash pin was rejected because it
  would block the decoder lane's legitimate fixture refreshes behind a QA-owned
  file. The gate prints a fixture census (name count + total bytes) on every run
  as visibility instead; `--verbose` lists every file.
- **Overlay frames are out of scope.** GOES, HRRR and mosaic tests build
  spec-shaped subwindows on purpose for georeference math; they are not radar
  moments. A future overlay gate needs its own allow-list and validation record.
- **`src/` injection is not gated.** Production `setDecodedChunk` call sites in
  `main_window.cpp` take decoder output; gating them would require an annotation
  on every normal load. The gate only guards test and tool code, which is where
  a fake frame has been tempting.
- **Prose build recipes in `*.md` are not linted.** A doc can still show a
  piped command example; only committed workflows and shell scripts are checked.
- **PowerShell is not checked.** `$LASTEXITCODE` is set by the last native
  command even through a pipeline, so the bash bug class does not transfer
  directly. `$?` in PowerShell after a pipeline is a different, untested shape.
- **A `docs/` touch is not a matching record.** The claims gate is advisory
  precisely because it cannot know whether `docs/whatever.md` covers the number
  next to it. A human still has to read it.
- **The YAML reader is a line scanner, not a parser.** It recognises `run:`
  scalars and block scalars at any indentation. Exotic YAML (anchors, `run:`
  built by `matrix` expansion, scripts invoked through other tools) can slip
  past. It has no false positives on the current workflows.
- **CI's build job does not install Qt.** `cappedweather_ui_tests` is only
  created when Qt6 is found (`CMakeLists.txt` line 160 onward), so `ctest` in
  the matrix job currently runs `cappedweather_core_tests` only. The honesty job
  is independent of that, but the UI suite is not yet covered by CI. Tracked
  separately; this gate does not pretend to close it.

## Maintenance

- Adding a legitimate in-memory map fixture: annotate the call site with
  `honesty-allow: frames — <what it proves and why it cannot be a real volume>`.
  Keep the reason one sentence; it is read by reviewers.
- Adding a gate: add a self-test case that fails without the gate logic, then
  wire it into `GATE_IDS` and this record.
- Removing a gate: delete its self-test cases and this section in the same
  commit. A silent removal is the thing this record exists to prevent.
