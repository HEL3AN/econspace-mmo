# Security

EconSpace is a networked game with accounts and a login, so it has the kind of bugs that
should not be reported in public first.

## Reporting a vulnerability

**Please report privately**, through GitHub's
[private vulnerability reporting](https://github.com/HEL3AN/econspace-mmo/security/advisories/new)
(the *Report a vulnerability* button on the repository's **Security** tab). It reaches the
maintainer without being visible to anyone else, and the fix can be discussed and released
before the details are public.

Please do not open an ordinary issue for anything on the list below.

## What counts

Anything that lets a client do what the server should not allow, or learn what it should not
know. In this codebase that means, among others:

- **The login** (#106). The server sends a nonce and the salt; the client answers with
  `H(nonce ‖ H(salt ‖ secret))`, so the secret never crosses the wire after an account is
  created. A way to log in as someone else, to recover a secret, or to replay an answer is a
  vulnerability. There is no transport encryption — that is a known limitation, documented in
  `CLAUDE.md`, not a finding.
- **The transport** (#14). A frame length is four bytes the peer chose; `TcpConnection` caps it
  and caps the send backlog. A way to make the server allocate, block or crash on input is a
  vulnerability.
- **Authority**. The server owns the world. A client that can move faster than the simulation
  allows, act on something it cannot reach, trade without being docked, or change another
  player's account has found a hole in it.
- **Saves**. Account and world files carry a schema version and refuse a newer one (#20). A
  crafted file that the server loads into something wrong, or writes back over a real account,
  belongs here.
- **`econagent`**. It is an MCP server on stdio and a game client; anything that lets a tool
  call escape what an ordinary player could do is in scope.

## What to expect

This is a small project maintained in the open. A report will be acknowledged, and a fix for a
real issue will be credited to the reporter unless they would rather it were not.
