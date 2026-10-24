# Add a good to the market

What markets trade is **data**, like everything else an object is
([#343](https://github.com/HEL3AN/econspace-mmo/issues/343)). The goods, their prices and stock,
what planets grow and what stations turn one good into another all live in `data/goods.json`,
and a new good needs no C++ at all.

This page adds **Coolant**: a refined good made from Ice at mining outposts and used up by trade
hubs. By the end it is a row in a docked station's market. The reference for every field is
[`world_format.md` › goods.json](../../documents/world_format.md#goodsjson--what-markets-trade-343).

- [Write the good](#write-the-good)
- [Make it and use it](#make-it-and-use-it)
- [See it](#see-it)
- [What each field is for](#what-each-field-is-for)
- [Loud and silent mistakes](#loud-and-silent-mistakes)

> **Try it in your own copy; do not commit it.** The tests load the shipped goods: with
> Coolant added, `tests/goods_tests.cpp` fails on purpose (it holds the file to ten goods, and
> to a chain with no loose end), and fixtures such as `tests/goods_named.h` name the goods
> there are. A pull request that adds a good for real changes those with it. When you are
> done: `git checkout data/goods.json`.

---

## Write the good

Add an entry to the `goods` array in `data/goods.json`. The order matters only for how the
market lists them, so put it beside the other refined goods, after `Fuel`:

```json
{ "id": "Coolant", "name": "Coolant", "tier": 1, "price": 30, "stock": 120, "supply": "imported" }
```

That alone makes it a good: every market in the galaxy now keeps a stock of it and quotes a
price for it. But nothing makes any and nothing uses any, so the stock only moves when a pilot
or a hauler trades it.

## Make it and use it

`industries` says what each kind of station does with goods, by its archetype id. A **recipe**
turns inputs into an output so many cycles an hour, as far as the inputs are in stock; **uses**
is what the station consumes an hour.

Add a recipe to the mining outpost, which already turns Ice into Fuel:

```json
"station.mining_outpost": {
    "recipes": [
        { "in": { "Iron": 3 }, "out": { "Steel": 1 }, "rate": 20 },
        { "in": { "Ice": 2 }, "out": { "Fuel": 1 }, "rate": 15 },
        { "in": { "Ice": 2 }, "out": { "Coolant": 1 }, "rate": 10 }
    ],
    "uses": { "Food": 8, "RepairPaste": 1 }
}
```

and a use to the trade hub:

```json
"station.trade_hub": {
    "recipes": [ ... as they are ... ],
    "uses": { "Food": 14, "Medicine": 2, "Fuel": 4, "Coolant": 3 }
}
```

Now a system with a mining outpost makes Coolant, one with a trade hub burns it, and the NPC
haulers carry it from where it is cheap to where it is dear, as they do every other good. A
trade hub short of it posts a delivery contract.

A planet can grow a good too: `"planets": { "planet.oceanic": { "Food": 40 } }` is per hour, per
planet of that archetype in the system. Coolant is made, not grown, so it has no line there.

## See it

Start a server, then a client that docks at the nearest station and opens its market on the new
good:

```sh
./build/bin/server/econserver.exe host 50800
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2 --start random --dock --market Coolant
```

`--dock` flies to the nearest station and docks; `--market Coolant` puts the market window in
front with Coolant selected, and on the right it says *refined* (its tier), what it pays you and
what it costs you. In the list it stands between Fuel and Circuits, with a bid, an ask and
the system's stock (a new world starts a system at its target: the good's `stock` for every
market in it, so 480 where four markets keep 120). Add
`--shot coolant.png --frames 900` to save that frame and exit instead.

Server and client must read the **same** `goods.json`: the wire sends goods in the file's order,
so a client with a different list is turned away at login with a message that names the file
(`Compat::SharedFiles`). A Debug build run from the build tree reads the repository's own
`data/`, so both sides see your edit with no rebuild. A copied build does not: give both
`--data <dir>`.

A world that already exists keeps the stock it saved for every other good and starts Coolant
fresh, at its target. To see a whole world grow up with it, run a batch world and watch the
prices: `./build/bin/server/econserver.exe 216000 --prices` prints how prices differ across the
markets at the start and every hour.

---

## What each field is for

| Field | Required | What it is for |
|---|---|---|
| `id` | yes | How saves, recipes, contracts and the wire name it. Unique. |
| `name` | | What a player is shown. Defaults to the `id`. |
| `tier` | | 0 raw, 1 refined, 2 components and consumables. Shown under the name in the market. |
| `price` | yes | A civilisation centre's price at the target stock. More than 0. |
| `stock` | yes | The target one market keeps; the price rises as the stock falls below it and falls above it. At least 1. |
| `supply` | | `local` (default): cheaper further from a centre, as ore is. `imported`: dearer there, as anything made in the centres is ([#342](https://github.com/HEL3AN/econspace-mmo/issues/342) S5). |
| `mined` | | An asteroid belt can hold it. A good that is not mined can still be made or grown. |

How far a price moves with the stock, the fee on every trade and how the haulers decide are the
`market` block's, shared by every good; see the reference.

## Loud and silent mistakes

`goods.json` is strict, like every content file
([#191](https://github.com/HEL3AN/econspace-mmo/issues/191)):

| Mistake | What happens |
|---|---|
| an unknown field (`"prise"`), a good named twice, a `supply` that is not `local` or `imported`, a `price` of 0 | the **whole** file is refused; the server and the client stop with a line naming it |
| a recipe or a use names a good that is not in `goods` | the **whole** file is refused, naming the industry |
| a syntax error -- a missing comma between two goods | the **whole** file is refused |
| the server and the client read different files | the client is turned away at login, with the file named |
| `industries` names an archetype that has no market | nothing happens: no station of that kind trades, so nothing is made or used |
| a recipe's inputs are never in stock where it runs | nothing is made, without a word: a recipe runs only as far as its inputs allow |

The last two are the quiet ones. If a good never appears where it should be made, check that the
station kind has a `market` component in `data/archetypes.json`, and that its inputs reach it.
