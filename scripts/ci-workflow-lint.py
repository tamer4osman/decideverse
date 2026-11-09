#!/usr/bin/env python3
"""Structural validation of .github/workflows/ci.yml for the release pipeline.

Checks the acceptance criteria that a plain YAML parse cannot catch:
job graph, action pins, artifact name/path consistency, forbidden patterns.
Exit 0 = all checks pass; exit 1 = at least one check failed.
"""
import re
import sys

import yaml

PATH = ".github/workflows/ci.yml"
text = open(PATH, encoding="utf-8").read()
wf = yaml.safe_load(text)

checks, failures = [], []


def check(cond, label):
    checks.append(label)
    if not cond:
        failures.append(label)
        print(f"FAIL: {label}")
    else:
        print(f"ok  : {label}")


jobs = wf["jobs"]
on = wf[True] if True in wf else wf["on"]  # PyYAML quirk: bare `on` -> True key

# Raw text with full-line comments stripped, for literal-pattern checks
# (the workflow header documents the REMOVED defects, so comments must
# not trip the forbidden-pattern checks).
code_lines = [re.sub(r"\s+#.*$", "", ln) for ln in text.splitlines()
              if not ln.lstrip().startswith("#")]
code = "\n".join(code_lines)

# --- top-level structure --------------------------------------------------
check(sorted(jobs) == sorted([
    "source-verification", "native", "wasm", "functional-parity",
    "browser", "benchmarks", "gamelab-node-tests", "gamelab-browser-tests",
    "release-report"]), "exactly the nine expected jobs (seven Stage 1 + two Stage 2)")
check("push" in on and "pull_request" in on and "workflow_dispatch" in on,
      "triggers: push + pull_request + workflow_dispatch")
check(wf.get("permissions") == {"contents": "read"},
      "minimal top-level permissions (contents: read)")
check(wf["env"]["EMSCRIPTEN_VERSION"] == "7.0.0", "pinned Emscripten version 7.0.0")
check(wf["env"]["NODE_VERSION"] == "24", "Node major pinned (24)")

# --- dependency graph ------------------------------------------------------
check(jobs["functional-parity"]["needs"] == ["native", "wasm"],
      "D needs [native, wasm]")
check(jobs["browser"]["needs"] == ["wasm"], "E needs [wasm]")
check(jobs["benchmarks"]["needs"] == ["wasm"], "F needs [wasm]")
check(jobs["gamelab-node-tests"]["needs"] == ["wasm"], "H needs [wasm]")
check(jobs["gamelab-browser-tests"]["needs"] == ["wasm"], "I needs [wasm]")
check(jobs["gamelab-browser-tests"]["runs-on"] == "ubuntu-24.04",
      "I pinned to the established Playwright-compatible runner image")
check(jobs["release-report"]["needs"] == [
    "source-verification", "native", "wasm", "functional-parity",
    "browser", "benchmarks", "gamelab-node-tests", "gamelab-browser-tests"],
    "G needs all eight mandatory jobs (six Stage 1 + two Stage 2)")

# --- forbidden patterns (checked against executable content only) -----------
check("|| true" not in code, "no '|| true' anywhere")
check("continue-on-error" not in code, "no continue-on-error")
check("/home/runner" not in code, "no undocumented /home/runner paths")
check("mymindstorm/setup-emsdk@" not in code, "outdated action reference removed")

# --- action pinning (immutable commit SHAs + version comments) --------------
def uses_of(job):
    return [s.get("uses", "") for s in jobs[job]["steps"] if "uses" in s]

all_uses = [u for j in jobs for u in uses_of(j)]
pin_re = re.compile(r"^[\w.-]+/[\w.-]+@[0-9a-f]{40}$")
bad = [u for u in all_uses if not pin_re.match(u)]
check(not bad, f"every uses pinned to a 40-hex commit SHA (bad: {bad})")
# Version comments live in the raw YAML next to each uses line (raw
# lines, not comment-stripped, since the comment is the thing checked).
uses_lines = [ln for ln in text.splitlines() if ln.strip().startswith("uses:")]
sha_comment_re = re.compile(r"uses:\s*[\w./-]+@[0-9a-f]{40}\s+#\s*v?\d")
no_comment = [ln.strip() for ln in uses_lines if not sha_comment_re.search(ln)]
check(not no_comment, f"every pinned action carries a readable version comment (bad: {no_comment})")

emsdk_uses = [u for u in all_uses if "setup-emsdk" in u]
check(len(emsdk_uses) == 2 and all(u.startswith("emscripten-core/setup-emsdk@") for u in emsdk_uses),
      "exactly two setup-emsdk steps (Jobs C and D), from emscripten-core org")
check(len(set(emsdk_uses)) == 1, "both emsdk steps pinned to the identical commit SHA")

# --- emsdk input syntax ------------------------------------------------------
emsdk_steps = [s for j in jobs for s in jobs[j]["steps"]
               if "setup-emsdk" in s.get("uses", "")]
for s in emsdk_steps:
    with_ = s.get("with", {})
    check(with_.get("version") == "${{ env.EMSCRIPTEN_VERSION }}",
          "setup-emsdk uses env.EMSCRIPTEN_VERSION input (v16 input syntax)")

# --- artifact architecture ---------------------------------------------------
uploads = {s["with"]["name"]: s["with"]["path"]
           for j in jobs for s in jobs[j]["steps"]
           if s.get("uses", "").startswith("actions/upload-artifact")}
downloads = [(j, s["with"]["name"], s["with"].get("path", ""))
             for j in jobs for s in jobs[j]["steps"]
             if s.get("uses", "").startswith("actions/download-artifact")
             and "name" in s.get("with", {})]

check("omnidecide-wasm-dist" in uploads and uploads["omnidecide-wasm-dist"] == "dist",
      "omnidecide-wasm-dist uploads exactly dist/ (no build/ paths mixed in)")
check("omnidecide-wasm-checksums" in uploads and uploads["omnidecide-wasm-checksums"] == "build/wasm-checksums.txt",
      "checksum report uploaded as a separate artifact")

for job, name, path in downloads:
    if name == "omnidecide-wasm-dist":
        if job == "functional-parity":
            check(path == "artifacts/ci-wasm", "Job D downloads dist into artifacts/ci-wasm")
        else:
            check(path == "dist", f"Job {job} downloads dist into dist/")
    elif name == "omnidecide-native-parity":
        check(job == "functional-parity" and path == "artifacts/ci-native",
              "Job D downloads the Job B parity runner into artifacts/ci-native")
    elif name == "omnidecide-wasm-checksums":
        check(job == "functional-parity" and path == "artifacts/ci-checksums",
              "Job D downloads checksum report into artifacts/ci-checksums")

# --- genuine fresh-build comparison -----------------------------------------
check("build.sh --output-dir build/fresh-wasm" in text,
      "Job D fresh-builds into isolated build/fresh-wasm via --output-dir")
check("cmp artifacts/ci-wasm/omnidecide.wasm build/fresh-wasm/omnidecide.wasm" in text,
      "cmp byte comparison of .wasm (downloaded vs fresh)")
check("cmp artifacts/ci-wasm/omnidecide.js build/fresh-wasm/omnidecide.js" in text,
      "cmp byte comparison of .js (downloaded vs fresh)")
check("ci-integrity.mjs" in text, "machine-readable integrity report generated")
check("ci-verify-dist.sh" in text, "shared dist verifier used after build/download")
check("0061736d" in open("scripts/ci-verify-dist.sh", encoding="utf-8").read(),
      "WASM magic bytes (\\0asm) verified by the shared verifier")
check("sha256sum -c -" in text, "downloaded artifact cross-checked against CI checksum report")

# --- release gate -------------------------------------------------------------
gate = [s for s in jobs["release-report"]["steps"] if "Fail the pipeline" in s.get("name", "")][0]
script = gate["run"]
for j in ["source-verification", "native", "wasm", "functional-parity", "browser",
          "benchmarks", "gamelab-node-tests", "gamelab-browser-tests"]:
    check(f"needs.{j}.result" in script, f"gate checks Job {j} result")

# --- Stage 2.1.1: shell permission restoration in Jobs H and I ---------------
# The Git index stores every shell script with mode 100644, so a fresh
# checkout has no executable bits; each new job must restore them before
# its first direct script invocation (scripts/ci-verify-dist.sh).
for j in ["gamelab-node-tests", "gamelab-browser-tests"]:
    steps = jobs[j]["steps"]
    perm_idx = [i for i, s in enumerate(steps)
                if s.get("name") == "Ensure shell scripts have executable permissions"]
    check(len(perm_idx) == 1, f"Job {j} restores shell script permissions exactly once")
    if perm_idx:
        first_run = next((i for i, s in enumerate(steps) if "run" in s), None)
        check(first_run == perm_idx[0],
              f"Job {j} restores permissions before any direct script invocation")
        chmod_cmd = steps[perm_idx[0]].get("run", "").strip()
        check(chmod_cmd == "chmod +x build.sh build-native.sh fetch-kernel.sh scripts/*.sh",
              f"Job {j} permission step covers build scripts and scripts/*.sh")

# --- Stage 2.1.1: missing-report gate ------------------------------------
# The consolidated release gate must distinguish "a test failed" from
# "the report is missing" and fail on both.
missing_gate = [s for s in jobs["release-report"]["steps"]
                if "required test reports are missing" in s.get("name", "")]
check(len(missing_gate) == 1, "release gate has a dedicated missing-report step")
if missing_gate:
    mscript = missing_gate[0]["run"]
    for report in [
            "build/artifacts/job-d-functional-parity/artifact-integrity.json",
            "build/artifacts/job-d-functional-parity/parity-report.json",
            "build/artifacts/job-e-browser/build/browser-check.json",
            "build/artifacts/omnidecide-wasm-checksums/wasm-checksums.txt",
            "build/artifacts/job-h-gamelab-node-tests/gamelab-node-report.json",
            "build/artifacts/job-h-gamelab-node-tests/adapter-lifecycle-report.json",
            "build/artifacts/job-h-gamelab-node-tests/gamelab-state-report.json",
            "build/artifacts/job-h-gamelab-node-tests/gamelab-node-summary.json",
            "build/artifacts/job-i-gamelab-browser-tests/build/gamelab-browser-check.json"]:
        check(report in mscript, f"missing-report gate checks {report.split('artifacts/')[1]}")

print()
print(f"{len(checks) - len(failures)}/{len(checks)} structural checks passed")
sys.exit(1 if failures else 0)
