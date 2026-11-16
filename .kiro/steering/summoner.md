---
inclusion: always
---

# Summoner and process access

Follow #[[file:AGENTS.md]]: psQuake never lists, renices or signals a process itself; the
bundled Summoner is the only path to real processes. The specs are psDoom's `docs/spec/`
at the commit in `psproc/UPSTREAM`; they decide where psQuake differs. Tests never act on a
real process, and every Summoner run uses a scratch `--root` and `HOME`.
