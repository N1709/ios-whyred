# Working on ios-whyred

- Read `NOTES.md` before starting; it contains the project goal, hardware facts,
  current blocker, local artifact locations and the next concrete action.
- The user switches agents frequently. Update the active handoff in `NOTES.md`
  after each milestone and before stopping. Record commits, CI run IDs, exact
  build/boot results and unfinished work. Distinguish observations from guesses.
- Commit completed changes. Use GitHub Actions (`.github/workflows/xnu.yml`)
  for kernel builds. Local single-file cross-compiles with `aarch64-linux-gnu-gcc`
  are available.
- Device bring-up uses temporary `fastboot boot` images on Xiaomi Redmi Note 5 / Note 5 Pro (`whyred`).
  Preserve the Android installation and partition backups. The iOS root volume
  goes into the existing `userdata` partition on eMMC 5.1 (no GPT changes). Keep bootloaders,
  recovery, modem, persist and backups intact, and confirm each destructive step
  with the user before running it.
- Keep device dumps, serial numbers and downloaded Apple artifacts out of git.
