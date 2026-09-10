# ToxRelayer

ToxRelayer is a store-and-forward relay bot for the [Tox](https://tox.chat)
network. It keeps a [toxcore](https://github.com/toktok/c-toxcore) node
permanently online, accepts friend requests automatically, and relays traffic
between contacts that cannot reach each other directly — storing telegrams for
offline peers, detecting lost ones and replaying them on request.

It is controlled entirely over ordinary Tox messages; there is no separate front
end, protocol gateway or web service.

> Renamed from *ToxBot* / *metaRelayer*. The change, the hardening work behind it
> and every remaining known limitation are recorded in
> [`docs/REMEDIATION_LOG.md`](docs/REMEDIATION_LOG.md).

## What it does

* **Presence.** Keeps one node online and bootstraps to the DHT whenever the
  connection drops.
* **Friend handling.** Accepts incoming friend requests automatically, unless
  the sender's public key is listed in `blockedkeys`.
* **Store-and-forward.** A telegram addressed to a contact the relayer knows is
  queued and delivered as soon as that contact can be reached. Telegrams
  addressed to somebody else are forwarded on to the peer on that channel.
* **Bounded retry.** Every queued telegram carries an attempt budget. Delivery
  is retried with a back-off and a confirmation timeout, and a telegram that
  keeps failing is retired as undeliverable rather than blocking the queue.
* **Gap detection and replay.** Received telegrams carry a serial number. A gap
  within the recovery budget makes the relayer ask the peer to replay the
  missing range; a gap too large to recover makes it resynchronise instead of
  asking for an unbounded replay.
* **Durable queue.** The delivery queue is mirrored to disk, so telegrams still
  waiting for their peer survive a restart instead of being lost with the
  process.
* **Message archive.** Received and sent telegrams are stored in a SQLite
  database, which is also what answers replay requests after a restart.
* **Group chat management.** Contacts can create groups and request invites;
  masters can set titles, passwords and the default room.

## Requirements

* a C11 compiler (clang or gcc)
* `pkg-config`
* [libtoxcore](https://github.com/toktok/c-toxcore) and `libtoxav`
* `sqlite3`

The toxcore headers come from the installed library (`#include <tox/tox.h>`);
the project does not vendor a copy, so the headers and the library you link are
always the same version. `pkg-config --modversion toxcore` tells you which.

On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libtoxcore-dev libsqlite3-dev
```

## Building

```sh
make            # builds ./toxrelayer
make install    # installs to $(PREFIX)/bin, default /usr/local
```

`make` treats every warning as an error (`-Wall -Wextra -Werror`). If you get
`cannot open shared object file`, run `sudo ldconfig`.

## Verifying

The verification gates are part of the build, not an afterthought:

```sh
make test            # 10 suites, 135 812 assertions, no network, no sleeping
make test-sanitize   # the same suites under AddressSanitizer + UBSan
make analyze         # clang static analyzer + clang-tidy
make analyze-strict  # the same, failing on any new finding
```

`make test` always runs the suites and fails on the first failure. Every suite
is self-contained: it injects time instead of sleeping, uses an offline Tox
instance where a handle is required, and writes only inside a `mkdtemp()`
directory. See [`docs/TEST_COVERAGE.md`](docs/TEST_COVERAGE.md) for the
function-by-function traceability matrix.

CI runs the same gates on Linux and macOS — see
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Running

```sh
./toxrelayer [OPTION] ...
```

| Option | Meaning |
|--------|---------|
| `-4`, `--ipv4` | Force IPv4 |
| `-L`, `--no-lan` | Disable LAN discovery |
| `-p`, `--SOCKS5-proxy <ip> <port>` | Use a SOCKS5 proxy |
| `-P`, `--HTTP-proxy <ip> <port>` | Use an HTTP proxy |
| `-t`, `--force-tcp` | Force connections through TCP relays (DHT disabled) |
| `-h`, `--help` | Show usage and exit |

The relayer runs in the foreground and logs to stdout. `Ctrl-C` exits cleanly:
the delivery queue is flushed to disk on the way out.

DHT bootstrap nodes are currently compiled in. Moving them into the
configuration file is tracked as remaining work in the remediation log.

## On-disk files

All paths are relative to the working directory, so run the relayer from the
directory that holds its data.

| File | Contents |
|------|----------|
| `toxbot.tox` | **The bot's identity.** The toxcore save file, holding its private key. See the note below. |
| `masterkeys` | One public key per line. Only these contacts may use the master commands. Created empty on first use. |
| `blockedkeys` | Public keys whose friend requests are refused. |
| `metaCom.config` | INI file holding the per-channel serial numbers and the metaCom peer flag. |
| `metaChatMsg.db` | SQLite archive of stored telegrams; answers replay requests. |
| `mq_queue.dat` | Durable mirror of the delivery queue. Safe to delete only if losing queued telegrams is acceptable. |

### About `toxbot.tox`

This file **is** the bot's address on the network: it holds the private key, and
without it the relayer starts with a brand-new identity, orphaning every contact
and every master key entry. It deliberately still carries its pre-rename name —
renaming the project must not rename the account. Do not delete it, and do not
rename it unless you migrate it deliberately.

## Controlling

Add the relayer as a friend, then send commands as ordinary messages. To use the
master commands, put your own public key in `masterkeys` first; anyone else gets
`You do not have permission to use this command.`

### Available to any contact

| Command | Effect |
|---------|--------|
| `/help` | Print the command list (masters see an extra line) |
| `/id` | Print the relayer's address |
| `/info` | Print status and the active group chats |
| `/invite` | Request an invite to the default group chat |
| `/invite <n> <pass>` | Request an invite to group chat `n` |
| `/group <text\|audio> [pass]` | Create a group chat |
| `/default <n>` | Set the default room to `n` |

### Master commands

The same list is in [`commands.txt`](commands.txt), which is what `/help` refers
to. Text arguments must be enclosed in double quotes.

| Command | Effect |
|---------|--------|
| `/default <n>` | Set the default group chat room |
| `/gmessage <n> "<msg>"` | Send a message to group chat `n` |
| `/leave <n>` | Leave group chat `n` |
| `/master <id>` | Append a Tox ID to `masterkeys` |
| `/name "<name>"` | Set the relayer's name |
| `/passwd <n> ["<pass>"]` | Set (or clear) the password of group chat `n` |
| `/purge <days>` | Set how many days before an inactive contact is deleted |
| `/status <online\|away\|busy>` | Set the user status |
| `/statusmessage "<msg>"` | Set the status message |
| `/title <n> "<title>"` | Set the title of group chat `n` |

## Security notes

* The master check is by public key, so keep `masterkeys` writable only by the
  account that runs the relayer.
* Group-chat invitations are currently **accepted from any contact**: the
  master-only gate in `cb_group_invite()` is commented out in the source. This
  is recorded as an open item; treat it as known behaviour until it is decided.
* The relayer only ever logs what an operator's own commands echo back; it does
  not write message contents anywhere else.

## Project layout

```
src/             sources
  toxrelayer.c   composition root: toxcore, bootstrap, event loop, callbacks
  commands.c     command parsing and dispatch
  relay.c        routing and store-and-forward policy (no toxcore)
  telegram.c     ZCZC/NNNN wire codec and receive-sequence tracking
  msg_queue.c    delivery queue: attempt budget, back-off, confirmation
  mq_persist.c   durable mirror of the delivery queue
  msg_database.c SQLite archive
  misc.c, log.c, groupchats.c    utilities
tests/           one self-contained suite per module
docs/            coverage matrix and remediation log
```

## Documentation

* [`docs/TEST_COVERAGE.md`](docs/TEST_COVERAGE.md) — every function, the test
  that verifies it, and how to add a suite.
* [`docs/REMEDIATION_LOG.md`](docs/REMEDIATION_LOG.md) — defect history, design
  decisions and the open work list.
* [`commands.txt`](commands.txt) — master command reference.

## Origin and credits

ToxRelayer began as a fork of
[ToxBot](https://github.com/JFreegman/ToxBot) by JFreegman, and this repository's
history still carries those upstream commits. The group-chat and command
framework grew out of that project; the relay protocol, the routing policy, the
delivery queue and its durable mirror, the message archive and the test suite
were added here. If you fork this work, please keep the attribution chain
intact.

## License

GNU General Public License v3 or later, as stated in the header of every source
file — see [COPYING](COPYING) for the full text.

