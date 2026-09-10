# Contributing

Thanks for taking the time to help. This document is deliberately short; the
authoritative details live next to the code.

## Getting set up

```sh
git clone <your-fork-url>
cd ToxRelayer

make            # build ./toxrelayer
make test       # 10 suites, 135 812 assertions, no network, no sleeping
```

Dependencies are listed in the [README](README.md#requirements):
a C11 compiler, `pkg-config`, libtoxcore/libtoxav and sqlite3.

## Before you write code

Open an issue first for anything larger than a bug fix, so the design can be
agreed before the work is done. For a security problem, follow
[SECURITY.md](SECURITY.md) instead of opening a public issue.

## The four gates

Every change must pass all of these locally before it is proposed:

```sh
make                 # 0 warnings: -Wall -Wextra -Werror
make test            # every suite; fails on the first failure
make test-sanitize   # the same suites under AddressSanitizer + UBSan
make analyze-strict  # clang static analyzer + clang-tidy: 0 findings
```

## Coding standard

The standard is not aspirational — it is enforced by the gates above and
recorded in [`.clang-tidy`](.clang-tidy) and
[`docs/REMEDIATION_LOG.md`](docs/REMEDIATION_LOG.md).

* **C11**, no compiler extensions, no warnings.
* **Library code never exits, aborts or prints.** It returns status codes; the
  composition root (`src/toxrelayer.c`) is the only place allowed to log.
* **No hidden state in policy code.** `relay.c`, `telegram.c`, `msg_queue.c` and
  `mq_persist.c` take their environment and their clock as parameters, which is
  what makes them testable without a network or a sleep.
* **Bounds before use.** Validate an index before it indexes anything, a length
  before it is copied, and a parsed number before it is trusted. Existing
  helpers (`friend_number_valid`, `parse_int_range`, `copy_tox_str`,
  `unquote_str`) exist so that this is the path of least resistance.
* **Do not disable a check to make a warning go away.** If a deviation is
  genuinely right, write the justification in `.clang-tidy` next to the other
  documented deviations.
* **Keep the composition root thin.** Logic moves into a module where a test can
  reach it; see how the relay policy was extracted from `toxrelayer.c`.

## Tests

A change to behaviour needs a test, in `tests/test_<module>.c`.

* No network, no `sleep`. Inject time instead.
* Use `tox_new()` with UDP, IPv6 and LAN discovery disabled when a Tox handle is
  needed, or a test double when it is not.
* Write only inside a `mkdtemp()` directory; never touch operator data.

[`docs/TEST_COVERAGE.md`](docs/TEST_COVERAGE.md) explains how the suites are
organised and tells you which table to update.

## Commits and pull requests

* One logical change per commit; write the message in the imperative mood and
  explain *why*, not just what. `Fixed a bug` helps nobody.
* Update [`docs/TEST_COVERAGE.md`](docs/TEST_COVERAGE.md) when you add or move a
  function, and [`docs/REMEDIATION_LOG.md`](docs/REMEDIATION_LOG.md) when you fix
  a defect, so the traceability stays true.
* Fill in the pull request template. Say what you changed, why, and how you
  verified it.

## Licence

Contributions are accepted under the GNU GPL v3 or later, the licence this
project already uses (see [COPYING](COPYING)).
