# rules.md — rules for AI agents working on this repo

These are hard rules. If a user request conflicts with them, stop and ask.

## 1. Read before touching anything

1. `progres.md` — the live board: what works, latest boot, next actions.
2. `docs/STATUS.md` — longer status and image hashes.
3. `docs/touch.md` — touch hardware, boot args, panel band map.
4. `docs/history/<newest month>.md` — why the code looks like it does.
5. `AGENTS.md` — environment and reference sources.

Do not re-derive facts that are already recorded. Do not trust a summary over
a ring dump.

## 2. Device safety (non-negotiable)

* Boot **only** with `fastboot boot <image>`. Never `fastboot flash`.
* Never write boot, modem, persist, recovery or any bootloader partition.
* Never format `userdata` or change the GPT. The iOS root lives inside the
  existing `userdata` partition.
* Keep the Android installation and the partition backups usable.
* Ask the user before any destructive step, even if it looks safe.

## 3. Language and interaction

* Reply in short Indonesian, minimal tokens. The user is often ill and away.
* Ask one concrete question at a time; prefer a numbered 3-step instruction.
* Never claim a hardware result you have not seen in a ring or a user report.
* Mark guesses as guesses.

## 4. Repository rules

* iOS 16 only; no iPadOS profile and no iPadOS option.
* Display and storage must use the kernel's built-in drivers.
* One folder per subsystem, one subfolder per driver, each with a README.
* No `Whyred` in driver names or driver file names; board facts go in
  `board.h` / the ADT.
* No HakiOS references, links or copied text.
* No device dumps, serial numbers or Apple artifacts in git.

## 5. Patch discipline (XNU series)

* One patch = one change. Next free number in `xnu/patches/`.
* Every patch must apply cleanly on the series tip.
* Every patch gets assertions in `tests/run_tests.sh`:
  * the added line or behaviour that matters,
  * the old broken form is gone,
  * the invariant that must not regress.
* Update the patch table in `README.md` (or `docs/patches.md`).
* Add an entry to `docs/history/<month>.md` with the observation, the guess,
  the change and the result.
* Run `bash tests/run_tests.sh` before committing. Green or do not commit.
* After pushing, run
  `python3 ../UniXNU/scripts/porting-record.py check --device ../UniXNU-whyred`
  and keep `whyred: 13 structured areas checked`.

## 6. Build and CI

* There is no local `unixnu build`; a kernel change costs one CI run
  (~10–15 min). Batch small changes into one patch.
* `gh` is not installed. Use the REST API:
  `curl -s "https://api.github.com/repos/N1709/ios-whyred/actions/runs?branch=whyred&per_page=1"`
  and match `head_sha`.
* Pack images with `tools/build-touch-diag.sh <run-id>`; pass `SWEETEARLY`,
  `SWEETI2C`, `SWEETRESET`, `SWEETREBOOT`, `OUT_IMAGE`.
* Always report the image path, sha256 and size to the user.

## 7. Git

* Push target: `ssh://git@ssh.github.com:443/N1709/ios-whyred.git`,
  branch `whyred`. Never push to `main`.
* Commit completed work, not half-states.
* Message = subject plus body. **No `Co-Authored-By:` trailer.**
* Do not rewrite pushed history.

## 8. Debugging discipline

* The SYSTEM ring (`tools/sweet-log-pull.sh`) is the primary evidence. Read
  `total`/`flush` first: an unchanged flush means no new log was written.
* Panel bands are the secondary evidence before the ring exists. The last
  visible colour names the step that did not finish.
* Symbolize with the repo-local Mach-O base:
  `llvm-symbolizer --obj=<kernel> 0xfffffe00...`, where `__TEXT` starts at
  `0xfffffe0007004000`.
* Read the whole register dump before naming a cause. `far`, `esr`, `pc` and
  the argument registers usually decide it.
* Change one variable per boot. Do not stack fixes.

## 9. Touch specifics

* The controller is the Synaptics DSX at I2C `0x20`. The `novatek@62` DT node
  is the other panel variant; do not pivot to it.
* The chip only answers in early boot, before eMMC and the QUP driver. Probe
  there (`sweetouchearly`).
* Do not pulse GPIO 66 except through the documented reset path.
* Do not trust a "F12 found" result alone: validate the F01 record
  (`00 06 0d 11`) first, and remember the PDT stream can be shifted by one
  byte.
* The final path is `SweetHIDTouch::touch(finger, down, x, y)`; do not invent
  a second HID stack.

## 10. Task delegation

* Use OpenCode for heavy reasoning, hardware register analysis and code
  authoring:
  `opencode run -m opencode/deepseek-v4.1-flash-free --auto "<prompt>"`.
* `AGENTS.md` carries the workflow; this file carries the hard rules. There
  is no separate Gemini file any more.

## 11. Handoff

Before stopping, update `progres.md`:

* the "What works" table,
* latest kernel commit, image name and sha256,
* the exact boot result,
* numbered next actions,
* blockers and unknowns.

Distinguish what was observed from what is believed. The next agent has no
memory of this session.
