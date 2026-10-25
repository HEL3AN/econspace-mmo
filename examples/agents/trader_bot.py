#!/usr/bin/env python3
"""A trading bot for EconSpace with no model in it: find a price difference and work it.

It docks where it starts and notes the market, flies to a neighbouring system or two and notes
theirs, then picks the good that is cheapest to buy in one of them against what another pays
for it -- fees both ways counted -- and carries a hold of it if that leaves a profit. It reports
the margin it made, or that no trade was worth making among the markets it saw: in a calm
neighbourhood the spread and the fee eat a small difference, and a trader that knows when not
to trade is the point of the example.

Like mining_bot.py it speaks MCP to `econagent` directly -- newline-delimited JSON-RPC over
the agent's stdin and stdout -- with nothing but the Python standard library. It uses the
market, travel and event-journal tools: dock, observe's MARKET line, undock,
travel_to_system, buy, sell_cargo and wait_for_event.

Run a server first, then the bot:

    ./build/bin/server/econserver.exe host 50800
    python examples/agents/trader_bot.py ./build/bin/agent/econagent.exe 127.0.0.1 50800

It exits 0 when it has traded or found nothing worth trading, and 1 when something went
wrong or it ran out of time (four minutes).

A new world starts every market at its targets, so next to a fresh start nothing is worth
carrying yet; prices part as stations make and use goods and haulers move them. With
--trade-anyway the bot carries the best good it found even at a loss -- how CI, which runs it
against a fresh world on every change, sees the whole trip: buy, travel, sell.
"""

import json
import re
import subprocess
import sys
import time

DEADLINE = 240.0  # seconds the whole run may take
MAX_NEIGHBOURS = 2  # systems to look at besides the start
MIN_MARGIN = 0.05  # what a unit must make, as a share of its price, to be worth carrying


class Agent:
    """One econagent process, driven over MCP. The same client as mining_bot.py."""

    def __init__(self, exe, host, port, name, secret):
        self.proc = subprocess.Popen(
            [exe, "connect", host, str(port), name, secret],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr,
            text=True, encoding="utf-8", bufsize=1)
        self.next_id = 1

    def request(self, method, params=None):
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.next_id += 1
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("econagent closed the connection -- see its stderr above")
        reply = json.loads(line)
        if "error" in reply:
            raise RuntimeError("%s failed: %s" % (method, reply["error"]))
        return reply["result"]

    def notify(self, method):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()

    def start(self):
        info = self.request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "trader_bot.py", "version": "1"},
        })
        self.notify("notifications/initialized")
        return info

    def call(self, tool, **args):
        result = self.request("tools/call", {"name": tool, "arguments": args})
        return "".join(part.get("text", "") for part in result.get("content", []))

    def wait_for(self, *kinds, timeout=120):
        """Sleep on the event journal until one of `kinds` arrives, or nothing does."""
        while True:
            text = self.call("wait_for_event", timeout_seconds=min(timeout, 300))
            print("  events:", text.strip().replace("\n", " | "))
            if text.startswith("nothing happened"):
                return None
            for kind in kinds:
                if kind in text:
                    return kind

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


class GaveUp(Exception):
    """An order failed, or the run is out of time: the bot stops and says why."""


def ids_of(observation, kind_word):
    """Every id of a kind in an observation, nearest first: `#12   station  Lysen Exchange ...`."""
    return [int(m) for m in re.findall(r"#(\d+)\s+%s\b" % re.escape(kind_word), observation)]


def gate_destinations(observation):
    """The systems this one's gates lead to: `#13   gate   Gate to Iorulvel  ...  to r21-2-3`."""
    return re.findall(r"#\d+\s+gate\s+.*?\bto (\S+)\s*$", observation, re.M)


def system_of(observation):
    """The id of the system the ship is in: `SYSTEM Galul (r21-1-1)  security 1.00 ...`."""
    m = re.search(r"^SYSTEM .*?\((\S+)\)", observation, re.M)
    return m.group(1) if m else None


def money_of(observation):
    m = re.search(r"money (\d+) cr", observation)
    return int(m.group(1)) if m else 0


def free_hold(observation):
    m = re.search(r"cargo (\d+)/(\d+)", observation)
    return int(m.group(2)) - int(m.group(1)) if m else 0


def market_of(observation):
    """The docked station's market, from observe's MARKET line, as {good: (bid, ask, stock)}.

    `MARKET (bid/ask x stock; fee 2% a trade) Iron 9.5/10.5 x1599, Repair Paste 99.8/110.2 x240`
    -- the bid is what the market pays you, the ask what it charges. Also returns the fee.
    """
    for line in observation.splitlines():
        if line.startswith("MARKET"):
            fee = re.search(r"fee ([\d.]+)%", line)
            goods = {}
            for name, bid, ask, stock in re.findall(
                    r"([A-Za-z][A-Za-z ]*?) ([\d.]+)/([\d.]+) x(\d+)", line.split(")", 1)[-1]):
                goods[name.strip()] = (float(bid), float(ask), int(stock))
            return goods, (float(fee.group(1)) / 100.0 if fee else 0.02)
    return None, 0.0


class Trader:
    def __init__(self, agent, trade_anyway=False):
        self.agent = agent
        self.trade_anyway = trade_anyway
        self.started = time.time()
        self.markets = {}  # system id -> (station id, {good: (bid, ask, stock)})
        self.fee = 0.02

    def check_time(self):
        if time.time() - self.started > DEADLINE:
            raise GaveUp("out of time after %d s" % DEADLINE)

    def observe(self):
        return self.agent.call("observe", detail="full")

    def order(self, tool, done, **args):
        """Give an order and sleep until it is done; an order that fails ends the run."""
        self.check_time()
        print(">", tool, args, "->", self.agent.call(tool, **args))
        if self.agent.wait_for(done, "order_failed") != done:
            raise GaveUp("%s did not finish" % tool)

    def dock_at_market(self):
        """Dock at the nearest station with a market here and note its prices."""
        world = self.observe()
        for station in ids_of(world, "station")[:3]:
            self.order("dock", "docked", station_id=station)
            world = self.observe()
            goods, fee = market_of(world)
            if goods:
                self.fee = fee
                self.markets[system_of(world)] = (station, goods)
                print("  market at #%d in %s: %d goods" % (station, system_of(world), len(goods)))
                return world
            self.order("undock", "undocked")  # a dock with no market: try the next one
        raise GaveUp("no station with a market in %s" % system_of(world))

    def undock_if_docked(self):
        if "docked (" in self.observe():
            self.order("undock", "undocked")

    def go_to(self, system):
        """Fly to `system` and dock at a market there, unless already docked at one."""
        world = self.observe()
        if system_of(world) == system and "docked (" in world:
            return world
        self.undock_if_docked()
        if system_of(self.observe()) != system:
            self.order("travel_to_system", "order_done", system=system)
        return self.dock_at_market()

    def best_trade(self):
        """The good, the market to buy it at and the one to sell it at that make the most a
        unit, fees both ways counted -- or lose the least -- as (good, buy at, sell at, per
        unit, ask)."""
        best = None
        for buy_at, (_, buy_goods) in self.markets.items():
            for sell_at, (_, sell_goods) in self.markets.items():
                if buy_at == sell_at:
                    continue
                for good, (_, ask, stock) in buy_goods.items():
                    if good not in sell_goods or stock <= 0:
                        continue
                    bid = sell_goods[good][0]
                    per_unit = bid * (1 - self.fee) - ask * (1 + self.fee)
                    if best is None or per_unit > best[3]:
                        best = (good, buy_at, sell_at, per_unit, ask)
        return best

    def run(self):
        world = self.observe()
        if "NO SHIP YET" in world:
            print(self.agent.call("choose_start", start="random"))
            world = self.observe()
        home = system_of(world)
        money_before = money_of(world)
        print("starting in", home, "with", money_before, "cr")

        world = self.dock_at_market()
        for neighbour in gate_destinations(world)[:MAX_NEIGHBOURS]:
            self.check_time()
            self.go_to(neighbour)

        trade = self.best_trade()
        if trade is None or (trade[3] < MIN_MARGIN * trade[4] and not self.trade_anyway):
            print("RESULT no trade worth making among %d markets: the spread and a %.0f%% fee "
                  "both ways eat every difference" % (len(self.markets), self.fee * 100))
            if trade is not None:
                print("  the best was %s from %s to %s, %+.1f cr a unit" % trade[:4])
            return 0
        good, buy_at, sell_at, per_unit, ask = trade
        print("trade: %s, buy in %s at %.1f, sell in %s, about %.1f cr a unit" %
              (good, buy_at, ask, sell_at, per_unit))

        world = self.go_to(buy_at)
        # Keep a little money back: every unit bought raises the price of the next.
        amount = int(min(free_hold(world), money_of(world) * 0.9 / (ask * (1 + self.fee))))
        if amount <= 0:
            print("RESULT no room or money for a single unit of", good)
            return 0
        print(">", "buy", good, amount, "->", self.agent.call("buy", resource=good, amount=amount))
        spent = money_before - money_of(self.observe())

        world = self.go_to(sell_at)
        held = free_hold(world)
        print(">", "sell_cargo", good, "->", self.agent.call("sell_cargo", resource=good))
        # A sale is a station verb, answered with the next snapshot rather than in the journal:
        # look again until the hold has room again.
        for _ in range(20):
            world = self.observe()
            if free_hold(world) > held:
                break
            time.sleep(0.25)
        money_after = money_of(world)
        print("RESULT carried %s from %s to %s: spent %d cr, money %d -> %d, margin %+d cr" %
              (good, buy_at, sell_at, spent, money_before, money_after,
               money_after - money_before))
        return 0


def main():
    args = [a for a in sys.argv[1:] if a != "--trade-anyway"]
    if len(args) < 3:
        print(__doc__)
        return 2
    exe, host, port = args[0], args[1], int(args[2])
    name = args[3] if len(args) > 3 else "trader-bot"
    secret = args[4] if len(args) > 4 else "trader-bot-secret"

    agent = Agent(exe, host, port, name, secret)
    try:
        info = agent.start()
        print("connected to", info.get("serverInfo", {}).get("name", "econagent"))
        return Trader(agent, "--trade-anyway" in sys.argv).run()
    except GaveUp as why:
        print("GAVE UP:", why)
        return 1
    finally:
        agent.close()


if __name__ == "__main__":
    sys.exit(main())
