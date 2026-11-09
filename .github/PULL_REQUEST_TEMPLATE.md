<!--
Title format: type(scope): summary — e.g. fix(search): keep find usable through catalog rebuilds
-->

## Summary

What changed and why. Link the issue it closes (`Fixes #NNN`) when there is one.

## Platform impact

The RVT1 contract (`app/backend.rkt` RPC/Event/State surface) is cross-platform:
changing any of it is a three-platform release, with the checked-in generated
clients regenerated in the same commit.

- [ ] No backend contract changes (RPC/Event/State untouched)
- [ ] `raco rivet generate` run; the three tracked clients diff clean
      (`windows/GeneratedBackend.hpp`, `linux/GeneratedBackend.hpp`,
      `macos-host/Sources/RivetHost/GeneratedBackend.swift`)
- [ ] `raco rivet i18n` run for changes under `shared/i18n/`

## Tests

How this is covered: `raco test tests/`, platform host builds, the acceptance
leg, or a new test added in the same commit.

## Checklist

- [ ] CHANGELOG.md updated for user-visible changes
- [ ] Docs updated in the same change (README / user-guide, both languages)
- [ ] Security-relevant behavior flagged per [SECURITY.md](SECURITY.md)
