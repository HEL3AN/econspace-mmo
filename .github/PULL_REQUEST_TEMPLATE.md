## What & why

<!-- What does this change do, and why? Link any related issue (e.g. "Closes #12"). -->

## Type

- [ ] Bug fix
- [ ] Feature
- [ ] World content / data
- [ ] Art / assets
- [ ] Docs
- [ ] Refactor / tooling
- [ ] Agent / MCP server

## Checklist

- [ ] Builds cleanly (`cmake --build build`) with no new `-Wall -Wextra` warnings. CI builds with `-Werror`.
- [ ] `ctest --test-dir build --output-on-failure` passes.
- [ ] The server smoke tests pass: `econserver hosttest`, `accttest`, `worldtest`, `ordertest`.
- [ ] If the wire changed: `PROTO_VERSION` is bumped and `econagent selftest` passes against a live server.
- [ ] If a save format changed meaning: `Save::WORLD_VERSION` / `Save::ACCOUNT_VERSION` is bumped.
- [ ] If anything visual changed: it was **looked at** (`worldeditor gallery shapes` or the game), and a screenshot is below. No test can see a picture.
- [ ] If `data/` changed: `documents/world_format.md` describes it.
- [ ] Follows [CONVENTIONS.md](../CONVENTIONS.md); comments and docs are in English.
- [ ] `engine` still doesn't depend on any other target.

## Notes

<!-- Anything reviewers should know: trade-offs, follow-ups, screenshots. -->
