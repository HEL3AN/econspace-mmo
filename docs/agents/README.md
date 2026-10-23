# Building a bot for EconSpace

EconSpace treats an AI agent as an **ordinary player**. The game ships its own
[MCP](https://modelcontextprotocol.io) server, `econagent`, and a bot built on it plays by
exactly the rules a human does: it logs in with an account, flies a ship the server owns, and
can do nothing a human client could not.

This guide is for anyone who wants to build one — with a language model deciding, or with a
plain script.

- [How it fits together](#how-it-fits-together)
- [Run one in five minutes](#run-one-in-five-minutes)
- [Connect a model](#connect-a-model)
- [What a bot can see](#what-a-bot-can-see)
- [What a bot can do](#what-a-bot-can-do)
- [Waiting: the event journal](#waiting-the-event-journal)
- [Writing your own](#writing-your-own)
- [What a bot cannot do yet](#what-a-bot-cannot-do-yet)

---

## How it fits together

```
 your bot  ──MCP over stdio──▶  econagent  ──game protocol over TCP──▶  econserver
 (a model,                       (an MCP server                          (the world)
  or a script)                    and a game client)
```

`econagent` wears two hats. Toward your bot it is an **MCP server** speaking JSON-RPC 2.0 on its
stdin and stdout. Toward the game it is an **ordinary client**, the same as the one a human
plays with, linked against the same protocol code — so the wire format has one implementation
and a bot cannot drift from what the game actually accepts.

Two consequences worth knowing before you start:

- **A bot gives orders, not keystrokes.** Tools like `mine` and `dock` hand the server a
  *standing order* that it carries out over seconds or minutes. Nobody streams thrust sixty
  times a second.
- **The server is the authority.** An order the server refuses — a station that will not dock
  you, an object that is not in this system — comes back as a refusal you can read, not as an
  exception.
- **A new pilot chooses where to begin.** A fresh account (or one whose system the galaxy no
  longer has) has no ship until it picks a start: `observe` then lists the capital of each
  civilisation, and `choose_start` takes one by id, or `random`. `econagent connect ...
  --start random` answers it for a bot that never calls a tool.

---

## Run one in five minutes

Build the project first ([README](../../README.md#build)). Then, in two terminals:

```sh
# 1. a server
./build/bin/server/econserver.exe host 50800

# 2. the example bot -- no model, no key, just the Python standard library
python examples/agents/mining_bot.py ./build/bin/agent/econagent.exe 127.0.0.1 50800
```

It mines a belt until the hold is full, docks, and sells the ore. What it prints, trimmed:

```text
connected to econspace
tools: observe, move_to, dock, undock, mine, travel_to_system, abort_order, wait_for_event, sell_cargo
SYSTEM Lysen (r22-1-1)  security 1.00  controlled by Traders Guild
SHIP   hull 100/100  shields 60/60  cargo 0/50  money 500 cr
       flying  stabilizer on  weapons off  mining off
NEARBY (31 shown, 6 further away not listed)
  #8    station  Lysen Exchange           3883u NE  dockable
  #13   field    Inner Iron Belt          7211u NE  Iron
  ...
mining field #13 until the hold is full
  mine: accepted (order 1). It runs on the server; use wait_for_event to sleep until it finishes.
  events: [1] order_done: mine: hold full
docking at station #8
  events: [2] docked: Docked at Lysen Exchange | [3] order_done: dock: docked
selling resource Iron -> sell sent; call observe to see the result
SHIP   hull 100/100  shields 60/60  cargo 0/50  money 1000 cr
```

That file is about a hundred and fifty lines and is meant to be read. CI runs it against a real
server on every change, so it cannot quietly stop working.

**There is also a built-in scripted run**, with no Python at all:

```sh
./build/bin/agent/econagent.exe selftest 127.0.0.1 50800 probe its-secret
```

---

## Connect a model

`econagent connect <host> <port> <name> <secret>` starts the MCP server. Any MCP client that
launches a server as a subprocess can use it.

**Claude Code:**

```sh
claude mcp add econspace -- /path/to/econagent.exe connect 127.0.0.1 50800 agent its-secret
```

**Anything else:** point the client at the same command line. The server announces MCP protocol
version `2024-11-05` and offers tools, resources and prompts.

**Accounts.** `<name>` is the account the bot plays as; `<secret>` is its secret. The first login
sets it, and every later login has to match — a wrong secret ends the connection. Name and
secret come as a pair: giving one without the other is refused with exit code 2, because a
default secret on a named account would be a password everybody knows. With neither, the bot
plays as `agent`.

**One account, one session.** If the name is already playing somewhere, the newcomer displaces
the earlier connection, which is saved first. Give each bot its own name.

**stdout is the protocol.** `econagent` writes JSON-RPC and nothing else on stdout; all
diagnostics go to stderr. If you wrap it, keep the two apart.

---

## What a bot can see

### `observe` — the world as text

A model reads text, so the world arrives as text designed to be read: what matters first, every
object with an id an order will accept, and nothing repeated that costs tokens without telling
the reader anything.

```text
SYSTEM Lysen (r22-1-1)  security 1.00  controlled by Traders Guild
SHIP   hull 100/100  shields 60/60  cargo 50/50  money 500 cr
       flying  stabilizer on  weapons off  mining off
CARGO  Iron 50
HOSTILE
  #41   pirate   Syndicate raider          900u S   Pirates  hull 100%
NEARBY (31 shown, 6 further away not listed)
  #8    station  Lysen Exchange           3883u NE  dockable
  #13   field    Inner Iron Belt          7211u NE  Iron
  #24   gate     Gate to Sigma Reach     23195u E   to reach
MARKET (bid/ask x stock; fee 2% a trade) Iron 9.5/10.5 x400, Ice 14.3/15.8 x300, ...
OFFERS (4 at this station)
  [0] Delivery  471 cr
EVENTS
  ...
```

| Section | What it says |
|---|---|
| `SYSTEM` | where you are, how safe it is, who controls it |
| `SHIP` | hull, shields, cargo, money, and what the ship is doing |
| `CARGO` | what is in the hold, **by name** — the names `sell_cargo` takes |
| `HOSTILE` | anything that would shoot you, listed first and in full, because it is the reason to change plan |
| `NEARBY` | everything else, nearest first. Stations and gates are always listed; other objects are capped (`detail: "full"` raises the cap) |
| `MARKET` | when docked: per good, what the market pays / what it asks, and how many it holds -- the names `buy` takes |
| `STORAGE` | when docked at your own structure, or a corporation mate's: what its storage keeps of how much it holds -- what `store_cargo` and `take_cargo` move |
| `LAW` | your security status, and a `CRIMINAL` flag while one is on you |
| `SCAN`, `HOLE` | the scanner: a pass under way and the signatures found; every roaming wormhole you know of, with the mass it takes and when it closes |
| `LAST LOSS` | the last ship you lost, and what insurance paid |
| `OFFERS`, `MISSIONS` | mission work at this station, and what you have taken |
| `EVENTS` | what happened recently |
| `STANDING` | reputation with each faction |

Each object line is `#<id>  <kind>  <name>  <distance>u <bearing>  <details>`, column-padded.
**The id is what every order takes.** Kinds are `star`, `planet`, `station`, `field`, `gate`,
`nebula`, `derelict`, `structure`, another player as `pilot` (with `player` in its details —
someone who decides things for themselves, not scenery), and NPCs by role — `trader`, `miner`,
`police`, `pirate`, `warship`, and a corporation's `guard`.

### Resources

| URI | What |
|---|---|
| `econspace://system` | the same picture as `observe`, as a resource |
| `econspace://galaxy` | every system on your charts (public, your own, your corporation's), its links, security and controller, and the news — what a bot plans a journey with |

---

## What a bot can do

Forty-four tools, in these groups. **Their exact arguments are in [reference.md](reference.md)**,
which is generated from the server itself and checked by CI, so it cannot disagree with what the
server actually accepts.

| To | Tools |
|---|---|
| look and wait | `observe`, `wait_for_event`, `choose_start` |
| fly | `move_to`, `dock`, `undock`, `hold_station`, `travel_to_system`, `abort_order` |
| earn | `mine`, `salvage`, `sell_cargo`, `buy`, `store_cargo`, `take_cargo`, `missions`, `accept_mission`, `complete_mission` |
| ships | `hangar`, `buy_ship`, `switch_ship`, `insure_ship`, `fitting`, `preview_fit`, `refit`, `designs`, `save_design`, `apply_design`, `rename_design`, `delete_design` |
| fight and the law | `attack`, `pay_bounty` |
| build | `blueprints`, `deploy`, `build`, `dismantle`, `set_defence`, `buy_licence` |
| explore | `scan`, `name_system` |
| talk and organise | `chat_send`, `chat`, `corp`, `corp_act` |

A few that are worth knowing before reading the reference:

- `move_to` takes an object id *or* a point, and `warp` for anything far away.
- `mine` with `until_full` keeps going until the hold is full or the belt runs out.
- `travel_to_system` is one order for a whole journey, gate by gate, planned over the systems
  you know (charted, and one gate past them). `preference` is `shortest`, `safer` (security
  0.5 and up where any way does) or `less_secure`; `via` names systems to pass through first.
- `sell_cargo` takes a resource by the **name** `CARGO` shows — `"Iron"`, not an index.
- `store_cargo` and `take_cargo` move goods between the hold and the storage of the structure
  you are docked at -- your own outpost or station, or a corporation mate's (#438).
- `buy` takes a good by the name `MARKET` shows and buys as many as the stock, the hold and
  the money allow, up to `amount`. Every unit moves the price of the next, and every trade --
  a human's or a bot's -- pays the market's fee (#348), so a small price difference loses money.
- `missions` numbers the station's board and your active jobs; `accept_mission` and
  `complete_mission` take those numbers. An offer's number is its own and stays the same while
  it is on the board -- a station keeps its work for a while (#425), so docking again does not
  draw a new board -- and a number that has gone is refused, never read as another job. Each
  tool refuses a call missing an argument it requires, rather than reading it as 0 (#425).
  Each job says what it still needs and where it is
  handed in. Jobs posted by a faction's plans (`supply`, `escort`, `siege`) name the system
  they are about and a deadline. They are handed in anywhere, and they move the world: a
  delivery speeds a build, an escort guards a convoy, a besieger shot down weakens a siege.
- `hangar` quotes ships at the price *you* would pay here, which depends on your standing
  with the station's owner. `buy_ship` and `switch_ship` take a ship by name.
- `attack` closes to weapon range and fires until the target is gone. Shooting what the law
  protects is a crime, with the same flag, security status and police a human gets; the
  tool's description in the reference says exactly when.
- `corp_act` is every corporation verb -- invite, roles, rules of engagement, police budgets,
  patrols -- the way the client's corporation window gives them.
- `pay_bounty` pays the station owner's bounty unless you name another faction.
- The station verbs are not orders: they need you docked, they say why when they cannot
  happen, and they report what the server actually did -- the money paid, the job taken.

Every tool answers in plain text. An order that is accepted says so and returns at once; the
order itself runs on the server, and you learn how it ended from the journal.

### Prompts

Five ready-made plans a client can offer its user: `mining_run`, `trade_run`, `scout`,
`contract_work` and `patrol` — their full text is in [reference.md](reference.md#prompts). They are short instructions to a model, written in terms of the tools above, and are
a reasonable place to start writing your own.

---

## Waiting: the event journal

**This is the habit that matters most.** An order takes seconds or minutes. A bot that calls
`observe` in a loop to see whether it has finished burns turns, tokens and money. Instead:

```text
mine(field_id=13)        -> accepted (order 1)
wait_for_event()         -> [1] order_done: mine: hold full
```

`wait_for_event` sleeps until something happens and returns everything that has happened since
the last time it returned. If something already has, it returns at once. Each event is
`[seq] kind: text`, and `seq` only ever increases.

| Kind | Means |
|---|---|
| `order_done` | a standing order finished as asked |
| `order_failed` | a standing order gave up; the text says why |
| `docked`, `undocked` | |
| `jumped` | arrived in another system |
| `cargo_full` | the hold filled up |
| `under_attack` | something is shooting at you |
| `ship_destroyed` | your ship was lost, with its fit and cargo; you wake docked at your home station in the free starter, and observe's `LAST LOSS` line says what insurance paid (`insure_ship`) |
| `notice` | anything else worth saying |

Decide on the **kind**. The text is for people and may be reworded.

---

## Writing your own

The example bot is the template. Everything in it is ordinary MCP:

1. Start `econagent connect …` as a subprocess.
2. Send `initialize`, then the `notifications/initialized` notification.
3. `tools/call` with a name and arguments; read the `text` parts of the result.
4. Loop: observe, decide, give an order, `wait_for_event`, repeat.

A model-driven bot is the same loop with a model choosing step 4. Two things make a large
difference to how well a model plays:

- **Give it the journal, not the clock.** Tell it to wait on events. A model that is not told
  will poll.
- **Give it the galaxy.** `econspace://galaxy` is what lets it plan a route or judge risk rather
  than guess from one system's view.

Ideas for bots worth building are welcome as issues — and so are the bots.

---

## What a bot cannot do yet

The agent surface follows the game: what a player sees reaches `observe`, and a verb a player
has is a tool. What neither has yet is not here either -- trading charts (#431 slice 16), and
commanding a fleet of ships (#32). A bot also cannot steer by hand: it gives orders, and a
fight is flown with `attack` and `hold_station` rather than with thrust.

If your bot needs something it cannot do, open an issue and say what.
