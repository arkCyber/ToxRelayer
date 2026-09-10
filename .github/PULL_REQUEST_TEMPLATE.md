# Pull request

## What and why

<!-- What does this change, and what problem does it solve? Link the issue. -->

Fixes #

## How it was verified

<!-- Paste the results, or delete the lines that do not apply. -->

- [ ] `make` — 0 warnings, 0 errors
- [ ] `make test` — all suites pass
- [ ] `make test-sanitize` — clean
- [ ] `make analyze-strict` — 0 findings

## Checklist

- [ ] The change is one logical unit, and the commit message explains *why*.
- [ ] New or changed behaviour has a test in `tests/test_<module>.c`.
- [ ] `docs/TEST_COVERAGE.md` is updated if a function was added, moved or
      removed.
- [ ] `docs/REMEDIATION_LOG.md` is updated if this fixes a defect.
- [ ] No operator data, key material or build output is included.
- [ ] No check was disabled to silence a warning without a documented
      justification in `.clang-tidy`.
