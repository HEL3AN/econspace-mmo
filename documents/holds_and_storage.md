# Holds, hangars and storage

Status: **decided, 2026-10-23.** The owner answered the draft's questions on #472; the answers
are folded into each section below and listed under [Decided](#decided). Slices 1-4 are being
built; the rest is the plan. Calls made where the decisions were silent are marked *(call)* and
are logged on #186 for review.
Related: #473 (player-to-player exchange and courier contracts), #468 (structure ownership),
#344 (ship loss and insurance, built), #279 and #150 (designs and their costs), #438 (outpost
storage, built), #464 (NPC fits from the living world), #297 (the window system).

## Why

Goods today live in two places: the hold of the ship you fly and the storage of a player
structure. There is nowhere to keep anything at an NPC station, so a pilot without an outpost
cannot buy low and wait, cannot stage goods for a haul, and cannot own more than one hold's
worth of anything. Every owned hull is usable at every station, so a ship has no location and
the map has no logistics for ships. Modules are not things; a refit is a payment. The interface
is a "Store all / Take all" button per good.

The economy is the reason this matters. Markets are local, prices move with stock, NPC haulers
and convoys carry goods between them (#343), and shortages are real (#407). A storage model
decides whether moving things is a profession, and whether a market can run short because
someone took the goods somewhere else. It also decides what a fight costs, because whatever is
stored somewhere can be lost there.

## Today (main, b951726)

**The hold.**
- `Ship::cargo_` is `std::map<ResourceType, int>` (`src/game/entities/Ship.h:225`). Capacity is
  the sum of the design's `provides.cargo` (`src/engine/core/ShipDesign.cpp:416`); in
  `data/ships.json` that comes from mid sections (`mid.slim` 5 to `mid.hopper` 60) and from
  `hull.cargo` modules (30 each). The stock designs hold roughly 35 (Courier) to 200 (Hauler).
- One unit of any good takes one unit of room. No volume, no mass. Only the ten goods in
  `data/goods.json` exist as things; modules are not items.
- The hold belongs to the pilot, not to a hull: it is saved in the account (`"cargo"`,
  `Simulation_Player.cpp:1786`), and switching ships keeps it. `SwitchShip` refuses when the
  cargo does not fit the other hull (`Simulation_Player.cpp:931`, `HoldFits`).

**Storage.**
- Only in player structures (#438): `Structure::stored_` (`src/engine/entities/Structure.h:150`),
  saved with the structure (`Simulation_World.cpp:793`). Capacity comes from the archetype's
  `storage` component (`Archetype.h:156`): 1000 for the outposts, 2000 for the outpost market.
- Access is the owner and their corporation, all or nothing (`Simulation::DockedStorage`,
  `Simulation_Build.cpp:402`, via `Structure::OwnersSide`).
- The four NPC station archetypes also declare `storage` (trade hub 5000, shipyard 4000, mining
  outpost and military 3000, `data/archetypes.json:254-365`), but `DockedStorage` only looks for
  a `Structure`, so nothing reads it on a `Station`.

**Moving goods.** All as fields on `Proto::Command` (`src/game/sim/Protocol.h:197`): `buyType` /
`sellType` (market, hold only), `storeType` / `takeType` (structure storage, hold only),
`lootId` (a derelict into the hold), mining into the hold, `deploy` and `dismantleId` (build
costs from the hold, refunds into it), mission and contract hand-ins from the hold. A lost ship
leaves a wreck with 25 % of each good (`Simulation_Player.cpp:174`); a `Derelict` holds one
good, so every further good becomes a separate "Cargo of" pod beside it.

**Ships.** `ClientSession::ownedShips` is a list of catalogue indices (`ClientSession.h:45`), so
an account owns at most one of each hull (`BuyShip` refuses a second, `Simulation_Player.cpp:1052`).
No hull has a location. Designs and insurance are keyed by the same index (`shipDesigns`,
`insurance`). On loss the hull leaves the list and the starter comes back (`Simulation_Player.cpp:217`).

**Interface.** The station window shows a STORAGE block only at a structure (`DrawStorage`,
`src/game/core/GameStation.cpp:94`) with "Store N" and "Take N" for the whole stack. The market
window (`GameStation.cpp:268`) has a slider for partial amounts, but only between market and
hold. The hangar window lists hulls and fits. The window framework (#297, `src/engine/ui`) has
tables, tabs, sliders and scroll areas; the only drag it knows is a window's title bar and
corner (`Desk.h`), so there is no item drag and drop.

**Agents.** `store_cargo`, `take_cargo`, `sell_cargo`, `buy`, `salvage`, `switch_ship`,
`buy_ship`, `insure_ship`, `hangar`, `refit` among the 44 tools in `src/agent/main.cpp`.

**Corporations.** Roles are member, director and CEO (`Corporation.h:31`); verbs go through
`Proto::CorpAct`. #468 decided that a member's structure belongs to the corporation, and that
stored goods stay with a structure when it is transferred.

## How others do it

| Game | Hold | Storage | Ships and modules | Loss | What we take |
|---|---|---|---|---|---|
| **EVE Online** | Volume in m³ per item; specialised holds (ore, fuel, ammo, fleet hangar); containers | A free, unlimited personal item hangar at every station; corporation hangars with 7 divisions and role rights; jettisoned cans in space | Ships and modules are items with a location; a ship is where you left it; contracts move them | Fitted modules drop at about 50 %; cargo likewise; structures' contents go to asset safety (5 days wait, a fee of about 15 %), except in wormhole space and for abandoned structures, where they drop as loot | Location for everything, a hangar per station, divisions with roles, jettison, the assets view, asset safety as a rule for one case |
| **Star Citizen** | SCU, physical boxes on a grid | Local inventory per location, reached through a freight elevator in the hangar | Ships stored at a home location; item recovery | Physical cargo can be stolen box by box | Local inventory per place. Not the physical box handling: busywork in a 2D game |
| **Elite Dangerous** | Tons, one rack type | No commodity storage at stations (until fleet carriers); modules and ships can be stored | Ship and module transfer for a fee scaled by value and distance, with a delay | Rebuy (insurance) of the ship; cargo lost | A paid transfer with a delay as an alternative to flying a ship over yourself |
| **X4 Foundations** | Per-ware volume; three storage types (solid, liquid, container) that decide which ship can carry what | Station storage modules per type | -- | -- | Volume per good. Not the three types: they are a design constraint that our parts already express |
| **Starsector** | Cargo, fuel and crew as separate capacities | Storage at a market: a one-time fee of 5000 credits, then 1 % of the stored value per month; free at your own colony | Ships can be stored too | -- | Storage as a service sold by the market, paid over time, free at your own place |
| **Albion Online** | Weight | A bank per city; nothing moves between cities except by being carried | -- | Full loot in dangerous zones | Local banks make regional prices and hauling real. The strongest argument for "no teleport" |

Notes:
- **What players hate.** EVE's unified inventory (Inferno, 2012) was attacked for lost window
  layouts, a broken "loot all" and slowness with big corporate hangars, not for the model
  itself; it survived. Elite's transfer delays and fees are a long-running complaint because
  the galaxy is huge and nothing else is to be done while waiting. Star Citizen's box handling
  is praised as immersion and resented as chores, and its item banks were slow with many items.
  Starsector's storage fee is mostly accepted because your own colony is free.
- **What matters for an MMO economy.** Albion and EVE both get a hauling profession out of one
  rule: goods exist in one place and have to be carried. Volume turns that into choices (bulk
  ore needs a hauler, circuits fit a courier). Contracts (#473) let one player pay another to
  carry. Losses where storage is attacked give sieges a point.

## Decided

The owner's answers on #472, 2026-10-23, by the draft's question numbers:

| # | Question | Decision |
|---|---|---|
| 1 | Located ships | **Yes.** A ship stays where it was left and has its own hold; several hulls of one type may be owned. |
| 2 | A personal hangar at NPC stations | **Yes. 100 units free; beyond that the pilot rents capacity**, choosing how many units (not tiers), paid periodically, about monthly, to the station's faction. Designed in [Rent](#rent). |
| 3 | Volume per good | **Yes**, with the proposed tiers. |
| 4 | A destroyed player structure | **Part of its storage drops into the wreck.** |
| 5 | NPC stations lost | Destroyed: **as drafted** (moved home after a day for a fee). Turned hostile to the pilot: **a paid delivery of the hangar to another storage**, designed in [Delivery out of a hostile station](#delivery-out-of-a-hostile-station). |
| 6 | Modules as goods | **Yes.** |
| 7 | Delivering ships and goods | **Player courier contracts (#473) and an NPC delivery service**, with economics that keep both alive. Designed in [NPC delivery and player couriers](#npc-delivery-and-player-couriers). |
| 8 | Drag and drop | **In the first version, as a general mechanic of the UI framework (#297)**, usable by any window. Designed in [Drag and drop](#drag-and-drop). |
| 9 | Cargo mass affecting handling | **Not now.** |

## The model

**A container is (place, holder, division)**: a ship's hold, a pilot's hangar at a station or a
structure, a structure's storage, a corporation division, or a can floating in space. Every one
of them is the same thing underneath -- goods by type with a capacity -- and one verb, `move`,
moves goods from one to another *at the same place*. Structure storage (#438) is not replaced
by a parallel store: it becomes the first case of the model, the one whose holder is the
structure's owner side. That is the integrate-not-bolt-on answer to "why a new store".

The containers reachable from where the pilot is:

| Where | Containers |
|---|---|
| In space | the hold; cans and wrecks within loot reach (take only, and put into a can one jettisoned) |
| Docked at an NPC station | the hold; the pilot's hangar here; later, the pilot's other ships here |
| Docked at a player structure | the hold; its storage, if the pilot's side's (#438); later, the pilot's hangar here and the corporation's divisions they may see |

Nothing moves between places except in a hold, or by a paid delivery (below). There is no
remote trading: you trade where you are docked.

## Sections

### 1. A personal hangar at NPC stations

Every station whose archetype has a `storage` component offers each pilot a personal hangar.
**100 units are free** for every pilot at every such station. Beyond that the pilot **rents**
capacity, any number of units they choose, up to the archetype's `storage` number, which becomes
the cap per pilot (trade hub 5000, shipyard 4000, mining outpost and military 3000). The money
goes to the station's faction's treasury, so storage is income the living world can see and
spend. Only the pilot sees their hangar. At one's own structure, and a corporation's (see 7),
storage is free, as Starsector's colony is.

#### Rent

- **A rent is a number of units and a date.** The pilot sets the units rented at a station
  (`rent_hangar units=N`, or the field in the inventory window); the hangar's capacity is then
  `100 + N`. The first period is paid in advance on the spot.
- **The period is 30 days of world time** *(call)*, and every rent at every station falls due
  on its own date, one period after it was taken. **World time, not real time,** because
  (1) every other periodic charge -- licences (#431 slice 6), police budgets -- already runs on
  the world clock, and a second clock is a second set of bugs; (2) the world clock stops while
  the server is down, and a pilot should not pay for a month the world did not exist; (3) a test
  can drive it. The world clock runs at the speed of real time while the server is up, so to a
  player it reads as "monthly".
- **The price is per unit and per period, not per value** *(call)*: `rentPerUnit` credits per
  unit for 30 days, 2 by default, in `data/world_rules.json` under `hangars`. The owner asked
  for units chosen, so the price is of the room, as a warehouse's is; a value-based price would
  make a pilot pay more for the same shelf because the market moved. When volume lands (slice
  7) units become volume units and the default is divided by ten.
- **Changing the rent mid-period.** Raising it charges the added units for what is left of the
  period (pro rata), and the due date stays. Lowering it takes effect at once with no refund,
  and never below what the hangar holds above the free 100.
- **Falling due.** The macro pass takes `units × rentPerUnit` from the pilot's money, through
  the same path a licence takes (`Simulation::PayRent`), online or away (an away pilot's money
  is taken from what they had and is settled when they log in). The journal says so.
- **Unpaid.** If the money is not there, nothing is thrown away and nothing is sold. The rent
  shrinks to what the hangar holds above the free allowance, the unpaid period becomes a debt
  on that hangar, and **the hangar is closed** -- nothing in, nothing out -- until the debt is
  paid. Each further period adds the rent of what is held above the free allowance to the
  debt. Docking there pays the debt from the wallet if it can, and opens the hangar again; the
  journal says each step. A hangar with only the free 100 in it never owes anything. This is
  a lien: the station keeps what it is owed in goods, which is what a warehouse does, and a
  pilot who stops paying cannot keep a thousand units for free.
- **The faction changes, or the station becomes hostile.** The rent is owed to whoever owns the
  station when it falls due. Hostility is section 8.

### 2. Volume per good

**Decided: yes, as integers**, with the tiers: Iron, Ice 10; Crystal, Food, Steel, Fuel 5;
Ammunition, Repair Paste 2; Circuits, Medicine 1. Every `provides.cargo` and `storage.capacity`
is multiplied by 10, so ore loads are unchanged and refined goods take less room. Iron is then
1 credit per volume and Circuits 130. Integers keep the wire and the saves exact. NPC haulers
(`haulCargo`) count volume too, so the macro sim follows the same rule. **No mass**: it would
touch `StepPlayerShip`, which both client and server compute (decision 9).

### 3. Specialised holds

**One general hold now, an ore hold later as data**: a section or module
`provides: { "ore": N }`, and that capacity takes only goods with `"mined": true`. `mid.hopper`
becomes an ore hold. The interface shows it as a second bar in the same container. A fuel bay
and an ammo hold wait for a fuel or ammunition mechanic. A late, optional slice.

### 4. Ships and modules as located items

**Ships (decided: yes).**
- An owned ship becomes an instance: `{ id, hull, design, station, hold, insurance }`. An account
  may own several of the same hull.
- **Each ship keeps its own hold.** Switching ships leaves the cargo in the ship you got out of;
  the `HoldFits` refusal disappears. This is how a hauler is parked full and a fighter flown
  out.
- You can switch only to a ship docked where you are. The starter is issued at your home on loss
  (#344 already wakes you there).
- A ship is moved by flying it, by a player courier (#473), or by the NPC delivery service
  (below), whose cargo it then is.

**Modules (decided: yes, as goods).** A module is a good of kind `module`, generated from
`ships.json` modules, with a volume from its mass. Shipyards produce them from their `cost`
through the existing recipe machinery, so they have a stock and a price that moves. A refit
stays one click: it takes modules from the hangar (then the hold) and buys the rest at the
shipyard's market price, so the "fitting as payment" path is still there for someone who owns
nothing. Removed modules go to the hangar. A fitted module that drops from a wreck feeds a
salvage and second-hand market, and #464 gets NPC fits that come from what their faction's
shipyards actually built. Not first: it widens `ResourceType` and every per-good vector on the
wire.

### 5. Moving goods

- **Between containers at the same place:** one verb, `move`, from one container to another,
  any amount up to what fits. `store` and `take` become two cases of it.
- **To and from the market:** sell from and buy into either the hold or the hangar here. A pilot
  can buy more than one hold, which is what staging a haul is. **No remote trading.**
- **Between places:** goods move in a hold, or by a paid delivery -- a player courier (#473) or
  the NPC service (below). Nothing teleports for free.
- **Between pilots:** #473 (direct trade, contracts), and the can.
- **Jettison: into a can** (a `Derelict` with cargo, as a wreck pod is today), visible to
  everyone, lasting `SHIP_WRECK_SECONDS`; scooped back with the existing loot verb or `move`.
  Two plays: dumping to escape with a lighter hold, and the in-space hand-over between two
  pilots without any new trade mechanics. A `Derelict` holds several goods at once; a lost ship
  leaves one wreck instead of a row of pods. A jettison into a can that is already there (one's
  own, within reach) adds to it.

### 6. One inventory window

**One Inventory window**, a desk window like the market (#297), open docked and in space, with
**drag and drop from the first version** (decision 8).
- **Left:** the containers reachable here, each with a used / capacity bar: the hold of the ship
  being flown; then, docked: your hangar here, and at a structure its storage (later: your other
  ships here and the corporation divisions you may see). In space: the hold, and cans within
  loot reach.
- **Right:** a table of the selected container: good, amount, local price, value. A filter field.
  Sorting by column.
- **Moving:** select a row, set an amount (a number field plus 1 / 10 / half / all), then a
  "Move to" button per other container listed. **Or drag the row** onto another container in
  the left list: the drag carries the amount set (Shift at the start of the drag carries the
  whole stack). The same buttons are what a keyboard and an agent use.
- **Into other windows:** a good dragged from the inventory onto the market window selects it
  there with the dragged amount ready to sell; selling stays a click in the market, so there is
  one market screen and no sale by accident. Dropped on the world in space, it is jettisoned
  (slice 4).
- The HUD's "Cargo x/y" and the `I` key open the window. The station window's STORAGE block and
  its store / take buttons go.

#### Drag and drop

A mechanic of the window framework (`src/engine/ui/Drag.h`), not of the inventory window, so
the market, the hangar (modules, later ships), the fitting screen, the overview (a target onto
the selected-item window) and the corporation window can each be a source or a target without
inventing their own.

- **The payload** is a small value, `Ui::DragPayload`: a `kind` saying what is carried (`"goods"`
  for now; `"module"`, `"ship"`, `"entity"` later), the `source` as the source names it
  (`"hold"`, `"hangar"`, `"storage"`, `"can:812"`), the `item` within the kind (a good's index),
  the `amount` carried and how many were `available`, and a `label` for the cursor. A value, not a
  pointer: the source window may not be drawn on the frame of the drop.
- **Starting a drag.** A source declares itself where it is drawn: `Layout::DragSource(id, index,
  payload)` for any element, or a table's `drag` callback for its rows. A press on it *arms* the
  drag; it starts once the cursor has moved a few pixels with the button held, so a click is
  still a click (selecting a row) and only a deliberate pull drags.
- **While dragging,** the mouse belongs to whatever is under the cursor rather than to the window
  pressed in (`DeskLayout::UpdateOwner`'s capture is lifted for an item drag), so a target can
  light up under the cursor. A target asks `Layout::DropHover(id, index, kind)` to draw itself as
  a place to drop, and the payload's label follows the cursor, over everything, drawn last.
- **Dropping.** On release the drop is offered for that frame only. The first target whose box is
  under the cursor, whose window owns the mouse and which accepts the kind takes it
  (`Layout::Dropped(id, index, kind)`); a window behind another never sees it. Released over no
  window, the game may take it as a drop on the world (`Ui::Drag::DroppedOnWorld`). Released
  where nothing takes it, nothing happens. Esc cancels a drag.
- **A partial amount** travels in the payload: the amount field of the source, or everything with
  Shift. The target clamps it to what fits and the server clamps again; what was actually moved
  comes back in the journal, as every move does.
- **What it is not:** a drop never acts on its own where a mistake is expensive. Moving goods
  between one's own containers is undoable by moving them back, so it happens on drop; selling
  is not, so a drop on the market only prepares the sale.

### 7. Corporation hangars and rights

**A few divisions with a minimum role per right, and a log.** The storage of a corporation's
structure (after #468) has three divisions the CEO or a director can rename: *Shared* (members
put and take), *Stock* (members put, directors take), *Command* (directors only). Each division
states the lowest role that may view, put and take; changing that is a `CorpAct` verb
(`division`). Every take from a division writes a line to the corporation's journal (who, what,
how much). Each member also has a personal hangar at a corporation structure, as at an NPC
station, free. A corporation office at an NPC station, rented from the treasury, is an optional
later step.

### 8. Losing the ship, and losing the place

**The ship.** As today until modules are items (hull, fit and hold lost; a wreck with 25 % of
each good), then each fitted module drops at about 50 %. Insurance (#344) covers hull and fit,
never cargo. Hangars, other ships and their holds are untouched, because they are somewhere
else; that is what located ships buy the player. The wreck is one can with every good.

**A destroyed player structure (decided):** part of its storage drops into the wreck -- a share
of each stack, 50 % by default (`structureDropShare` in `world_rules.json`, *(call)* on the
number); the rest is destroyed. Every personal hangar and division in it drops the same way.
Defending a structure defends something, and a siege has a prize. The owners hear about it
offline, in the journal (#431 slice 13). An abandoned structure (#468) drops everything.

**A destroyed NPC station (decided, as drafted):** the pilot's hangar there moves to their home
station after a day of world time, for a fee of 10 % of its value at the home market, paid on
arrival; until paid, the moved goods sit in the home hangar under the same lien as unpaid rent.

#### Delivery out of a hostile station

When a station's faction comes to refuse a pilot (the Hated tier, at which its docks turn them
away), whatever the pilot keeps there is out of reach. The pilot is not punished for a war they
did not fight, but neither do they get their goods for free:

- **The request.** Docked at any other station or structure where they have a container, the
  pilot opens the unreachable hangar (it is listed, greyed, in the assets view, slice 9) and
  asks for **delivery to the container where they are** -- their hangar here, or a structure's
  storage they may use. All of it or chosen stacks. The destination must have room for it when
  asked (rented room counts), and that room is reserved.
- **The price is the NPC delivery quote** (below) for the route from that station to here, **times
  a hostility surcharge** (1.5 by default, *(call)*), paid up front, **to the hostile faction**:
  they hold the goods, and releasing them is a service they sell. A faction one hates still
  takes one's money, which is how a black market works and gives the living world a reason to
  hold rather than burn what it seizes.
- **It takes time**, the NPC service's time for the route. Rent stops accruing at the hostile
  station from the request. The goods are in transit and cannot be touched; the journal says
  when they arrive.
- **If the pilot makes peace first** (no longer Hated), nothing changes for a delivery under way;
  without one, the hangar is simply reachable again.
- **If the hostile station is destroyed** before or during the delivery, the destroyed-station
  rule applies to whatever has not arrived, and the delivery fee is refunded.

#### NPC delivery and player couriers

Two ways to have something carried, and the rule that keeps both worth having (decision 7):
**the NPC service is always there, always slower, and never cheap; players are free to beat it
on price and speed, and earn the gap.**

- **The NPC delivery service** is sold at any station with a market by its faction's freight
  office. It carries goods (later modules and ships) from a container at one station to a
  container at another, for a quote:

  `fee = max(minFee, rate × volume × jumps × risk) + insure × value × (risk - 1)`

  - `volume`: the goods' volume (units until slice 7); a ship counts as its hull's volume.
  - `jumps`: the route's length (`Route`, #432), the safer preference.
  - `risk`: 1 plus the sum over the route's systems of `(1 - security)`, so a route through
    lawless space costs more; a route through a system under siege or held by a faction at war
    with the carrier is refused.
  - `insure`: the service guarantees delivery, so the risky part of the value is priced in; it
    never loses a shipment.
  - `minFee`: no delivery is cheaper than this, so moving one unit next door is not free.
  - **Time:** `hoursPerJump` of world time per jump (2 h by default), at least one hour. A player
    flies a jump in minutes.
  - The fee goes to the faction's treasury, as rent does.
- **Player courier contracts (#473)**: the issuer puts the goods and a reward (and a collateral)
  on a contract; a courier picks them up and carries them in their hold, physically, through
  the real danger. **The NPC quote is shown beside every contract** when it is posted and when
  it is browsed, so both sides know the alternative.
- **Why couriers are not pointless.** The service is a fixed formula set *above* what a player's
  trip costs: by default `rate` is calibrated so that a full Hauler's load over five jumps
  quotes at about three times what that load earns as a trade margin per jump, and it takes
  hours where a player takes minutes. An issuer who pays a courier anything under the quote
  saves money; one who needs it today has no other choice. The gap between what the issuer
  saves and what the courier's time and risk are worth is the courier's living.
- **Why the service cannot be undercut into nothing** *(call on the reading)*: its price does
  not respond to the player market at all -- it is a quote from the route, never an auction --
  so no amount of player undercutting moves it, takes it off the map or turns it into a race to
  the bottom. It is the ceiling of the courier market and the floor of the issuer's certainty:
  always available, always insured. Players compete with each other below it, not with it.
- **Coherence with the living world.** The service is the faction's existing freight (#343 NPC
  haulers and convoys) sold to pilots; it refuses routes its own haulers would refuse, and when
  a faction loses its convoys to raids, its quotes rise with the risk of the systems on the way.
  A pilot raiding convoys therefore raises the price of the service -- and the value of player
  couriers -- in that region, which is the economy answering, not a balance patch.

## What we build overall

Everything a pilot owns has a place. Goods live in containers: a ship's hold, a personal hangar
at any station or structure, or a corporation division. Containers are one store with one
`move` verb and one window, driven by the mouse (drag and drop) or by buttons and agents alike.
NPC stations give 100 units free and rent the rest; one's own and one's corporation's
structures store for free and can be raided. Goods take room by volume, so what is worth
carrying, and in what ship, is a decision. Ships stay where they are left, each with its own
hold, so fleets and logistics exist on the map. Nothing moves between places except in a hold,
by a player courier or by the slower, dearer NPC service. Modules become goods made at
shipyards, which makes fitting, salvage and NPC fits part of the same economy.

What it costs the player in busywork: one window, a drag, and the occasional trip to fetch a
ship. What it does not add: physical boxes, three storage types, per-item timers, remote
trading, cargo mass.

## Slice plan

Each slice is one PR off `main`, shippable alone. Versions are what each needs; the numbers are
whatever is current when it lands.

1. **The inventory window over today's stores, and drag and drop in the framework.** `Ui::Drag`
   (payload, arm-then-drag, drop targets, the cursor label, ownership following the cursor
   during a drag) with tests through the public API; tables can be drag sources. A desk window
   listing the hold and, at a structure, its storage; amounts by a field and presets; a filter;
   moves by button or by drag; a good dragged onto the market selects it there; the HUD cargo
   line and `I` open it; the STORAGE block leaves the station window. Uses the existing
   `store` / `take` fields. *PROTO: none. Saves: none. Agent: `inventory` (read: every container
   reachable here and its contents).*
2. **One `move` verb and one container model.** An engine `Inventory` (goods by type, add,
   remove, used) carried by the ship's hold and the structure's storage; the server resolves a
   container reference (`hold`, `storage`, later `hangar`, `can`) to an inventory, a capacity and
   who may put and take. `Proto::Command` gets `move { from, to, good, amount }`; `storeType` /
   `takeType` stay as aliases for one version. *PROTO bump. Saves: none (the saved shapes do
   not change). Agent: `move_goods`; `store_cargo` / `take_cargo` become wrappers.*
3. **Personal hangars at NPC stations, with rent.** A hangar per (station, pilot) with the free
   100 and rented units up to the archetype's `storage`; rent in the macro pass to the station
   faction's treasury through `PayRent`; the lien when unpaid; buy into and sell from the
   hangar. Saved with the world by station key. *PROTO bump. WORLD: a new section an older
   reader ignores, no bump. Agent: `rent_hangar`; `move_goods`, `buy` and `sell_cargo` gain
   the hangar.*
4. **Cans and jettison.** A `Derelict` holds an `Inventory`; a lost ship leaves one wreck;
   `move` to a can jettisons (a new can, or one's own within reach); loot and `move` take from
   one; a good dropped on the world in space is jettisoned. *PROTO bump (a derelict's cargo is
   several goods). WORLD: the derelict's saved `cargo` becomes an object of goods; an older
   one-good entry is read as before, so a bump only if the reader cannot take both. Agent:
   `jettison`; `salvage` unchanged.*
5. **Corporation divisions and personal hangars at structures** (after #468). Storage splits into
   personal hangars and three divisions with role rights; `CorpAct division`; takes logged to the
   corporation journal. Existing `stored` goods migrate into the owner's party's *Shared*
   division. *PROTO bump. WORLD bump (the meaning of `stored` changes). Agent: `corp_act
   division`, containers carry a division.*
6. **When a place is lost.** Structure storage and hangars drop into the wreck at a share;
   NPC station hangars move home after a day for a fee; delivery out of a hostile station on
   request; offline journal lines. *PROTO bump (requests and transit). WORLD: pending moves
   saved, a new field. Agent: `request_delivery`; transit reported through `observe` and the
   journal.*
7. **Volume.** `volume` per good in `goods.json`; capacities ×10 in `ships.json` and
   `archetypes.json`; the free hangar allowance and rent per unit scaled with them; NPC haulers
   and job sizes in volume; volume and value-per-volume columns in the inventory and market
   windows. *PROTO bump (capacity changes meaning). ACCOUNT and WORLD: none (counts are
   unchanged; a container over its new capacity keeps its goods and accepts no more). Agent:
   `observe` and `inventory` report volume.*
8. **Ships with a location.** Owned ships become instances with a station and their own hold;
   several of one hull; switching only to a ship here; the starter issued at home; insurance and
   designs per instance; the hangar window lists ships here and elsewhere; ships are drag
   payloads between the hangar and the inventory. *ACCOUNT bump with a migration (every owned
   hull placed at the pilot's home, the current hold staying with the ship being flown). PROTO
   bump. Agent: `hangar` reports locations; `switch_ship` by ship id.*
9. **The assets view.** A tab of the inventory window: everything the pilot owns, grouped by
   system and place, with a "route there" button into the route planner (#432), and the
   unreachable hangars of slice 6. *PROTO bump (an assets summary sent on request). Agent:
   `assets`.*
10. **Modules as goods** (after #150, with #464). Modules join the goods registry as kind
    `module`; shipyards make them from their `cost`; refits draw from the hangar, then the hold,
    then buy; removed modules go to the hangar; wrecks drop fitted modules at a share; a module
    dragged onto a fitting slot fits it. *PROTO bump (per-good vectors widen). ACCOUNT and WORLD
    bumps if any saved key changes. Agent: `refit` unchanged; modules move with `move_goods`.*
11. **The NPC delivery service** (with #473's courier contracts, which show its quote). Quotes
    from the route, freight in transit, the fee to the faction. *PROTO bump. WORLD: shipments in
    transit. Agent: `delivery_quote`, `request_delivery`.*
12. **Optional: the ore hold** (`provides.ore`, mined goods only) and **a corporation office at
    an NPC station**. Data plus small rules; versions as for slice 7.

Slices 1-4 make sense on their own and do not depend on #468, #473 or #150. Slice 8 is the
largest single change to the player's habits and should be played before 10.

## Sources

- EVE asset safety: [EVE support](https://support.eveonline.com/hc/articles/208289365),
  [Brave Collective guide](https://wiki.bravecollective.com/public/dojo/wiki/returning-players),
  [forum on the fee](https://forums.eveonline.com/t/station-gets-destroyed-then-what/319954).
- EVE unified inventory reception (2012): [forum archive](https://forums-archive.eveonline.com/message/1342124/),
  [another thread](https://forums-archive.eveonline.com/message/1344276/).
- Star Citizen local inventory and freight elevators:
  [Alpha 3.24 notes](https://starcitizen.tools/Update%3AStar_Citizen_Alpha_3.24.0),
  [Freight elevator](https://starcitizen.tools/Freight_elevator), [Hangars](https://starcitizen.tools/Hangars).
- Elite Dangerous transfers: [PC Invasion](https://www.pcinvasion.com/?p=135203),
  [Frontier poll](https://elitepve.com/viewtopic.php?p=70749),
  [player reports](https://steamcommunity.com/app/359320/discussions/0/1744519017713471310).
- X4 wares and storage types: [Egosoft wiki](https://wiki.egosoft.com/X4%20Foundations%20Wiki/Manual%20and%20Guides/Objects%20in%20the%20Game%20Universe/Wares/).
- Starsector storage: [Market](https://starsector.wiki.gg/wiki/Market), [Colony](https://starsector.wiki.gg/wiki/Colony).
- Albion regional markets: [GuildOrder wiki](https://guildorder.com/games/albion/wiki/market-arbitrage-and-regional-pricing).
