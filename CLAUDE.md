# fc2ds

Reverse-engineer NES games and reimplement them natively on NDS.

- Talk to the user in Chinese. Code, comments and commit messages in English.
- Commits: follow existing history style; no description body, no co-author lines. Only commit when asked. Use gitmoji in commit subjects.
- Never commit ROMs (`roms/`, `*.nes`).
- Build in a login shell (`bash -l`) so BlocksDS env vars from `/etc/profile.d/blocksds.sh` are set.
- Start by reading `docs/design.md`, `docs/setup.md` and the README of the game being worked on (`games/<name>/README.md`, which tracks status and next steps). The porting process is `docs/porting-guide.md`; the `nes-port` and `nds-check` skills in `.claude/skills/` drive it.
- The port is an enhanced version, not 1:1: NES hardware limits (slowdown, sprite flicker, raster tricks) are removed and colour may be enhanced; gameplay is kept. See `docs/design.md` section 3.
