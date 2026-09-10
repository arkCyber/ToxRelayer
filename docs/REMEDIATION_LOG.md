# Remediation Log

Change history for the metaRelayer store-and-forward bot, recorded so every
modification is traceable to a defect, a requirement and a verification result.

Baseline for increment 1: `5d670ef` (dirty work tree) — reproducible build with
78 warnings, 14 static-analyzer findings, 262 clang-tidy diagnostics, 0 tests.

---

## Increment 1 — Foundation: persistence works, P0 defects removed

### Defects fixed

| ID | Defect | Location | Evidence before | Status |
|----|--------|----------|-----------------|--------|
| D-01 | `MAX_Friend_NUM` defined twice (1024 vs 5000) so friend-indexed arrays had inconsistent sizes | `toxrelayer.h:52`, `toxrelayer.c:149` | `-Wmacro-redefined` | Fixed: single definition in `toxrelayer.h`, `friend_number_valid()` guard added |
| D-02 | Function pointer stored as a timestamp | `msg_queue.c:86` | `-Wint-conversion` | Fixed: `get_unix_time()` |
| D-03 | Function pointer passed as `long datetime` | `toxrelayer.c:2244` | `-Wint-conversion` | Fixed: `get_unix_time()` |
| D-04 | `time_t` passed where `const struct tm *` expected | `misc.c:183` | `-Wint-conversion` | Fixed: `localtime()` with NULL check |
| D-05 | Write into a `const` buffer (undefined behaviour) | `toxrelayer.c:667` | `-Wincompatible-pointer-types-discards-qualifiers` | Fixed: non-const buffer + `snprintf` |
| D-06 | NULL passed to `memset` on a reachable path | `groupchats.c:42` | `unix.cstring.NullArg` | Fixed: index and pointer guards in `group_add()` |
| D-07 | Zero-sized variable length array (undefined behaviour) | `toxrelayer.c:1771,2049`, `commands.c:332` | `core.VLASize` | Fixed: fixed `MAX_NUM_GROUPS` buffer with clamped count |
| D-08 | SQL built by `sprintf`; injection and buffer overflow possible | `msg_database.c` | audit finding | Fixed: prepared statement + bound parameters |
| D-09 | Message archive silently non-functional: schema was never created | `msg_database.c` | audit finding | Fixed: `CREATE TABLE IF NOT EXISTS` + WAL on open |
| D-10 | `sprintf` format/data argument mismatch | `toxrelayer.c:484` | `-Wformat-extra-args` | Fixed: `snprintf`, argument corrected |
| D-11 | `printf` with `%s` and no argument | `toxrelayer.c:937` | `-Wformat-insufficient-args` | Fixed: `fprintf(stderr, ..., path)` |
| D-12 | `char` used as array subscript (can be negative) | `toxrelayer.c:1851` | `-Wchar-subscripts` | Fixed: `int` index plus bounds clamp |
| D-13 | `char (*)[N]` passed as `char *` | `msg_queue.c:194` | `-Wincompatible-pointer-types` | Fixed: correct pointer passed |

### Interface changes

* New `src/msg_database.h` — single authoritative contract for persistence.
  Removed the duplicated `struct CHAT_MSG` and the ad-hoc forward declarations
  in `toxrelayer.c`, so the compiler now enforces one signature per entry point.
* `msg_database_open()` creates the schema and is idempotent; added
  `msg_database_close()`, `msg_database_is_ready()`, `msg_database_add()`,
  `msg_database_top()` and `msg_database_last_sn()`.
  The last one is the hook for serial-number recovery across restarts.
* The on-disk table name `CHAT_MESSEAGE` is intentionally preserved so
  databases written by earlier builds remain readable.

### Verification

| Check | Before | After |
|-------|--------|-------|
| `make` warnings | 78 | 38 (all unused-variable / unused-function / pointer-sign) |
| `make` errors | 0 | 0 |
| Defect-class warnings (`int-conversion`, `macro-redefined`, `format-*`, `char-subscripts`, `incompatible-pointer-types`) | 8 | **0** |
| clang static analyzer findings | 14 (incl. NULL `memset`, zero-size VLA) | 12 (`deadcode.DeadStores` only) |
| Unit tests | none | `make test` → 40 checks passed |
| Sanitizers | none | `make test-sanitize` → 40 checks passed under ASan+UBSan |

### How to reproduce

```sh
make                 # build the bot
make test            # run the persistence verification suite
make test-sanitize   # same suite under ASan + UBSan
```

---

## Increment 2 — Delivery queue: correctness, bounded retries, full test coverage

### Why

`msg_queue.c` is the heart of the store-and-forward design, and the audit found
it could not guarantee delivery:

| Defect | Detail |
|--------|--------|
| D-14 | `send_count` was initialised to 0 and **never incremented**, so a message that could not be delivered was retried forever. A poison message could occupy the queue indefinitely and the queue could fill with undeliverable traffic. |
| D-15 | `msg_queue_insert()` returned `MQ_ERR_FULL` without the caller noticing, after the message had already been removed by `msg_queue_delete()` — **silent data loss** on a full queue. |
| D-16 | Empty/full detection used a wasted-slot scheme (`head`/`tail` with a discarded slot) that made the usable capacity ambiguous and let `msg_queue_item_get()` return `4048` for an empty queue. |
| D-17 | `msg_queue_mark_read()` scanned all 4048 slots and kept only a single global `last_comfirm_msg_receipt`. |
| D-18 | Time was read through a global clock inside the module, and the module required a live `Tox *`, so no deterministic test was possible. |
| D-19 | `msg_queue_check_timeouts()` was declared but never defined or called. |
| D-20 | The module printed to stdout from its primitives, mixing diagnostics into the transport layer. |

### What changed

* New `src/msg_queue.h` — the module contract; `msg_queue.c` rewritten.
* **Transport decoupled**: delivery is a callback (`mq_transport_fn`). The module
  no longer references toxcore at all, so it is unit-testable without a network.
  The only toxcore contact point is `tox_transport()` in `toxrelayer.c`.
* **Ring buffer with an explicit element count**, so empty and full are exact and
  the whole capacity is usable.
* **Bounded retries**: every message carries an attempt counter; after
  `MQ_MAX_ATTEMPTS` it is retired and reported through a drop hook
  (`on_telegram_dropped()` logs the loss). No more livelock.
* **No silent loss**: `mq_enqueue()` reports `MQ_ERR_FULL` to the caller.
* **Injected time**: `mq_service(..., time_t now)` makes every timeout path
  deterministic and removes all sleeping from the tests.
* **Invariant assertions** (`mq_check_invariants()`): occupancy and in-flight
  counts are re-derived and asserted on every transition.
* **Struct padding removed** (field reordering), saving ~64 KB of BSS.
* `msg_queue_init()/insert()/try_send()/mark_read()` replaced by
  `mq_init()/mq_enqueue()/mq_service()/mq_confirm()`.

### Verification

| Check | Result |
|-------|--------|
| `make` warnings | 38 → **32** (`msg_queue.c` now contributes zero) |
| `make test` | `test_msg_database` 40 checks + `test_msg_queue` **20 364 checks**, all pass |
| `make test-sanitize` | both suites pass under **ASan + UBSan** |
| clang static analyzer | **0 findings** in `msg_queue.c` and `msg_database.c` |
| clang-tidy (project profile) | **0 findings** in `msg_queue.c` and `msg_database.c` |
| New static-analysis profile | `.clang-tidy` added; `make analyze-strict` is the gate |

Queue scenarios covered (tests/test_msg_queue.c):

1. initial state 2. argument validation 3. FIFO order 4. full-queue rejection
5. acknowledgement removes the message 6. acknowledgement timeout retransmits
7. **bounded retries then drop** (regression test for D-14) 8. fatal error drops
immediately 9. several messages in flight at once 10. payload integrity across
retransmission 11. backoff window and confirm validation 12. **ring wrap-around**
over two full fill/drain laps 13. counter accounting across mixed outcomes.

### Reproduce

```sh
make test            # builds and runs every suite
make test-sanitize   # same suites under ASan + UBSan
make analyze-strict  # static-analysis gate for the verified modules
make analyze         # full report, legacy modules included
```

---

## Increment 3 — Reliable store-and-forward: gap detection, replay, recovery

### Why

The relay could *lose* telegrams silently. The audit found:

| Defect | Detail |
|--------|--------|
| D-21 | The missed-telegram branch was an empty placeholder: `if ((receive_sn + 1) != msg_sn) { /* ???? */ ; }`. A gap was detected and then ignored, so the lost telegrams were never recoverable. |
| D-22 | The `ZCZC CMD` control telegram — the only mechanism a peer has to request a replay — was an empty statement. |
| D-23 | Serial numbers were compared with `+ 1` arithmetic that ignores the 1..999 wrap, so the comparison was wrong around the wrap point. |
| D-24 | Nothing recovered the receive serial numbers after a restart: the first telegram of every session looked like a gap. |
| D-25 | Sequence numbers were never validated; a peer could send serial `0`, `9999` or a negative value and corrupt the bookkeeping. |
| D-26 | The protocol had no parser that could be tested: it depended on the global cursors `aline_index`, `line_token` and `token`. |

### What changed

* **New `src/telegram.h` / `src/telegram.c`** — pure, bounds-checked codec and
  sequence tracker: no globals, no toxcore, no I/O, therefore exhaustively
  testable.
  * `tg_encode_text()` / `tg_encode_cmd()` / `tg_decode()`; `decode(encode(x)) == x`.
  * Key fields containing a line separator are rejected, so one field cannot
    forge the framing of the next.
  * A body claiming the terminator is rejected, so a payload cannot truncate
    itself.
  * `tg_sn_valid/next/prev/distance` are wrap aware.
* **New `sn_tracker` with an outstanding-missing set.** This is the subtle part:
  once a gap is detected `expected` has already jumped past it, so the replayed
  telegram arrives "behind" the expectation. Without the pending set the replay
  would be classified as a duplicate and *discarded*, and the gap could never
  close. `SN_RECOVERED` reports exactly that case.
* **Bounded recovery**: the outstanding set is capped at `SN_MAX_PENDING`; when
  the gap cannot be tracked the tracker resynchronises instead of overflowing.
* **`msg_database_range()`** — wrap-aware replay query (`from > to` means the
  range crosses the wrap point), covered by tests.
* **`toxrelayer.c` wiring**:
  * `request_missed_telegrams()` — emits the control request through the same
    delivery queue as ordinary traffic, so it inherits the retry policy.
  * `handle_cmd_telegram()` — validates that the request is addressed to us and
    replays the stored `outgoing` telegrams.
  * `recover_receive_serial_numbers()` — rebuilds the receive serials from the
    database at startup, so the first telegram after a restart is not mistaken
    for a gap.
  * Duplicate telegrams are now detected and dropped instead of being stored
    twice.

### Verification

| Check | Result |
|-------|--------|
| `make` | 0 errors, 32 warnings (`telegram.c` contributes zero) |
| `make test` | 69 + 20 364 + **110 404** + 73 = **130 910 checks**, all pass |
| `make test-sanitize` | same four suites pass under **ASan + UBSan** |
| clang static analyzer | **0 findings** in `telegram.c` |
| clang-tidy (project profile) | **0 findings** in `telegram.c` |
| `make analyze-strict` | **PASS** (gate now covers three modules) |

Test suites

| Suite | Checks | Scope |
|-------|--------|-------|
| `test_msg_database.c` | 69 | schema, binding, injection, clamping, replay range |
| `test_msg_queue.c` | 20 364 | ring buffer, retries, drops, wrap-around |
| `test_telegram.c` | 110 404 | codec round trips, malformed input, tracker, fuzzing |
| `test_reliability.c` | 73 | **end-to-end loss → detect → request → replay → recover** |

Two real defects were found *by the tests themselves* while writing this
increment, which is exactly the point of the exercise:

1. An off-by-one in the gap budget (`dist - 1 <= max_gap` instead of
   `dist <= max_gap`) that made `max_gap = 0` still recover a gap.
2. The duplicate-vs-recovered confusion described above, which would have made
   gap recovery silently ineffective in production.

### Reproduce

```sh
make test            # all four suites
make test-sanitize   # same suites under ASan + UBSan
make analyze-strict  # static-analysis gate
```

---

## Increment 4 — Zero-warning build, verified hot paths, zero static-analysis findings

### Why

With the three new modules clean, the remaining risk sat in the legacy code that
the relay actually executes. This increment closes that gap.

### Defects fixed

| ID | Defect | Location | Impact |
|----|--------|----------|--------|
| D-27 | The receive path passed the *receiver key* into the `send_receive` argument, so stored records carried a public key instead of `"incoming"` | `toxrelayer.c` `cb_friend_message` | The startup serial recovery added in increment 3 could never match, so every session re-detected a gap |
| D-28 | `purge_empty_groups()` passed a **slot index** to `group_leave()`, which expects a **group number** | `toxrelayer.c` | Cleared the wrong entry (or none), leaving stale group state |
| D-29 | The conference member listing passed the **peer count** as the conference number to three toxcore calls | `commands.c` | Group member listing was non-functional |
| D-30 | The room message was truncated to the **title length** because `length` was reused for two different quantities | `toxrelayer.c` `cb_conference_message` | Messages displayed with the wrong length |
| D-31 | `hex_string_to_bin()` looped `strlen()` times while advancing two characters per step, reading past the string and returning uninitialised bytes | `misc.c` | Out-of-bounds read; undefined values |
| D-32 | `file_contains_key()` never stripped the line terminator and shadowed its `FILE *` | `misc.c` | Keys were parsed with a trailing newline; the shadowed handle was easy to misuse |
| D-33 | `save_data()` jumped to its error label when `malloc` failed, **leaking the open stream** | `toxrelayer.c` | File descriptor leak on every out-of-memory save |
| D-34 | `load_tox()` returned without closing the file when `tox_new` failed | `toxrelayer.c` | File descriptor leak |
| D-35 | `load_tox()` used a **variable length array** sized by the on-disk file | `toxrelayer.c` | Unbounded stack allocation driven by a file |
| D-36 | `timed_out()` added a signed `time_t` to an unsigned timeout | `misc.c` | Signed/unsigned mix; overflow for large timeouts |
| D-37 | `_kbhit` is a reserved identifier at file scope | `toxrelayer.c` | Reserved-namespace violation |
| D-38 | `++optind` inside a compound condition | `toxrelayer.c` | Order-of-evaluation ambiguity |

### Hardening

* **Verified codec wired into the hot path.** `cb_friend_message()` now dispatches
  through `tg_decode()` instead of the global cursors `aline_index`, `line_token`
  and `token`. `read_buffer_line()` and its two globals were deleted: the parser
  that the relay trusts is the one with 110 000 assertions behind it.
* **Outbound telegrams** are produced by `tg_encode_text()` rather than
  `sprintf`, so the framing limits are the tested ones.
* **Storage format unified**: the database stores message *bodies*; a replay is
  regenerated by the encoder, so a stored blob can never be replayed malformed.
* **Console output centralised** in `console_out()` / `console_err()`, which
  observe the underlying write result once instead of ignoring it at ~130 call
  sites (CERT ERR33-C).
* **`hex_string_to_bin()` decodes by hand**: provably in-bounds, no run-time
  format parsing, locale independent.
* **Explicit `(void)`** on every deliberate discard, so no return value is
  ignored implicitly.
* **Callback parameters** that the toxcore ABI requires but the handler does not
  use are annotated individually rather than by disabling `-Wunused-parameter`.
* **Build standard tightened**: `-Wall -Wextra -Werror` is now the default
  (`WERROR=` relaxes it for a new toolchain).

### Verification

| Check | Before | After |
|-------|--------|-------|
| Compiler warnings | 78 (increment 0) / 6 (increment 3) | **0** under `-Wall -Wextra -Werror` |
| clang static analyzer + clang-tidy, all modules | 66 findings | **0 findings** |
| Unit tests | 130 910 checks | **130 932 checks**, all pass, also under ASan + UBSan |
| Analysis gate coverage | 3 modules | **all 7 modules** |

```
make                 # 0 errors, 0 warnings
make test            # 69 + 20364 + 110404 + 95 checks
make test-sanitize   # the same, under ASan + UBSan
make analyze         # 0 findings
make analyze-strict  # gate: fails on any new finding
```

---

## Increment 5 — Function-level test coverage and traceability

### Goal

Every function in the built sources must have a stated verification basis. The
project now carries a full traceability matrix in `docs/TEST_COVERAGE.md`, and
four new suites close the gaps that the matrix exposed.

### New suites

| Suite | Checks | Covers |
|-------|--------|--------|
| `tests/test_misc.c` | 100 | every utility function in `misc.c` except the one that needs a live conference |
| `tests/test_log.c` | 67 | logging and console output, asserted on captured stdout/stderr |
| `tests/test_groupchats.c` | 331 | the group table: add, leave, index, grow, capacity |
| `tests/test_commands.c` | 53 | the command parser and the whole dispatch table, driven by an offline Tox instance |

Total: **8 suites, 131 483 assertions**, all passing, also under ASan + UBSan.

### Defects found by writing these tests

| ID | Defect | Location | How it was found |
|----|--------|----------|------------------|
| D-39 | `char_find()` evaluated `s[idx]` unconditionally, so an index beyond the string **read past the buffer** | `misc.c` | AddressSanitizer reported a global-buffer-overflow |
| D-40 | `commands_parse()` copied an over-long argument into a fixed-size destination, **overflowing the argument buffer** | `commands.c` | the test asserted a terminator inside the declared bound |
| D-41 | `realloc_groupchats()` grew the table without initialising the new slots, so `group_add()` read an **indeterminate value as a `bool`** | `groupchats.c` | UndefinedBehaviorSanitizer reported "load of value 190, which is not a valid value for type 'bool'" |
| D-42 | `commands_parse()` produced a **spurious empty argument** whenever the input ended with a quoted argument, and an empty argument between a quoted argument and the next one, making `argc` one larger than the caller expected | `commands.c` | the parser tests asserted the exact argument count |
| D-43 | `get_time_str()` left an **unspecified partial value** in the buffer when `strftime()` failed | `misc.c` | the small-buffer test |
| D-44 | `sprintf()` used for address formatting (deprecated, unbounded) | `commands.c`, `toxrelayer.c` | the sanitizer build turned the deprecation into an error |

### Interface changes made for testability

* `parse_command()` became **`commands_parse()`**, declared in `commands.h` with a
  documented contract. Exposing a pure function so it can be tested is the point:
  the parsing rules are now pinned by tests instead of by inspection.
* `get_unix_time()` and `valid_nick()` were promoted from definitions in `misc.c`
  to declarations in `misc.h`, so callers and tests share one contract.
* `execute()`'s declaration was corrected to take `uint32_t`, matching the
  definition; the mismatch was hidden because the two were never seen together.

### Verification

| Check | Before | After |
|-------|--------|-------|
| Test suites | 4 | **8** |
| Assertions | 130 932 | **131 483** |
| Functions with a stated verification basis | partial | **134 of 134** (`docs/TEST_COVERAGE.md`) |
| Compiler warnings | 0 | 0 |
| Static-analysis findings | 0 | 0 |
| Sanitizer clean | yes | yes |

### Traceability

`docs/TEST_COVERAGE.md` lists every function with its tier:

* **U** — unit tested in isolation,
* **I** — integration tested with an offline toxcore instance or through the real
  path beneath it,
* **J** — justified as not independently testable, with the reason per function.

31 of the 45 functions in `toxrelayer.c` are tier J. That is not an accident: the
module is the composition root (process lifetime, DHT start-up, toxcore callback
entry points). The remediation deliberately moved the testable logic *out* of it
into `telegram.c`, `msg_queue.c`, `msg_database.c` and `commands.c`, which is why
the untestable remainder is now small and mechanical.

---

## Increment 6 — Relay policy extracted and unit tested

### Goal

Increment 5 left 31 of the 45 functions in `toxrelayer.c` at tier J ("not
independently testable"). Most of those were not really untestable — they were
*entangled* with toxcore. The relay's decision logic was the important part:

    is this record ours, or must it be forwarded?
    have we already seen it?
    is there a gap, and what exactly must we ask for?
    how is a replay request answered?

None of that needs the network. It only needed the globals and the `Tox *` to be
cut away.

### What changed

* **New `src/relay.h` / `src/relay.c`** — the routing and store-and-forward
  policy, with **no toxcore dependency and no global state**. Everything it needs
  from its surroundings is injected through `relay_env`: the peer directory, the
  message database, the delivery queue and the INI mirror.
* `toxrelayer.c` now supplies those callbacks (`relay_*` adapters) and calls
  `relay_receive()` once per record, logging the returned outcome. Five functions
  left the file: `handle_local_telegram`, `handle_text_telegram`,
  `handle_cmd_telegram`, `request_missed_telegrams` and
  `recover_receive_serial_numbers`.
* `sn_tracker_resume()` was added to `telegram.c` so a tracker can be resumed
  after a restart without the caller reaching into its fields.
* `relay_recover_trackers()` replaces the inline recovery loop, so restart
  behaviour is now covered by a test rather than by inspection.

### Verification

| Check | Before | After |
|-------|--------|-------|
| Test suites | 8 | **9** |
| Assertions | 131 483 | **131 600** |
| Relay policy tier | I / J (inside `toxrelayer.c`) | **U** (`relay.c`) |
| `toxrelayer.c` functions at tier J | 31 | **29** |
| Compiler warnings | 0 | 0 |
| Static-analysis findings | 0 | 0 (now covering 8 modules) |
| Sanitizer clean | yes | yes |

`tests/test_relay.c` (117 assertions) drives the relay against the **real**
telegram codec, the **real** delivery queue and the **real** message database, so
what is stored, what is queued and what is sent back on a replay are all
observed. Scenarios:

1. argument validation 2. malformed and foreign records 3. deliver and store
4. duplicate suppression 5. gap detection with the exact requested range
6. recovery of a replayed telegram 7. resynchronisation on an oversized gap
8. forwarding intact to a metaCom peer 9. forwarding body-only to a plain peer
10. unroutable record reported 11. replay answered from history
12. replay with nothing stored 13. tracker resume after a restart.

### Defects found while writing the tests

| ID | Defect | How it was found |
|----|--------|------------------|
| D-45 | The test's resync scenario sat exactly on the recovery-budget boundary (`gap == RELAY_MAX_GAP` is recoverable, `gap > RELAY_MAX_GAP` is not), so the boundary had to be characterised precisely | the assertion failed until the boundary arithmetic was made explicit |

---

## Increment 7 — Durable delivery queue: a restart no longer loses telegrams

### Defect

| ID | Defect | Location | Evidence before | Status |
|----|--------|----------|-----------------|--------|
| D-46 | The delivery queue lived **only in memory**. `mq_init()` wiped it at every start-up, so a telegram that was still waiting for an offline peer when the process stopped was lost forever — the queue's whole purpose is exactly that window. | `msg_queue.c`, `toxrelayer.c:main()` | there was no save/load path at all; the queue file did not exist | Fixed: new `src/mq_persist.c` + start-up restore and debounced flush |

### What changed

* **Extracted the queue's read side for mirrors** — `msg_queue.h` gained
  `mq_foreach()` (visit every stored message oldest first) and
  `mq_set_change_hook()` (fired after enqueue, confirmation and retirement).
  The ring buffer's internals stay private; the persistence module only sees the
  documented interface.
* **New `src/mq_persist.h` / `src/mq_persist.c`** — a zero-toxcore, stateless
  module that writes the queue to one versioned file and reads it back:
  * explicit little-endian encoding, so the file is host independent;
  * **atomic replace** — write `<path>.tmp`, `fflush`, `fsync`, then `rename`
    over the target. A crash leaves either the old queue or the new one, never a
    torn file, and no `.tmp` survives a successful save;
  * **validate before import** — the whole file is read and every record is
    range-checked before a single `mq_enqueue()` happens, so a corrupt file
    cannot leave the queue half restored;
  * every failure is a return code; the module never prints, exits or aborts.
* **`toxrelayer.c` wiring** — `main()` restores the queue right after `mq_init()`,
  `on_queue_changed()` sets a dirty flag, the event loop flushes at most once
  per `QUEUE_SAVE_INTERVAL` seconds, and `exit_toxrelayer()` forces a final flush.
  Restored telegrams start with a fresh attempt budget and are immediately
  eligible, which is the right policy: a restart is a new chance to reach the
  peer.
* **`QUEUE_FILE`** (`mq_queue.dat`) is declared in `toxrelayer.h` next to the other
  on-disk artefacts, so the files an operator must preserve are centralised.

### Verification

| Check | Before | After |
|-------|--------|-------|
| Test suites | 9 | **10** |
| Assertions | 131 600 | **135 760** |
| `toxrelayer.c` functions at tier J | 29 | **29** (the queue flush is delegated to tested code, so no new J function) |
| Compiler warnings | 0 | 0 |
| Static-analysis findings | 0 | 0 (now covering 10 modules) |
| Sanitizer clean | yes | yes |

`tests/test_mq_persist.c` (4 160 assertions) drives the **real** queue through
the **real** format inside a `mkdtemp()` directory. Scenarios: round trip in
FIFO order; path validation (NULL, empty, missing, unwritable target); atomic
replace with no leftover `.tmp`; empty queue; binary and maximal-size payload
fidelity; corrupt file rejected without touching the live queue (bad magic,
unknown version, short file); invalid records rejected before any import
(truncation, zero and oversized length, negative friend number, absurd count);
full queue refuses the restore; a simulated restart delivers the restored queue
in order; the change hook fires only on real changes.

### Defects found while writing the tests

| ID | Defect | How it was found |
|----|--------|------------------|
| D-47 | The change-hook test initially expected a retirement on the same tick as the failed attempt, but the queue's retry backoff defers it. The test now advances the injected clock past `MQ_RETRY_BACKOFF_SEC`, which pins the interaction between the backoff and the retirement path. | the assertion failed until the clock was advanced |

---

## Increment 8 — Audit follow-through: input validation, format safety, durability

Four of the five items left open by increment 7 are closed here; the fifth is
recorded below. The work was driven by the audit question "what can a hostile or
careless input still do", and it turned up more than the open list predicted.

### Defects

| ID | Defect | Location | Evidence before | Status |
|----|--------|----------|-----------------|--------|
| D-48 | `atoi()`/`atol()` parsed every console command and INI value: a non-numeric argument became `0`, and an overflowing one had undefined behaviour. `/default abc` therefore selected group 0, `/#abc` started a chat on channel 0, and a hand-edited INI serial became 1. | `commands.c` (7 sites), `toxrelayer.c` (4 sites) | `cert-err34-c` had to be disabled in `.clang-tidy` | Fixed: new unit-tested `parse_int_range()`; the check is enabled |
| D-49 | `log_timestamp()` and friends had no format attribute, so `log_timestamp(out_buffer)` passed a caller-influenced string as the format: a format-string vulnerability the compiler could not see | `toxrelayer.c:511` | first build after adding the attribute | Fixed: `LOG_FORMAT()` attribute + `log_timestamp("%s", out_buffer)` |
| D-50 | `mq_persist_save()` flushed the file before the rename but never flushed the directory, so the rename itself could be lost on a crash and the previous queue would reappear | `mq_persist.c` | atomic replace was incomplete for a real power loss | Fixed: `mq_sync_parent_dir()` after the rename |
| D-51 | `group_index()` returns -1 for an unknown group and two callers dereferenced it anyway, reading `g_chats[-1]` | `commands.c` (`/info`, `/title`), `toxrelayer.c` (profile display) | audit of every `group_index()` call site | Fixed: guarded; the table is treated as advisory |
| D-52 | A title was copied into `g_chats[].title` (`TOX_MAX_NAME_LENGTH`) with a length taken from a `TOX_MAX_MESSAGE_LENGTH` buffer — a heap overflow that only the library's own length check happened to prevent | `commands.c:cmd_title_set`, `toxrelayer.c:cb_group_titlechange` | audit of every copy into the group table | Fixed: the copy is clamped to the destination |
| D-53 | The quote-stripping idiom `len = strlen(x) - 1; x[len] = '\0'` writes one byte before the buffer when the argument carries a single quote, and duplicated the parsing rule in four places | `commands.c` (4 sites) | audit of the unquoting call sites | Fixed: one shared, unit-tested `unquote_str()` |
| D-54 | `make test` listed only the suite source as a prerequisite, so a changed module did not rebuild its suite: the gate could report success against a stale binary | `Makefile` | observed while running the suite after changing `misc.c` | Fixed: `$(EXTRA_SRC)` added to the prerequisite list |
| D-57 | The suites were executed from the *build* rule, so once the binaries were up to date `make test` printed nothing and exited 0 — a verification gate that could pass without running a single assertion | `Makefile` | a second `make test` in the same tree produced no output | Fixed: the `test` target now always runs every suite and fails on the first failure |
| D-55 | `relay_recover_trackers()` probed all 4096 channels at start-up — one database query per channel, of which at most a handful can exist | `relay.c`, `toxrelayer.c` | registered as remaining work in increment 7 | Fixed: optional `channel_known` filter on `relay_env`, wired to the peer directory |
| D-56 | `cb_friend_connection_change()` indexed `relay_name_list[friendnumber]` without the `friend_number_valid()` check that `toxrelayer.h` states as a rule for every friend-number index | `toxrelayer.c` | audit against the rule in `toxrelayer.h` | Fixed: guard added; out-of-range callbacks are logged and ignored |

### Interface changes

* `misc.h` gains two unit-tested helpers:
  * `parse_int_range(text, min, max, out)` — parses the **entire** string, rejects
    trailing junk, whitespace, `ERANGE` and out-of-range values, and leaves `*out`
    untouched on failure. It is now the only place in the code base where text
    becomes a number.
  * `unquote_str(arg, out, out_size)` — strips the surrounding quotes, tolerates a
    missing closing quote, always NUL terminates and never exceeds `out_size`.
* `log.h` gains `LOG_FORMAT(fmt, first_arg)`, expanding to
  `__attribute__((format(printf, ...)))` on GCC/Clang and to nothing elsewhere. It
  is applied to `log_timestamp`, `log_error_timestamp`, `console_out` and
  `console_err`, so every call site is now checked at compile time.
* `relay_env` gains an optional `channel_known` callback. `NULL` means "every
  channel in `[0, channels)`", so no existing caller or test changes behaviour.
* `mq_persist_save()` additionally flushes the containing directory. A failure of
  that flush is deliberately **not** an error: the rename has already happened,
  and some file systems do not support `fsync()` on a directory.
* `Makefile`: a suite now depends on `$(EXTRA_SRC)`, so changing a module rebuilds
  it, and the suites are executed by the `test` target itself, so `make test`
  always runs them and fails on the first failure.

### Verification

| Check | Before | After |
|-------|--------|-------|
| Test suites | 10 | 10 |
| Assertions | 135 760 | **135 812** |
| clang-tidy checks enforced | `cert-err34-c` disabled by deviation | **`cert-err34-c` enabled** |
| Compiler warnings | 0 | 0 (now with format checking on every log call) |
| Static-analysis findings | 0 | 0 |
| Sanitizer clean | yes | yes |

New coverage: `test_misc.c` (`parse_int_range` 25 assertions, `unquote_str` 11),
`test_mq_persist.c` (a subdirectory target that exercises the directory flush,
12 assertions) and `test_relay.c` (the channel filter bounds the start-up scan,
asserted by counting the probes: 4 assertions).

---

## Increment 9 — Renamed to ToxRelayer

The project is now called **ToxRelayer**. It had three names: *ToxBot* in the
build files and README, *metaRelayer* in the documentation and in the runtime
name, and *arkMeta* in the usage text.

### What changed

* `src/toxbot.c` / `src/toxbot.h` → `src/toxrelayer.c` / `src/toxrelayer.h`,
  moved with `git mv` so the history follows the files. Include guard
  `TOXBOT_H` → `TOXRELAYER_H`.
* The binary is built and installed as `toxrelayer`; `make`, `make install`,
  `make uninstall`, `make clean` and `.gitignore` use the new name.
* Runtime names: `Bot_Name` is `ToxRelayer@元宇宙` (the name contacts see), and
  `--help` prints `usage: toxrelayer` instead of the unrelated spelling
  `arkMeta`.
* `README.md` was rewritten around what the relayer actually does today,
  `commands.txt` uses the new name, and both documents in `docs/` were updated.

### Deliberately not renamed

* **`toxbot.tox`** — the toxcore save file holds the bot's private key, so it
  *is* the bot's address on the network. Renaming it would make the relayer
  start with a fresh identity and orphan every contact, master key entry and
  stored serial. The name is kept, and documented in `toxrelayer.h` and in the
  README; migrating it needs a failure-safe rename step and is separate work.
* **`struct Tox_Bot` / `Tox_Bot`** (188 occurrences) — an internal identifier,
  not part of the product name; renaming it is churn with no behaviour change.
* **`metaCom`** — the name of the peer protocol and network the relayer speaks
  (`metaCom.config`, `meta_chat_flag`, `peer_is_metacom`), not of this project.
* The vendored `src/new_meta/` tree (a copy of toxic) and the unused scratch
  sources under `src/` were left untouched.

### Observation recorded while writing the README

Documenting actual behaviour surfaced a discrepancy: `commands.txt` and the old
README both promised that a group-chat invite is accepted *from a master*, but
the check in `cb_group_invite()` is commented out, so the relayer joins an
invitation from **any** contact. That is security relevant and is not something a
rename should change silently, so it is recorded as open work below instead of
being altered here. The README states the current behaviour.

### Verification

| Check | Result |
|-------|--------|
| Compiler warnings | 0 |
| Test suites | 10, 135 812 assertions, 0 failures |
| Sanitizers | clean |
| Static-analysis findings | 0 |
| Binary name | `toxrelayer` |

---

## Increment 10 — Packaging the repository for publication

The tree had never been prepared for other people to clone: it carried upstream
build output, operator data and roughly 25 MB of vendored material, and only 16
files were under version control.

### Defects

| ID | Defect | Evidence before | Status |
|----|--------|-----------------|--------|
| D-58 | `origin` pointed at `https://github.com/JFreegman/ToxBot.git`, the upstream project. A bare `git push` would have targeted somebody else's repository. | `git remote -v` | Fixed: the remote was renamed to `upstream` and the branch tracking removed, so a push now fails with an explicit "no upstream branch" error |
| D-59 | The toxcore headers were vendored copies (0.2.12) in `src/`, and `#include "tox.h"` resolved to them, so the project compiled against older declarations than the library it linked (0.2.18) | `diff src/tox.h $(pkg-config --variable=includedir toxcore)/tox/tox.h` → 1 221 changed lines | Fixed: the copies were deleted and the nine includes now use `<tox/tox.h>`, `<tox/toxav.h>`, `<tox/toxencryptsave.h>` from the installed library |
| D-60 | The licence file `COPYING` was deleted in the working tree, and `.gitignore` listed it — a repository with GPL headers and no licence text | `git status` → `D COPYING` | Fixed: restored, and removed from `.gitignore` |
| D-61 | `.gitignore` covered neither the ten test binaries (only one was listed) nor `.vscode/`, `.DS_Store`, the SQLite files or the vendored trees, so build output and editor state showed up as candidates for commit | `git status` listing `test_relay`, `.vscode/`, `*.db` | Fixed: rewritten into build output / operator data / local-only sections |
| D-62 | Publishing would have included `toxbot.tox`, the private key that *is* the bot's identity, had `.gitignore` not happened to cover it | `*.tox` rule only | Confirmed ignored, and now documented as operator data with a comment explaining why it can never be committed |
| D-63 | `.github/FUNDING.yml` advertised the upstream author's sponsorship account | file contents | Removed |
| D-64 | No CI, no contribution or security guidance, no issue or pull request templates, no editor or whitespace configuration | absent from the tree | Added |
| D-65 | The `analyze` targets drove the clang static analyzer through `$(CC)`. On macOS `cc` is clang so this worked; on Linux `cc` is gcc, which has no `--analyze`, so the analysis job would have failed for every Linux user and in CI | `make analyze` on a machine where `cc` is gcc | Fixed: the analyzer now uses an explicit `CLANG ?= clang` |

### What changed

* **`.gitignore`** rewritten into three labelled sections, with the test-binary
  pattern anchored as `/test_*` so it cannot also swallow the suite sources under
  `tests/`. Verified with `git check-ignore` in both directions: operator data,
  build output and the vendored trees are ignored; `tests/*.c`, `src/relay.c`,
  `COPYING` and the documents are not.
* **Nine includes** moved from quoted local headers to the library's own
  `<tox/...>` headers, in the six published sources plus one test. Verified by
  building, testing, sanitising and analysing with the vendored copies removed.
* **`.github/workflows/ci.yml`** — build, test and sanitise on Linux and macOS,
  plus a separate static-analysis job running `make analyze-strict`. Actions are
  pinned to a major version and the workflow uses read-only `contents`
  permission, cancels superseded runs, and enforces `fail-fast: false` so one
  platform does not hide the other's result.
* **`CONTRIBUTING.md`** — the four gates, the coding standard as it is actually
  enforced, how to add a test, and commit expectations.
* **`SECURITY.md`** — private reporting channel, plus the operational facts that
  decide the cost of a compromise (the identity file, `masterkeys`, the
  auto-accepting friend policy, the disabled invite gate, unencrypted archive).
* **`.editorconfig`**, **`.gitattributes`** (LF everywhere, binaries marked),
  **`.github/PULL_REQUEST_TEMPLATE.md`** and three issue-template files.
* **`README.md`** gained origin/attribution credit, a CI pointer and an accurate
  dependency statement.

### Verification

| Check | Result |
|-------|--------|
| `git check-ignore` | operator data ignored, sources visible |
| `make` with vendored headers removed | 0 warnings, 0 errors |
| Test suites | 10, 135 812 assertions, 0 failures |
| Sanitizers | clean |
| Static-analysis findings | 0 |

The CI workflow itself cannot be executed locally; the commands it runs are the
same four gates that pass here, and the Linux package names are the
distribution's defaults but were not verified on a Linux host.

---

## Increment 11 — The first CI run found what macOS had hidden

The workflow added in increment 10 failed on its first run, on both platforms.
Every failure was a **real portability defect** that the local toolchain
(Apple clang 14, toxcore 0.2.18) could not see: the suites, the sanitizers and
the analyzer were all green locally while the project did not compile at all
with GCC.

### Defects

| ID | Defect | Why the local build missed it | Status |
|----|--------|-------------------------------|--------|
| D-66 | `src/toxrelayer.c` called 41 string and memory functions without including `<string.h>`; it included `<strings.h>`, the BSD interface, instead | Darwin's `<strings.h>` pulls in `<string.h>`; glibc's does not. Apple clang 14 also only warned about the implicit declarations, while newer clang and gcc make them errors | Fixed: `<string.h>` is included |
| D-67 | `numfriends < 0` compared an unsigned value | GCC reports `-Werror=type-limits`; clang does not implement that warning | Fixed: the dead comparison is gone |
| D-68 | `console_out("... %lu", TOX_ADDRESS_SIZE)` did not match its format | toxcore 0.2.18 expands `TOX_ADDRESS_SIZE` to a `size_t` expression, so `%lu` was correct there; 0.2.23 defines it as the plain integer `38`, so it is wrong | Fixed: the value is cast to `long` and printed with `%ld`, which is correct against both |
| D-69 | `chat_tx_Control[MAX_Friend_NUM]` was **defined** in `toxrelayer.h`, so every translation unit that included it emitted its own copy | A tentative definition is merged by older toolchains; GCC 10+ and LLVM 11+ default to `-fno-common` and fail at link with `duplicate symbol`. Apple clang still defaults to `-fcommon`, so macOS linked and Linux would not | Fixed: the header declares it `extern` and `toxrelayer.c` defines it once |

### What this validates

* A green `make test` on one machine is not evidence that the project builds on
  another. The gates are only as strong as the toolchain running them, which is
  precisely what the CI matrix turns into a fact.
* The CI build stops at the first translation unit that fails, so it had reached
  only `toxrelayer.o`. All eleven build units were therefore re-checked locally
  with a newer clang under `-Wall -Wextra -Werror -Wtype-limits` before the fix
  was pushed, rather than fixing one failure at a time through the CI loop. That
  is what surfaced D-69: the link is only reached after every object compiles.
* Reproducing the platform is part of the work. A second toolchain on the same
  machine (Homebrew LLVM clang 21, which defaults to `-fno-common` and rejects
  implicit declarations) reproduced the Linux failures exactly, including the
  link error, without needing a Linux host.

### Verification

| Check | Result |
|-------|--------|
| `make` with Apple clang 14 | 0 warnings, 0 errors |
| `make` with Homebrew LLVM clang 21 (incl. link) | 0 warnings, 0 errors, binary produced |
| `make test` with Homebrew LLVM clang 21 | 10 suites, 135 812 assertions, 0 failures |
| All sources under `-Wtype-limits` with clang 21 | clean |
| Test suites (Apple clang) | 10, 135 812 assertions, 0 failures |
| Sanitizers | clean |
| Static-analysis findings | 0 |

---

## Remaining work

1. Decide on the group-chat invite gate: `cb_group_invite()` accepts an
   invitation from any contact because its `friend_is_master()` check is
   commented out, which contradicts `commands.txt`. Either re-enable the gate or
   document and test the open behaviour deliberately.
2. Move the hard-coded DHT bootstrap node list into a configuration file
   (`toxrelayer.c` still marks it `TODO: hardcoding is bad`).
3. Extract the remaining inline handling in `cb_friend_message()` so more of the
   relay logic is covered by tests rather than inspection.
4. `toxkey.c` still calls `atoi()`, but it is in neither the build (`OBJ`) nor the
   analysis set (`ANALYZE_SRC`). If it is ever promoted to a built target it must
   move to `parse_int_range()` first, because the gate now enforces
   `cert-err34-c`.
5. The remaining `strtol()` calls in `telegram.c` validate their end pointer and
   range-check the result, but do not test `errno` for overflow. The narrowing
   cast currently catches it; an explicit `ERANGE` check would make that
   independent of the conversion.
6. The warning set covers `-Wall -Wextra` but not `-Wshadow`, `-Wconversion` or
   `-Wcast-qual`. A trial build of the ten analysed modules with those flags
   reports 151 warnings (53 in `toxrelayer.c`, 83 in `commands.c`, 11 in
   `groupchats.c`, 4 in `misc.c`; the other six modules are clean). Enabling them
   is an increment of its own rather than a drive-by change.
7. Rename the toxcore save file `toxbot.tox` to match the project, via an
   explicit and failure-safe migration (extend `legacy_data_file_rename()`), so
   that an existing identity is moved rather than replaced.

