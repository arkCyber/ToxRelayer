# Security policy

ToxRelayer is a long-running node that accepts connections from the Tox network
and stores other people's messages, so treat it as an exposed service.

## Reporting a vulnerability

Please do **not** open a public issue for a security problem. Use GitHub's
*Report a vulnerability* form on the **Security** tab of this repository, or
contact the maintainers privately.

A useful report contains:

* what you did, what happened, and what you expected;
* the affected version or commit;
* whether it can be triggered by a **remote contact** over the Tox network or
  needs local access to the machine;
* a minimal reproduction if you have one.

We aim to acknowledge a report within a few days.

## Supported versions

The project is pre-1.0: only the current `master` is supported.

## Operational notes

These are properties of the design, not vulnerabilities, but they decide how
much a compromise costs:

* **`toxbot.tox` is a private key.** It holds the bot's identity. Anyone who has
  the file can impersonate the relayer. It is excluded from the repository by
  `.gitignore`; keep it that way, and keep the file mode `0600`.
* **`masterkeys` is the authorisation list.** A public key in that file may use
  every command, including `/master` (which grants the same rights to somebody
  else). Restrict write access to it.
* **Friend requests are accepted automatically** unless the sender's key is in
  `blockedkeys`. The blocklist is the only gate at that point, so populate it
  before exposing the node.
* **Group-chat invitations are accepted from any contact.** The master-only
  check in `cb_group_invite()` is currently disabled; see the open items in
  `docs/REMEDIATION_LOG.md`.
* **Stored messages are not encrypted by the relayer.** `metaChatMsg.db` holds
  telegram bodies as plain text. Tox protects them in transit, not at rest.
* Run the relayer as an unprivileged account, in a directory that only that
  account can read, and do not expose it through a proxy that bypasses Tox's
  own end-to-end encryption.
