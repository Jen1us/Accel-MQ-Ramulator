# Publish GitHub Snapshot Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Publish the current Accel-Sim + MQSim + Ramulator2 co-simulation workspace to the private GitHub repository `Jen1us/Accel-MQ-Ramulator` as a self-contained source snapshot.

**Architecture:** Keep the current development tree untouched as the source workspace, clone the destination GitHub repository into a separate publish directory, then copy the workspace contents into that clone while flattening nested repositories and excluding disposable runtime artifacts. Commit and push from the isolated publish clone so the GitHub repository contains real source files instead of gitlinks/submodules.

**Tech Stack:** Git, Bash, rsync, Markdown, Accel-Sim / GPGPU-Sim C++, MQSim C++, Ramulator2 C++

---

### Task 1: Define snapshot inputs and exclusions

**Files:**
- Create: `docs/plans/2026-03-21-publish-github-snapshot.md`
- Reference: `.gitignore`
- Reference: `docs/CO_SIM_MQSIM_RAMULATOR2.md`
- Reference: `docs/CO_SIM_MQSIM_RAMULATOR2_DEV.md`

**Step 1: Inspect current repository state**

Run: `git status --short --branch`
Expected: current `dev` workspace contains tracked simulator edits plus untracked source directories such as `MQSim/`, `ramulator2/`, `external_wrappers/`, `docs/`, and `workloads/`.

**Step 2: Identify nested repositories that must be flattened**

Run: `find . -maxdepth 3 \( -path './.git' -o -path './MQSim/.git' -o -path './ramulator2/.git' -o -path './externals/MQSim/.git' -o -path './externals/ramulator/.git' \) -print`
Expected: output lists the top-level `.git` plus nested `.git` directories under `MQSim/`, `ramulator2/`, and `externals/`.

**Step 3: Define disposable paths to exclude from the publish snapshot**

Exclude:
- `.git/`
- any nested `.git` directory or git metadata file
- `hw_run/`
- `sim_run_12.8/`
- `sim_run_mlperf_hbf/`
- `sim_run_mlperf_hbm/`
- `sim_run_mlperf_cosim/`
- `traces/`
- `.tmp_sm89_trace/`
- `.local-cuda/`
- `.venv/`
- `__pycache__/`
- `gpu-simulator/build/`
- `ramulator2/build/`
- `external_wrappers/*/build/`
- `*.log`
- `*.pid`
- `*.cubin`
- `.mlc-log.txt`
- `accelwattch_power_report.log`

**Step 4: Define source paths that must remain in the snapshot**

Keep:
- `gpu-simulator/`
- `MQSim/`
- `ramulator2/`
- `external_wrappers/`
- `externals/`
- `docs/`
- `workloads/`
- `tools/`
- `util/`
- top-level build/config scripts that belong to source control

### Task 2: Prepare isolated publish clone

**Files:**
- Clone target: `/home/jenius/Accel-MQ-Ramulator-publish`

**Step 1: Remove any stale publish directory**

Run: `rm -rf /home/jenius/Accel-MQ-Ramulator-publish`
Expected: no previous publish clone remains.

**Step 2: Clone the destination repository over HTTPS**

Run: `git clone https://github.com/Jen1us/Accel-MQ-Ramulator.git /home/jenius/Accel-MQ-Ramulator-publish`
Expected: clone succeeds and checks out `main`.

**Step 3: Inspect the clean destination clone**

Run: `git -C /home/jenius/Accel-MQ-Ramulator-publish status --short --branch`
Expected: branch is `main` and worktree is clean before sync.

### Task 3: Sync workspace into the publish clone

**Files:**
- Modify in isolated clone: `/home/jenius/Accel-MQ-Ramulator-publish/*`

**Step 1: Copy workspace contents with exclusions**

Run: `rsync -a --delete --exclude='.git/' --exclude='**/.git/' --exclude='hw_run/' --exclude='sim_run_12.8/' --exclude='sim_run_mlperf_hbf/' --exclude='sim_run_mlperf_hbm/' --exclude='sim_run_mlperf_cosim/' --exclude='traces/' --exclude='.tmp_sm89_trace/' --exclude='.local-cuda/' --exclude='.venv/' --exclude='__pycache__/' --exclude='gpu-simulator/build/' --exclude='ramulator2/build/' --exclude='external_wrappers/*/build/' --exclude='*.log' --exclude='*.pid' --exclude='*.cubin' --exclude='.mlc-log.txt' --exclude='accelwattch_power_report.log' /home/jenius/accel-sim-framework/ /home/jenius/Accel-MQ-Ramulator-publish/`
Expected: publish clone now contains the current source tree contents, including nested repo working tree files but not nested git metadata.

**Step 2: Verify nested repositories are flattened**

Run: `find /home/jenius/Accel-MQ-Ramulator-publish -path '*/.git' -print`
Expected: only `/home/jenius/Accel-MQ-Ramulator-publish/.git` exists.

**Step 3: Inspect publish clone status**

Run: `git -C /home/jenius/Accel-MQ-Ramulator-publish status --short`
Expected: Git shows normal file additions and modifications rather than gitlinks/submodules.

### Task 4: Commit and push snapshot

**Files:**
- Commit from isolated clone: `/home/jenius/Accel-MQ-Ramulator-publish`

**Step 1: Stage all snapshot content**

Run: `git -C /home/jenius/Accel-MQ-Ramulator-publish add -A`
Expected: all source files and deletions are staged.

**Step 2: Create a snapshot commit**

Run: `git -C /home/jenius/Accel-MQ-Ramulator-publish commit -m "Publish co-simulation snapshot"`
Expected: a new commit records the flattened source snapshot.

**Step 3: Push to GitHub**

Run: `git -C /home/jenius/Accel-MQ-Ramulator-publish push origin main`
Expected: remote `main` is updated successfully over HTTPS.

### Task 5: Verify published result

**Files:**
- Verify: `/home/jenius/Accel-MQ-Ramulator-publish`

**Step 1: Verify publish clone is clean after push**

Run: `git -C /home/jenius/Accel-MQ-Ramulator-publish status --short --branch`
Expected: branch `main` is clean and tracks `origin/main`.

**Step 2: Verify remote head**

Run: `git ls-remote https://github.com/Jen1us/Accel-MQ-Ramulator.git HEAD refs/heads/main`
Expected: both refs point to the new snapshot commit.

**Step 3: Summarize outcome**

Report:
- publish repository URL
- whether nested repos were flattened successfully
- major excluded artifact categories
- any caveats that still require manual follow-up
