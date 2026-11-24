# Player-to-player exchange

Status: **design draft, 2026-11-25. Nothing here is built, and no code is written until the
owner has answered the [decisions](#decisions-for-the-owner).** Calls made where the issue is
silent are marked *(call)*.
Related: #473 (this), #472 (holds, hangars, located ships, modules as goods, the NPC delivery
service), #468 (structure ownership), #431 slice 16 (charts sold to factions, built), #346
(scanning), #347 (corporations), #345 (faction jobs and contracts), #343 (the market), #42
(agents), #297 (the window system).

## Why

Today a pilot can trade with the world and not with anyone in it. Goods go to a market and come
back from it; charts go to a faction; a corporation shares its treasury and its structures but
a pilot cannot hand a friend ten units of Iron without jettisoning them into a can. The owner's
starting point was selling a chart to another pilot (#473); the general shape is wider: *any
two parties should be able to swap anything they own, where they are, with the server standing
between them so neither can cheat.*

The premises that frame it (CLAUDE.md):
- **This is an MMO and the server is authoritative.** The server is the only escrow there is,
  and it is a perfect one. That removes whole classes of fraud EVE needs rules for; the
  [fraud section](#4-trust-and-fraud) says which.
- **A new feature extends what is already built.** Everything below grows `Contract`, the
  container model of #472, the market and the journal. It adds one noun (a *deal*, which is
  the `Contract` generalised) and no store, window or message that an existing one could carry.
- **The economy is living: nothing from nothing.** Player exchange moves credits and goods
  between parties. It never mints either, and it never lets a player route around the
  market's fee or the NPC market's price discipline.
- **No remote trading** (#472 decision). Goods exist in one place and are carried; exchange
  happens where the parties are docked.

## What exists

| Piece | Where | What it gives the exchange |
|---|---|---|
| Faction `Contract` (a market short of a good posts "bring N units by a deadline for a price") | `Simulation_Contracts.cpp`, `Contract` in `Simulation.h`; saved with the world | The record, the board at a station, the deadline, the reward set aside when it is posted and given back when it ends, the hand-in docked with goods in the hold. Today the issuer is always a faction's treasury |
| Containers `(place, holder, division)` and one `move` verb | #472 slices 1-4, `Container`/`ContainerOf` | Where escrow lives; a hangar has reserved room; goods are never duplicated by a move |
| Personal hangars at NPC stations | #472 slice 3 | The place a delivered bundle lands, so *an offline party can be paid* |
| `StepPlayerBuy/Sell`, `Market`, `FeeAt` | `Simulation_Player.cpp`, `economy/Market.h` | The price curve, the market's fee to its owner or the system's holder, the reputation markup |
| NPC delivery service, `Shipment`, `QuoteDelivery` | `Simulation_Delivery.cpp` | The route quote that is the ceiling of a courier's fee (#472 section 5) and the "in transit, arrives when the world clock says" machinery |
| Located ships with their own hold | #472 slice 8 | A ship is something at a station with an id, a fit and a hold, so it can be listed |
| Structure transfer (`CorpAct transfer`), owners `account`, `corp:<id>`, `faction:<id>` | #468 | Structures can change hands with their licence, rules and storage; `StructureParty`/`PartyOf` say who a party is |
| Per-pilot knowledge: `ClientSession::known` (`system:<id>` keys), `scanned`; corporation charts; `ChartOfferFor` and `SellCharts` | #431, slice 16 | What a chart *is* in the data, and how a faction values it |
| Journal and notices (`Events`) | `Events.h` | Offline notice of "your contract was taken" |
| Corporations, treasury, divisions with role rights | `Corporation.h`, #472 slice 5 | A corporation as issuer or recipient |
| Drag and drop in the framework | #472 slice 1 | The trade window and the contract editor are drag targets, not forms |
| Agent tools `sell_charts`, `move_goods`, `corp_act`, `assets`, `request_delivery` | `docs/agents/reference.md` | The shape new verbs take (an action argument, the server's text as the answer) |

## How others do it

| Game | Direct | Contracts | Market | What we take |
|---|---|---|---|---|
| **EVE Online** | A trade window: both put items and ISK in, each confirms, any change un-confirms. Only in the same station or space | Item exchange, courier (collateral, reward, route, days to complete), auction. Public, to a character, a corporation or an alliance; the issuer's items are held by the game at the station. A free-text title | Regional order book, orders placed and filled within a range of jumps; broker fee on placing, sales tax on selling; NPC sell orders are ordinary orders in the same book | The trade window and its un-confirm rule; the three contract kinds and the audiences; escrow; collateral. **Not** the regional book (it ends hauling as a job) and not the free-text title (the source of most scams) |
| **Albion Online** | Trade window and mail | None | A market per city, sell and buy orders, a setup fee and a market tax; nothing moves between cities except by being carried | Per-place orders: the reason hauling is a profession. The fee shape |
| **Star Citizen** | Item transfer between players in person | Hauling and delivery contracts, mostly issued by NPCs; player-to-player markets are limited and have changed between patches | Per-system shop prices | Little; its player economy is mostly direct. Confirms that carrying should be physical |
| **X4 Foundations** | -- | Missions from NPCs; no player contracts between players (single-player) | Station trade offers, buy and sell, at prices that follow stock | The stock-driven offer is what our NPC market already is |
| **Elite Dangerous** | No trade window by design; players move goods between them by dropping cargo in space | Passenger and cargo missions from NPCs, never from players | Commodity markets per station (and a carrier owner's own market) | Mostly a counter-example: no player exchange is why Elite's economy feels like one player against a menu |
| **Starsector** | -- | NPC missions | Local markets | Nothing for this |

Where EVE is the model and why we depart from it:
- EVE can run a regional order book because ISK is an abstraction the NPCs also mint (NPC
  orders sit in the same book, agent rewards pay ISK from nowhere). Ours is a closed economy
  with a stock-driven price; the NPC market is the backstop of both sides and stays the one
  thing prices cannot leave (section 2).
- EVE players distrust anything free-text. We have no free text in an exchange at all.

## 1. What is exchanged

One thing is exchanged: a **bundle**, the server's own description of what one side puts in.
It lists any of:

| Item | In the bundle as | Notes |
|---|---|---|
| Goods and modules | `{ good, amount }` | Modules are goods (#472 slice 10), so they need nothing new. Taken from a container *here*: the hold or the pilot's hangar here, or a corporation division the issuer may take from |
| Credits | `{ credits }` | From the pilot's money; or a corporation's treasury (`withdraw` rights) when a corporation issues |
| Charts and observations | `{ system, layers }` | A copy of what the pilot knows of a system (section 3). The seller keeps theirs |
| A ship | `{ ship id }` | A located ship (#472 slice 8) at this station: its fit, its hold and its design go with it, **its insurance does not** *(call: a hull cannot be insured, dumped for the payout, and traded on)* |
| A structure | `{ structure id }` | The #468 transfer: licence and what is owed on it, storage, rules of engagement go with it. Refused while it is under siege or being evicted, and the listing shows its damage, its rent owed and its licence state |

A deal has a **giving** bundle and an **asking** bundle (either may be empty: a gift is an
empty ask; a bounty on delivering something is a courier contract). What each side receives
lands in a container *here*: the hangar, or a division for a corporation. **Room is reserved
at posting**, the way the NPC delivery service reserves room at its destination, so a deal
cannot fail at the last moment for want of space, and an offline pilot can be paid.

*Not exchanged:* skills, standings, licences on their own, the right to name a system, the
discoverer's chronicle entry (K5 of the knowledge design). A bought chart gives the map, not
the credit for finding it.

## 2. Forms

Four forms, one object. A **deal** is the faction `Contract` generalised; the four forms are
its `kind`.

```
Deal {
  id, kind                 // direct | exchange | courier | order  (auction later)
  issuer                   // an owner string: "<account>", "corp:<id>"; "faction:<id>" is today's contract
  to                       // open | account | corp:<id>
  station, system          // where it is held and where it is done
  giving, asking           // bundles; escrow = the giving bundle, held at the station
  // courier:  destination, reward, collateral, deadline, carrier, taken
  // order:    one good, a unit price, partial fills
  postedAt, deadline
}
```

### Direct trade (kind `direct`)

Two pilots docked at the same station (or in the same system, in reach of each other, as the
loot verb has reach) open a trade. Each puts a bundle in; each sees the *server's* description
of both; each presses confirm; the server swaps.

- **Any change by either side un-confirms both**, and the confirm button waits a moment after
  a change. A confirm carries the revision of the bundles it saw, and the server refuses it if
  they have moved: the trade-window scam (swap the item after the other side looked) cannot
  happen even with a laggy or malicious client.
- It is **atomic and server-held**: nothing leaves anyone's container until both have
  confirmed, and then all of it moves in one step; a failed re-check (an item no longer there,
  a hold full) aborts the whole trade and says why.
- It is **live, not saved**: both online, both here; undocking or leaving the system cancels
  it. It needs no escrow (nothing is held between the confirms) and no board.
- It is the *same* `Deal` with kind `direct` and the same bundles, so the window that draws a
  bundle and the verb that checks one are shared with every other form.

### Contracts: item exchange and courier

**Item exchange (kind `exchange`).** The issuer posts a giving bundle and an asking bundle at a
station. The giving bundle is **taken from the issuer at posting and held** as a container
`(station, deal)` -- the escrow, "held by the station", with the lost-place rules of a hangar
(#472 slice 6). Anyone in the audience, **docked there**, with the asking bundle in a container
here, accepts: the server moves the ask to the issuer's reserved room and the escrow to the
acceptor in one step. Cancelling, or the deadline, returns the escrow.

Audiences: **open** (any pilot, listed on the station's board), **to a pilot** (listed for them
anywhere in their contracts list; done docked there), **to a corporation** (any member may
accept; what is received goes to the corporation's division or treasury, as a member's build
becomes the corporation's). A corporation may also *issue*: a director or CEO posts from a
division and the treasury, and the deal is the corporation's (`corp:<id>` as the owner string,
read through the corporation registry and `StructureParty`, never by comparing an account
name -- #468's lesson).

**Courier (kind `courier`).** The issuer's goods are held at the origin station; the issuer
sets a **reward** (escrowed from them) and, optionally, a **collateral** the courier must put
up; a destination station; and a deadline.
1. A courier accepts, docked at the origin. **Their collateral is taken from their money and
   held.** The goods are *released to the courier's hold* -- the courier physically carries
   them (hold volume matters, #472 slice 7), through whatever lies between.
2. Docked at the destination, the courier hands in: the goods go to the issuer's (or the named
   receiver's) reserved room, and the reward and the collateral come to the courier.
3. If the deadline passes, or the courier's ship is lost, **the collateral goes to the issuer**
   (and the reward returns to them). Lost with the ship, the goods drop in the wreck at the
   share they always do; that is what the collateral is for.

A courier contract can also move a **ship**: later, and only once a hull's *custody* is
separate from its *owner* (a field a located ship does not yet have); see the slices.

**What a contract costs.** A flat posting fee, and the market's own fee on the value that
changes hands, both to the station's owner or the system's holder through `FeeAt`, exactly as a
market trade pays it *(call)*. Without that, every trade would go through a contract to avoid
the fee, and the holder's tax (#431 T3) would mean nothing the moment two pilots talk.
Per-pilot cap on open deals (`world_rules.json` -> `exchange.maxOpen`), a maximum deadline of
fourteen world days, and a journal line for each party at each step (posted, taken, delivered,
failed), which reaches an offline pilot on login like an attack notice.

### Player orders beside the NPC market (kind `order`)

The market is a stock and a price curve, not an order book: `Bid` and `Ask` follow how short of
its target the stock is, every unit traded moves them, and the spread between them is the
market's living (#343). Two pilots trading *between* the bid and the ask both do better than
trading with the market: the seller gets more than the bid, the buyer pays less than the ask.
That is the same inequality a faction's delivery contract already uses (`ContractPrice` lies
strictly between selling here and buying here), and it is the whole of the design:

- A **sell order** is an exchange deal for one good: the seller moves goods into escrow at the
  station and names a **unit price**. A **buy order** escrows credits and names a unit price and
  an amount. Both allow **partial fills**. They are the same `Deal`, kind `order`, listed in the
  market window as a second block under the NPC's row, **not** a new store: the "book" for a
  good at a station is the open orders with that good at that station.
- An order is **matched only between players**: a pilot buying sees the cheapest player sell
  order beside the market's ask and takes whichever is cheaper; a buy order and a sell order
  that cross are filled at the earlier order's price. **The NPC market never trades with a
  player order.** Haulers, recipes, contracts and the price curve see none of it: goods sitting
  in an order are not market stock, so they neither lower its price nor satisfy a shortage.
- An order priced **outside the spread** (a sell above the market's ask, a buy below its bid)
  is accepted but will not fill while the market is better, since the better price is one click
  away. So **the NPC market bounds player prices on both sides without any rule**: nobody can
  corner and gouge a good, because the market's ask is always the buyer's alternative. A
  shortage is the one case where players can charge more than the market's *current* ask: when
  the market has no stock the ask sits at the ceiling and the order is the only supply -- which
  is the economy answering a shortage, not a break in it.
- **Nothing from nothing.** Credits and goods move from one party to the other. No skill
  multiplier and no reputation bonus applies to a player-to-player price (they scale what the
  *market* pays, which has no wallet; here they would print credits). The fee is the market's
  `FeeAt`, as for a trade. A conservation test (credits plus escrowed credits, goods plus
  escrowed goods, across any sequence of deals) is part of the first build.
- **Per station, never regional**, and only while docked (decision 1). The map's *observed*
  layer already carries "markets as last observed, with the date": a pilot sees a far station's
  orders as of their last visit, and an old picture is stale information, which is the point.

### Auctions (later)

A contract with a reserve and an end time; bids are escrowed credits, an outbid bid is
returned, the winner is the top bid when it ends, and an optional buy-out ends it. It is the
same escrow with a bid list on the deal. EVE's auctions are lightly used next to its market,
and our orders cover most of what an auction is for; so it is last, and only if the playtest
asks.

## 3. Charts

A chart is **knowledge copied to the buyer**; the seller keeps it. That is the one exchange
that creates a copy, and the one the owner worried about.

**What the buyer receives**, for each system sold, exactly what the seller has in their own
record of it and nothing more:
- the **geography** (place, links, star, bodies, name as the seller knows it) -- known for good,
  K6;
- the **observed layer** as the seller last saw it, **with the date** (stations and structures,
  the live owner and security as of that look) -- M1;
- the seller's resolved **scan finds** there (a roaming hole's end, a hidden base) -- the
  account's `scanned`.

It is written into the buyer's own `known` map under the same `system:<id>` key, the way a
visit would write it (the `MarkCharted` path, never `ChartBy`, so no first contact is created
by a sale). The **merge rule is the faction's rule**: a newer observation is never overwritten
by an older one, and what the buyer already knows is worth nothing. The buyer's data after the
sale is the better of the two layers, each with its own date.

**What a buyer is shown before they pay** is computed by the server *for that buyer*, so a
listing cannot lie and the knowledge stays each pilot's own (#431): "new to you: 3 systems, 4
stations, 1 find; the freshest observation is 20 hours old; one find is a hole closing in about
6 hours". Names and places stay hidden until accepted, because showing them is giving the
goods away; the shape of the bundle does not give them away. A system the buyer already has
newer shows as "known to you" and is **not charged for** (the server drops it from the price,
as `ChartOfferFor` does for a faction). A roaming hole that is about to close is shown with its
remaining life, so a find is never sold dead without the buyer being able to see that.

**Proof of genuineness is that the server wrote it.** A chart is not a document a player can
author; the seller can offer only what is in their `known`, re-checked when the deal is accepted,
and the listing is the server's description of that record. There is nothing to forge. What can
still go wrong is not forgery but *value*: a chart that is old, common, or about to be
worthless; all of those are on the listing.

**Resale, and what it does to the price.** The seller keeps the chart and can sell it again, to
anyone, any number of times, at no cost to themselves. So the price of a *common* chart goes to
nothing, and that is correct: it is information everyone has. What holds a price is exactly what
the faction rule already values -- **fresh, rare, and a find** -- and the observed layer decays
(`halfLifeHours`), so a chart's value is mostly in its first days. A seller wanting exclusivity
has no mechanism to enforce one; they sell once, to someone they trust, and it is a bet, as it
is in EVE's wormhole-intel trade. Two things make it fairer without being a rule against
copying:
- **Provenance.** The listing shows how many times the seller has sold this system's chart
  (never to whom), and the buyer's copy remembers where it came from and when: "bought from
  Kira, 3 days ago". A buyer pays less for a chart that has been sold ten times, as they would
  anywhere.
- **A bought chart is second-hand and does not sell to a faction.** `SellCharts` takes "only
  the pilot's own record", a system they went to themselves; a copy bought from a pilot is not
  that, so buying a chart and selling it to a faction cannot be an arbitrage on the faction's
  treasury. A bought chart *may* be resold to a pilot, marked second-hand with its chain
  (decision 3).

A faction's buying stays as it is: it is a *different buyer* with a price rule and a treasury,
and the chart verb of slice 16 is the sale to it. A sale to a pilot is the `chart` item of a
bundle, in a direct trade or an exchange contract, with the pilot's price.

**How it fits knowledge keys.** There is no new store. The seller's chart is a `system:<id>`
entry (and the `scanned` finds); the buyer gets the same. A corporation's charts
(`Corps::Corp::charts`) may be given or sold *to* a corporation, and a member may sell only
their own. The knowledge key (`KnowledgeKey`) changes for the buyer and their per-session
index is rebuilt from it, as it is when they visit, so no per-tick work is added.

## 4. Trust and fraud

**What the server guarantees** (the part EVE needs a rulebook for, and we have by
construction):
- **Escrow.** What a pilot puts into a deal leaves their containers and sits in the deal until
  it completes or is returned; it cannot be spent, sold or moved meanwhile, and it is saved with
  the world, so a restart loses nothing.
- **Atomic swaps.** A deal completes entirely or not at all; nothing is created or destroyed,
  checked by a conservation test.
- **The server describes everything.** Every item, its fit, a ship's hold, a structure's debts,
  a chart's age: the player never types a description. There is **no free-text title or message
  on a deal** *(call: the title is where EVE's impersonation and bait scams live; chat is where
  people talk)*.
- **Re-validation on accept.** The giver still has what they offered, the receiver has room,
  the station still stands, the structure is not now under siege.
- **A revision on every confirm** (direct trade), so what you confirm is what you saw.
- **Collateral** is taken from the courier at acceptance and held, never promised.
- **A ledger.** Every completed deal is a journal line for both parties, and a line in the
  server log for admins (who gave what to whom, where, when).

**Scams from EVE, and what we do:**

| Scam | Answer |
|---|---|
| Switch the item in the trade window after the other side looked | **Designed out**: un-confirm on change, revision on confirm |
| Contract titled like something else; "free ship" bait; impersonating a trusted name | **Designed out**: no free text; the server's description only |
| Fitted ship with cheap modules sold as a fitted ship; items inside a ship not looked at | **Designed out**: a ship's listing shows its fit and its hold in full |
| Selling a structure with unpaid rent, damaged, or about to be sieged | **Designed out**: debts, damage and state are on the listing, and a besieged structure is refused |
| Selling an insured hull | **Designed out**: insurance does not transfer |
| Price-typo scam: an item-exchange priced a hundred times the market | **Softened, not forbidden**: the accept dialog warns when a price is far from the market's mid |
| Courier takes the goods and runs; collateral smaller than the goods | **Left in, deliberately**, but made visible (decision 5): a courier contract shows the goods' value at the destination's market and what share the collateral covers. A pilot who takes a contract with 10 % cover is running a bet, and so is the issuer who posts one. The server will not choose the cover for them |
| The courier is killed by pirates, or the issuer is a pirate who posts bait and waits at the gate | **Left in**: it is the game, with the crime rules of the law design deciding what the killer owes. Collateral makes the courier's risk a price |
| Market manipulation: buy out a good and relist it high | **Bounded, not forbidden**: the NPC market's ask is always the buyer's alternative; a corner can only profit in a real shortage |
| Alt accounts, wash trading, funding an alt | **Left in**: money is conserved, so it harms no one; the fee is paid on every pass; the ledger lets an admin see it |
| Real-money trading, account sales | **Out of scope** of the design; the ledger and the allowlist are the tools |
| Scam in chat: "trade me first", "send me money to double it" | **Cannot be designed out**; a direct trade is the safe path, and an agent has no such urge at all |
| Contract spam, board flooding | **Designed out**: a posting fee, a cap on open deals, a fortnight at most |
| Selling a chart that is old, common or dying | **Shown**, not forbidden (section 3) |
| Ransom: holding a courier's goods or a structure to ransom | **Left in**; it is player behaviour and needs no support or prevention |

The general rule: **remove what the server can make impossible; show what it cannot; leave in
what is play.** A scam that needs the server to lie or lose a swap is removed; one that needs a
player to trust another player is the MMO.

## 5. Courier contracts and the NPC delivery service

The two have a clean relation already fixed by #472: *the NPC service is always there, always
slower, and never cheap; players are free to beat it on price and speed, and earn the gap.*
Concretely:

- The quote is a function of **route and cargo only** (volume, jumps, the security along the
  way, value for the insurance part). It never responds to what players charge, so players
  cannot undercut it into a race to the bottom, and it cannot be starved by them. It is the
  **ceiling** of the courier market: an issuer paying more than the quote for a route the
  service will take is wasting money, so the contract editor shows the quote and warns above it.
- The courier's competition is each other, below the quote, and the NPC service's *delay*
  (two hours a jump, at least an hour) is what a player courier sells: speed.
- Where the service **refuses a route** (a system under siege, held by a faction at war with the
  carrier), no quote exists and a courier contract has no ceiling. That is where a player
  courier's trade is most worth doing, and the risk is theirs and priced by collateral.
- **One carrier model.** `Shipment` (goods leave a container, arrive in a container when the
  clock says, a fee paid to a holder) generalises to `{ from, to, goods, carrier }`, the carrier
  being the faction's freight office or a courier's hold. A courier contract is the same
  shipment with a different carrier and a different failure rule (collateral, not insurance). A
  contract unfilled at its deadline offers its issuer a button: **send it by the service
  instead**, at the quote.
- **The NPC quote is shown beside every courier contract**, when it is posted and when it is
  browsed (#472 section 5).
- Raiding raises both: the service's risk term rises with piracy on the route, and the
  contract's reward and collateral rise with it. Raiders make couriers' work worth more, which
  is the economy answering.

## 6. Agents

An agent is a player (#42); its verbs are the same, and what it reads is what the window shows.
It reads the server's description of a deal, never a title, so it cannot be baited by text.
Following `corp_act` and `move_goods`, the tools are few and take an `action`:

| Tool | What it does |
|---|---|
| `deals` | Read: the open deals here (the board, orders for a good), the pilot's own (posted, taken, to them, delivered), with every bundle in full, the NPC quote on courier ones and the chart diff "new to you" |
| `deal_act` | `post` (kind, giving, asking, to, deadline; courier: destination, reward, collateral), `accept`, `cancel`, `deliver` (a courier's hand-in), `offer` / `put` / `confirm` / `withdraw` for a direct trade with a named pilot |
| `orders` | Read: the player orders for a good at this station beside the market's bid and ask |
| `order_act` | `sell` / `buy` (good, amount, unit price), `cancel`, and a taking verb that fills against the cheapest player order |
| `sell_charts` (exists) | Sells to a faction. Charts to a pilot are a bundle item in `deal_act` |

`observe` and `wait_for_event` carry what a player sees in the journal: a deal posted for the
agent, taken, delivered or failed, so an agent can run a trade desk or a courier route on the
`wait_for_event` loop it already uses. The server rate-limits deal actions as it does chat, so a
hostile agent cannot flood a board. `trade_run` and `contract_work` prompts gain a paragraph; no
new prompt.

## 7. Extending what exists

| Form | Carried by | Why nothing existing could carry it as it is | What is new |
|---|---|---|---|
| Exchange, courier, order | `Contract` (generalised: `issuer` becomes an owner string; `kind`; `giving`/`asking` bundles; `to`; escrow container) and `contracts_` in world.json | A faction contract has one good and a treasury as its payer; the record, the board, the deadline, the set-aside reward and `EndContract` are the same machinery | Fields on `Contract`; a section of `SaveContracts`; `StepContracts` handles the new kinds |
| Escrow | The container model of #472: `(station, deal)` is a container whose holder is the deal | A hangar is (station, pilot); escrow is a hangar whose holder is a deal and whose rules of loss are a hangar's | A holder kind |
| Courier carriage | `Shipment` / `Delivery` | The service's shipment is the same movement with a faction as carrier | A `carrier` field |
| Direct trade | A `Deal` of kind `direct`, **not saved**, in the two `ClientSession`s | No existing thing is live between two sessions; contracts need a board and escrow, which two pilots standing side by side do not | A session-level pending deal. This is the one place a new transient state is needed, and it is not a store |
| Player orders | The same `Deal`, filtered by good and station, shown in the market window | The market has stock and a curve, no counterparties | A block under the NPC row in the market window, and a matching rule |
| Fees | `FeeAt` | -- | Nothing |
| Charts | `known`, `scanned`, `MarkCharted`, `ChartOfferFor`'s diff | Knowledge is already per pilot and already priced | A `chart` bundle item and a "second-hand" provenance field |
| Ships, structures | #472 slice 8 ship instances, #468 `transfer` | Both are already things with an id and a place | A bundle item that calls them; `custody` on a ship for courier (later) |
| Corporations | `corp:<id>`, `Corps::Registry`, divisions and their rights | -- | Nothing |
| Notices | The journal, the corporation journal | -- | Line kinds |
| Windows | A **Trade window** and a **Contracts tab** of the station window, a **block** in the market window, a **deal** list in the inventory window's assets tab; all built from the existing desk, tables and drag and drop | A bundle is two lists and drag-and-drop is built | Two windows' worth of layout; no framework change |
| Agents | `deals`, `deal_act`, `orders`, `order_act` | A new verb is a `Command` | The four tools; a `Command` field `deal` (one verb with an action), as `CorpAct` has |

Versions: `PROTO_VERSION` bump (a new command and a bundle on the wire); `WORLD_VERSION` bump for
the `Contract` section (an older reader would drop a player deal's escrow, which is the
dangerous reading, so it must refuse); `ACCOUNT_VERSION`: none (escrow is in the deal, not the
account). Data: `world_rules.json` -> `exchange` (posting fee, max open deals, max days, the
price-warning ratio).

## 8. Slices

Each is one PR, shippable alone, smallest useful first. Slices 1-3 do not need the later
holds work.

1. **Direct trade, of goods, modules, credits and charts.** The `Deal` bundle, the revision
   rule, the Trade window and `deal_act` for a direct trade. No saves and no board. A chart
   sale here answers the owner's origin question first: two pilots, a chart, a price, with the
   buyer's "new to you" shown. *PROTO bump. Agent: `deal_act`.* The conservation test is in
   this slice.
2. **Exchange contracts.** `Contract` generalised, escrow containers, the board in the station
   window, audiences (open, a pilot, a corporation), reserved room, fee and cap, journal lines,
   the saved section. *PROTO and WORLD bumps. Agent: `deals`, `deal_act post/accept/cancel`.*
3. **Corporation issuing and receiving**, the division rights on the giving side, a corporation
   as `to`. *Small; may merge into 2.*
4. **Courier contracts**, with collateral, the NPC quote beside them, the shared shipment model
   and "send it by the service instead". *WORLD: the courier's state in the deal. Agent:
   `deal_act deliver`.*
5. **Ships and structures in bundles.** Needs #472 slice 8 and #468 (both built). Listing
   rules, insurance not transferring, a structure's debts shown, refusals under siege. *Agent:
   covered by `deal_act`.*
6. **Player orders** in the market window, partial fills and crossing, with the "NPC market
   never takes them" rule and a macrobench to show the economy is untouched. *Agent: `orders`,
   `order_act`.*
7. **Chart provenance and second-hand rules**, if not already in slice 1. A small slice of
   fields and a listing line.
8. **Ship couriers** (custody separate from ownership). Last, because it changes what a ship is.
9. **Auctions**, only if the playtest asks.

## Decisions for the owner

The forks that change the game's feel. Each is a recommendation, not a decision made.

**1. Is exchange local to a station, or is there a regional market?**
- *A. Local, dock-only* (orders and contracts are placed, taken and filled where you are docked;
  a far station's board is seen as last observed, with the date). **Recommended.**
- *B. Regional*: orders and contracts searchable and placeable across a region or a range of
  jumps, EVE's way.
- *C. Local to place, but a pilot may browse (not take) every board they know of.*
- EVE: regional book, remote orders within a range of jumps, which is why hauling for a margin
  is a smaller job there. Albion: a market per city, nothing between cities except carried
  goods, which is the strongest reason for a hauling profession. Elite: none.
- *Why A:* it is the premise "no remote trading" (#472) applied once more, it keeps the NPC
  delivery quote and courier contracts meaningful (a regional book would let a buyer pay no one
  to move anything), and the observed layer already gives the "stale picture" a price. C is
  the cheapest compromise if A feels blind; it should be played before B is considered.

**2. How do player orders meet the NPC market?**
- *A. Beside it, players only*: the NPC market never trades with an order, and an order priced
  outside the bid-ask spread simply does not fill. The market's own price is untouched.
  **Recommended.**
- *B. In the same book*: the NPC market is an order too, orders count as stock, and a player's
  sell order lowers the price and supplies a shortage. EVE's way.
- *C. No player orders at all*: contracts and direct trade only.
- EVE: one book, NPCs and players alike, because NPC rewards mint ISK. Albion: a book per city,
  NPCs are not in it, which is A. X4: offers only. 
- *Why A:* B makes goods sitting in an order part of the living economy: haulers, recipes,
  shortages and contracts would all read them, and so would every balance already tuned
  (#343, #407). A leaves them alone and still gives players a better price than either side
  gets from the market. C is the safest and the least alive; A can be dropped later at the
  cost of slice 6 only.

**3. A chart is a copy: what, if anything, protects the seller or the price?**
- *A. Copy, provenance shown, a bought chart not sellable to a faction but resellable to a
  pilot as second-hand.* **Recommended.**
- *B. Copy, no provenance, no limit* (EVE's information trade: all trust).
- *C. A sale can be exclusive*: the seller gives up their observed layer for that system (the
  geography stays, K6), so the buyer holds the only fresh picture, at a higher price.
- *D. A bought chart cannot be resold at all.*
- EVE: no chart item; intel is sold on trust, and the sellers who last are the trusted ones.
  Elite: exploration data sells to the one faction, never to a player. 
- *Why A:* a price is held by freshness and rarity, which the faction rule already values and
  decay already erodes; provenance lets buyers price what they cannot verify; barring the sale
  to a faction removes the one arbitrage on a treasury. C is a real alternative that makes
  exclusivity tradeable at the cost of a seller losing their own information; I would add it
  only if the playtest shows the price of a fresh chart collapsing.

**4. Is player trade taxed, and do skills help?**
- *A. The same fee as a market trade* (`FeeAt`: the owner's or the system's holder's), a small
  flat posting fee, and **no skill or reputation multiplier on a player-to-player price**.
  **Recommended.**
- *B. Free*: a trade outside the market costs nothing.
- *C. Like EVE*: a fee when the order is placed (broker) and a tax when it fills, both sinks.
- *Why A:* the fee is how the holder's tax (#431 T3) and an outpost market's income mean
  anything; with B every pilot with a friend trades outside it; skills scale what the market
  pays and the market has no wallet, whereas here they would print credits. C's two fees are
  a sink we do not have a use for: our fee is already to a treasury, which spends it.

**5. Does the server set a floor on a courier's collateral?**
- *A. Free: the issuer sets it, the server shows the share of the goods' value it covers.*
  **Recommended.**
- *B. A floor at the goods' value* at the destination's market.
- *C. No collateral*: reward only.
- EVE: free, with the collateral scam as the result; the trade is lively and the scam is
  well known and avoidable by looking at the number. Star Citizen's and Elite's NPC hauling
  have no collateral at all.
- *Why A:* B would make a courier of valuable goods require the whole value in capital, which
  closes the trade to a newcomer, and it is the server deciding how risky a bet a player may
  make. A turns the scam into a visible number. If the playtest shows ruinous losses the floor
  can be added as a rule value later (`exchange.minCover`) without changing anything else.

**6. Can structures and ships be *sold*, and what stops a territory from being bought?**
- *A. Yes, with the rules of their own transfer* (#468): a structure's licence is checked for
  the new owner, a besieged one is refused, a ship's insurance lapses. A structure's sale can
  change who holds a system by influence, which is allowed: it is the game. **Recommended.**
- *B. Gift or corporation-internal only*: no credits for structures.
- *C. Ships yes, structures no.*
- EVE: structures and ships are sold by contract; sovereignty structures are restricted.
- *Why A:* a player-mutable world (#44) needs a property market, and the held-system rules
  (holder licence, front line, contested claims) already limit what a purchase can do. B or C
  are the cautious start; slice 5 can ship with C and add structures after a playtest.

Smaller calls made in the text, easy to change: no free text on a deal (section 4); insurance
does not transfer; a fortnight at most; auctions last; ship couriers last.
