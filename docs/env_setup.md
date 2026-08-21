# AHLS CLI: compile and run any source (for agents)

Use these commands when you have a `.cpp` file, **not** a sample CMake project.
`ahls` is the HLS IP Gen / SYCL FPGA compiler. It only exists **inside** the
AHLS Apptainer. Do not call `ahls` on the host.

## How to invoke `ahls`

Prefer `ahls-exec` for one-shot commands (agents, CI, scripts). Do not open
`ahls-sh` unless a human wants an interactive shell.

```bash
AHLS=<path-to-ahls-install>          # directory that contains bin/ahls-exec
SRC=path/to/kernel.cpp               # one or more .cpp files on the same ahls line
INC=path/to/include                  # extra -I dirs (optional; repeat -I)
DEVICE=Agilex7                       # or a part number, e.g. 10AS066N3F40E2SG
WORKDIR=/path/to/project             # folder to bind (must contain SRC and INC)

# Pattern:
$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- <command>
```

`--bind` makes the project visible inside the container. Bind the tree that
contains the source and any headers. `--pwd` is the working directory.

If the install has no `ahls-exec`, copy it from the drop into `$AHLS/bin/`
(see `VSCODE_DEBUG.md`). Fallback: enter `$AHLS/bin/ahls-sh` and run the
`ahls ...` lines below without the wrapper.

---

## Variables used below

| Name | Meaning |
|------|---------|
| `SRC` | Input `.cpp` file(s). Pass several on one `ahls` line when needed. |
| `INC` | Extra include dir, e.g. this repo’s `include/` for `exception_handler.hpp` |
| `DEVICE` | `-Xsdevice=` value. Default family: `Agilex7` |
| `OUT` | Output name (no extension in the recipes; each flow adds one) |

Add `-I$INC` for each include directory. Drop it if the source needs none.

Debug info (emulator only, for GDB): add `-g -O0`.

---

## 1. FPGA emulator (`fpga_emu`) — seconds

Kernel runs on the CPU. Use this for functional tests and GDB.

**One shot** (preferred; list every `.cpp` on this line):

```bash
ahls -Wall -DFPGA_EMULATOR -g -O0 -I"$INC" $SRC -o "$OUT.fpga_emu"
```

**Run:**

```bash
./"$OUT.fpga_emu"
```

Expect a host-printed result (these samples typically print `PASSED` / `FAILED`
and exit 0 / non-zero). Kernel breakpoints work in GDB here.

Via `ahls-exec`:

```bash
$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ahls -Wall -DFPGA_EMULATOR -g -O0 -I"$INC" $SRC -o "$OUT.fpga_emu"

$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ./"$OUT.fpga_emu"
```

---

## 2. Report (`report`) — minutes

Produces RTL + optimization reports. No runnable program.

**One shot** (preferred; list every `.cpp` on this line):

```bash
ahls -Wall -DFPGA_HARDWARE -I"$INC" \
  -Xshardware -Xsdevice="$DEVICE" -fsycl-link=early \
  $SRC -o "$OUT.report"
```

Do not execute `$OUT.report`. Look for generated reports / RTL next to the
output (exact layout is compiler-version specific). Extra FPGA flags go on
the same line as `-Xs...` (e.g. `-Xsclock=...`).

---

## 3. FPGA simulator (`fpga_sim`) — minutes

Kernel is compiled to RTL and co-simulated on the SYCL **FPGA simulator**
device (MPSIM / `aclmsim0`). Needs Quartus tooling **plus** a supported HDL
simulator on PATH **inside** the container (**Questa** / `questa_fse` here).

This is **RTL co-sim of the HLS-generated kernel**, not Quartus post–place-
and-route timing simulation, and not the early `report` flow alone.

**One shot** (preferred; list every `.cpp` on this line):

```bash
ahls -Wall -Xssimulation -DFPGA_SIMULATOR -I"$INC" \
  -Xssimulation -Xsghdl -Xsdevice="$DEVICE" \
  -Xsoptimize=latency \
  -reuse-exe="$OUT.fpga_sim" \
  $SRC -o "$OUT.fpga_sim"
```

Optional clock pin (also temporarily set
`[[intel::scheduler_target_fmax_mhz(N)]]` on the kernel lambda if you want the
scheduler to match): `-Xsclock=480MHz`.

**Run:**

```bash
CL_CONTEXT_MPSIM_DEVICE_INTELFPGA=1 ./"$OUT.fpga_sim"
```

Latency cycles: parse `*.prj/reports/resources/json/sim_stats.ndjson` and
`simulation_raw.ndjson` (see `docs/latency_optimization.md`). MPSIM often
keeps `period=1000` ps as the tracker tick even when `-Xsclock` is set —
convert ns with the pinned / reported fMAX.

GDB can debug **host** code. The kernel is RTL — no in-kernel GDB.

`-reuse-exe=` points at the previous sim binary to skip host relink when only
the device image changed. First build: the file may not exist yet; the compiler
still accepts the flag.

---

## 4. FPGA hardware (`fpga`) — hours

Place-and-route via Quartus. Needs Quartus in the container and a real device
for a board run. Targeting a BSP is **not** supported by these samples; use a
device family or part number.

**One shot** (preferred; list every `.cpp` on this line):

```bash
ahls -Wall -DFPGA_HARDWARE -I"$INC" \
  -Xshardware -Xsdevice="$DEVICE" \
  -reuse-exe="$OUT.fpga" \
  $SRC -o "$OUT.fpga"
```

**Run** (on a system with the FPGA; not typical for headless agents):

```bash
./"$OUT.fpga"
```

Not GDB-debuggable as a kernel.

---

## Flags cheat sheet

| Flag | Where | Meaning |
|------|--------|---------|
| `-DFPGA_EMULATOR` | compile (emu) | Select emulator device in source `#if` |
| `-DFPGA_SIMULATOR` | compile (sim) | Select simulator device |
| `-DFPGA_HARDWARE` | compile (report/fpga) | Select hardware device |
| `-g -O0` | compile (emu debug) | Debug info; skip for report/sim/fpga |
| `-Wall` | compile | Warnings |
| `-I<path>` | compile | Include path |
| `-c` / `-o` | compile | Object (`-c`, optional) / output |
| `-Xssimulation` | sim one-shot | Simulation flow (MPSIM + Questa RTL) |
| `-Xsghdl` | sim one-shot | GHDL/sim testbench |
| `-Xsoptimize=latency` | report/sim | Minimum-latency scheduler flow |
| `-Xsclock=<N>MHz` | report/sim/fpga | Pin kernel clock / schedule target |
| `-Xshardware` | report/fpga one-shot | Hardware / RTL backend |
| `-Xsdevice=<device>` | report/sim/fpga one-shot | Family or part |
| `-fsycl-link=early` | report one-shot | Stop after early link / report |
| `-reuse-exe=<bin>` | sim/fpga one-shot | Reuse previous host exe |

FPGA backend options (`-Xsclock`, `-Xsoptimize=latency`,
`-Xshyper-optimized-handshaking=off`, …) go on the same one-shot line for
report / sim / fpga, not on the emulator.

Macros in the source typically look like:

```cpp
#if FPGA_SIMULATOR
  auto selector = sycl::ext::altera::fpga_simulator_selector_v;
#elif FPGA_HARDWARE
  auto selector = sycl::ext::altera::fpga_selector_v;
#else
  auto selector = sycl::ext::altera::fpga_emulator_selector_v;
#endif
```

If the file has no selectors, you still pass the matching `-D` so any
`#if FPGA_*` code is consistent.

---

## Agent recipe (emulator first)

Default for automated tests: **emulator only**. Fast, no Quartus, exit code
from the program.

```bash
set -euo pipefail
AHLS=${AHLS:?set AHLS to the Apptainer install root}
WORKDIR=${WORKDIR:?}
SRC=${SRC:?}
OUT=${OUT:-test}
INC_FLAGS=()
[ -n "${INC:-}" ] && INC_FLAGS+=(-I"$INC")

# SRC may be one file or several: host.cpp test.cpp ...
$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ahls -Wall -DFPGA_EMULATOR -g -O0 "${INC_FLAGS[@]}" $SRC -o "$OUT.fpga_emu"

$AHLS/bin/ahls-exec --bind "$WORKDIR" --pwd "$WORKDIR" -- \
  ./"$OUT.fpga_emu"
```

Then optionally report / sim / fpga if Quartus is installed and the job allows
minutes-to-hours.

**Do not** use the VS Code F5 path for agents. That is IDE-only (`cppdbg` +
`ahls-exec` pipeTransport). Agents should call `ahls-exec` as above.

---

## Multiple source files

Use one `ahls` invocation with every `.cpp` listed. This is the default for
**all** flows (emu, report, sim, fpga) — put that flow’s flags on the same
line, then the sources, then `-o`:

```bash
# emu
ahls -Wall -DFPGA_EMULATOR -g -O0 -I"$INC" a.cpp b.cpp -o "$OUT.fpga_emu"

# report
ahls -Wall -DFPGA_HARDWARE -I"$INC" \
  -Xshardware -Xsdevice="$DEVICE" -fsycl-link=early \
  a.cpp b.cpp -o "$OUT.report"

# sim
ahls -Wall -Xssimulation -DFPGA_SIMULATOR -I"$INC" \
  -Xssimulation -Xsghdl -Xsdevice="$DEVICE" \
  -reuse-exe="$OUT.fpga_sim" \
  a.cpp b.cpp -o "$OUT.fpga_sim"

# fpga
ahls -Wall -DFPGA_HARDWARE -I"$INC" \
  -Xshardware -Xsdevice="$DEVICE" \
  -reuse-exe="$OUT.fpga" \
  a.cpp b.cpp -o "$OUT.fpga"
```

Split compile/link (`-c` per file, then a link) still works if you need
separate objects; it is not required.

---

## Common failures

| Symptom | Cause / fix |
|---------|-------------|
| `ahls: command not found` | Ran on the host. Wrap with `ahls-exec` or use `ahls-sh`. |
| `exception_handler.hpp` not found | Bind the tree that contains `include/` and pass `-I`. |
| container mount / bind error | Bind a path that exists; bind the project root, not a missing sibling. |
| sim/fpga fails looking for Quartus | Those flows need Quartus (and a simulator for sim) inside the container. |
| breakpoints don’t hit in sim/fpga | Expected. Kernel debug is emulator-only. |
| Release-quality emu, no locals | Add `-g -O0` (and `-DFPGA_EMULATOR`). |

## Example SYCL Code
Can be found under /p/psg/swip/w/renlizha/code-samples/hls-samples