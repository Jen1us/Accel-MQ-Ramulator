# GitHub Slimming And Co-Sim Docs Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove regenerated artifacts and experiment outputs that do not belong in a GitHub-ready tree, then produce a detailed developer-facing document explaining the Accel-Sim, MQSim, and Ramulator2 co-simulation interfaces and design.

**Architecture:** Keep source-of-truth code and third-party source trees in place, but delete run outputs, local caches, binaries, and wrapper build directories. Update ignore rules so those artifacts do not reappear in future commits. Refresh the co-simulation documentation to explain layering, interfaces, control flow, data flow, and code entry points for GitHub readers.

**Tech Stack:** Git, Bash, Markdown, Accel-Sim / GPGPU-Sim C++, MQSim C++, Ramulator2 C++, CMake

---

### Task 1: Classify GitHub-safe vs disposable paths

**Files:**
- Modify: `.gitignore`
- Reference: `docs/CO_SIM_MQSIM_RAMULATOR2_DEV.md`

**Step 1: Inspect current repository state**

Run: `git status --short`
Expected: tracked modifications in simulator files plus many untracked generated directories.

**Step 2: Inspect current ignore rules**

Run: `sed -n '1,240p' .gitignore`
Expected: existing ignore rules cover some run outputs but not all recent co-sim artifacts.

**Step 3: Inspect generated artifacts and wrapper build outputs**

Run: `find . -maxdepth 2 \( -name build -o -name '*.log' -o -name '*.pid' -o -name '*.cubin' -o -name '.tmp*' -o -name '.mlc-log.txt' \) | sort`
Expected: output includes wrapper build directories, logs, local temp files, and tracer cubins.

**Step 4: Define disposal set**

Dispose of:
- `hw_run/`
- `sim_run_12.8/`
- `sim_run_mlperf_hbf/`
- `sim_run_mlperf_hbm/`
- `sim_run_mlperf_cosim/`
- `traces/`
- `.local-cuda/`
- `.tmp_condarc`
- `.tmp_sm89_trace/`
- `.mlc-log.txt`
- `accelwattch_power_report.log`
- `tracer_tool.1.sm_89.cubin`
- `tracer_tool.2.sm_89.cubin`
- `external_wrappers/*/build/`
- `gpu-simulator/build/`
- `ramulator2/build/`
- `accel-sim-pull.log`
- `accel-sim-pull.pid`
- `accel-sim.tar`
- `accel-sim.tar.sha256`

**Step 5: Keep source paths**

Keep:
- `gpu-simulator/`
- `MQSim/`
- `ramulator2/`
- `external_wrappers/*/*.cpp`
- `external_wrappers/*/CMakeLists.txt`
- `workloads/`
- `docs/`
- `tools/`
- `util/docker/`
- `externals/` when it contains source dependencies rather than build outputs

### Task 2: Update ignore rules

**Files:**
- Modify: `.gitignore`

**Step 1: Add co-sim build and runtime artifact patterns**

Add ignore patterns for:
- wrapper build directories
- top-level run output directories
- local temp/cubin/log artifacts
- simulator build directories

**Step 2: Preserve source trees**

Do not ignore:
- `MQSim/`
- `ramulator2/`
- `external_wrappers/**/*.cpp`
- `docs/`
- `workloads/`

**Step 3: Verify ignore intent**

Run: `git status --short`
Expected: deleted disposable paths disappear after removal; retained source/doc paths remain visible if untracked.

### Task 3: Remove disposable files

**Files:**
- Delete runtime and build artifacts listed in Task 1

**Step 1: Delete wrapper build outputs**

Run: `rm -rf external_wrappers/mqsim_wrap/build external_wrappers/ramulator2_wrap/build`
Expected: only wrapper source files remain.

**Step 2: Delete simulator build outputs and local caches**

Run: `rm -rf gpu-simulator/build ramulator2/build .local-cuda .tmp_sm89_trace`
Expected: source trees remain, local build outputs are gone.

**Step 3: Delete logs, tarballs, traces, and run outputs**

Run: `rm -rf hw_run sim_run_12.8 sim_run_mlperf_hbf sim_run_mlperf_hbm sim_run_mlperf_cosim traces accel-sim.tar accel-sim.tar.sha256 accel-sim-pull.log accel-sim-pull.pid accelwattch_power_report.log tracer_tool.1.sm_89.cubin tracer_tool.2.sm_89.cubin .mlc-log.txt .tmp_condarc`
Expected: repository tree becomes GitHub-oriented instead of experiment-oriented.

**Step 4: Inspect top-level tree**

Run: `ls -1`
Expected: source, docs, wrappers, workloads, and third-party source trees remain.

### Task 4: Rewrite GitHub-facing co-simulation design document

**Files:**
- Modify: `docs/CO_SIM_MQSIM_RAMULATOR2_DEV.md`
- Reference: `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc`
- Reference: `gpu-simulator/gpgpu-sim/src/gpgpu-sim/ramulator2_backend.cc`
- Reference: `gpu-simulator/gpgpu-sim/src/gpgpu-sim/mqsim_backend.cc`
- Reference: `external_wrappers/ramulator2_wrap/ramulator2_wrap.cpp`
- Reference: `external_wrappers/mqsim_wrap/mqsim_wrap.cpp`

**Step 1: Re-read current document and actual integration code**

Run:
- `sed -n '1,260p' docs/CO_SIM_MQSIM_RAMULATOR2_DEV.md`
- `rg -n "ramulator2_backend|mqsim_backend|use_ramulator2_backend|use_mqsim_backend" gpu-simulator/gpgpu-sim/src/gpgpu-sim`
- `sed -n '1,260p' external_wrappers/ramulator2_wrap/ramulator2_wrap.cpp`
- `sed -n '1,320p' external_wrappers/mqsim_wrap/mqsim_wrap.cpp`

Expected: enough context to rewrite the document around code paths instead of high-level claims.

**Step 2: Document interface layering**

Describe:
- Accel-Sim memory partition side
- backend interface objects
- C-ABI wrapper boundary
- Ramulator2 backend flow
- MQSim backend flow

**Step 3: Document control flow and time synchronization**

Explain:
- who owns master time
- how `cycle()` advances the external simulators
- why partition 0 drives the shared backend instance
- how `poll()` drains completions back into `mem_fetch`

**Step 4: Document data flow and request lifecycle**

Explain:
- `mem_fetch` creation and routing
- request tagging and backend selection
- address translation/partition-local addressing
- read completion path
- posted-write completion path

**Step 5: Document design trade-offs and current limitations**

Include:
- shared backend instance behavior
- HOL interactions via return queues
- simplified MQSim HBF path versus SSD full stack
- what remains TODO for mixed HBM/HBF policy and richer flash semantics

### Task 5: Verify and summarize

**Files:**
- Verify: `.gitignore`
- Verify: `docs/CO_SIM_MQSIM_RAMULATOR2_DEV.md`

**Step 1: Verify tree cleanliness**

Run: `git status --short`
Expected: only meaningful source/doc changes remain; disposable artifacts are gone or ignored.

**Step 2: Verify document structure**

Run: `sed -n '1,260p' docs/CO_SIM_MQSIM_RAMULATOR2_DEV.md`
Expected: document reads as a GitHub-facing engineering explanation with concrete code references.

**Step 3: Summarize final repo state**

Report:
- what categories were deleted
- what was intentionally kept
- where the new/updated documentation lives
