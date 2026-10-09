#!/usr/bin/env python3
"""A bot for EconSpace with no model in it: mine a belt, then sell the ore.

This is the smallest complete example of building on `econagent`, the game's MCP server.
It speaks MCP to it directly -- newline-delimited JSON-RPC 2.0 over the agent's stdin and
stdout -- using nothing but the Python standard library, so there is nothing to install and
nothing hidden. A model-driven bot calls exactly the same tools; the difference is only who
decides which one to call next.

Run a server first, then the bot:

    ./build/bin/server/econserver.exe host 50800
    python examples/agents/mining_bot.py ./build/bin/agent/econagent.exe 127.0.0.1 50800

It prints what it sees and does, and exits 0 when it has sold a hold of ore. CI runs it
against a real server on every change, so this file cannot quietly stop working.
"""

import json
import re
import subprocess
import sys


class Agent:
    """One econagent process, driven over MCP."""

    def __init__(self, exe, host, port, name, secret):
        # The agent is an ordinary game client: it logs in as `name` with `secret`. The
        # first login sets the secret; afterwards it has to match.
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
        # The MCP handshake: say who we are, then confirm we are ready.
        info = self.request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "mining_bot.py", "version": "1"},
        })
        self.notify("notifications/initialized")
        return info

    def call(self, tool, **args):
        """Call a tool and return its text. Every econagent tool answers in plain text."""
        result = self.request("tools/call", {"name": tool, "arguments": args})
        return "".join(part.get("text", "") for part in result.get("content", []))

    def wait_for(self, *kinds, timeout=180):
        """Sleep on the event journal until one of `kinds` arrives.

        This is the habit that matters most in a bot: wait on events, do not poll observe.
        An order runs for seconds or minutes on the server; the journal says when it is done.
        """
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


def first_id(observation, kind_word):
    """The id of the first object of a kind in an observation.

    Objects appear as `#<id> <kind> <name> <distance>u <bearing> ...`, column-padded -- e.g.
    `#12   station  Aurora Hub   2400u NE  dockable`. Ids from observe are the ids every
    order accepts.
    """
    m = re.search(r"#(\d+)\s+%s\b" % re.escape(kind_word), observation)
    return int(m.group(1)) if m else None


def cargo_resources(observation):
    """The names of what is in the hold, from the `CARGO  Iron 30, Ice 4` line.

    Those names are what sell_cargo takes.
    """
    for line in observation.splitlines():
        if line.startswith("CARGO"):
            return [item.strip().rsplit(" ", 1)[0] for item in line[5:].split(",") if item.strip()]
    return []


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    exe, host, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    name = sys.argv[4] if len(sys.argv) > 4 else "miner-bot"
    secret = sys.argv[5] if len(sys.argv) > 5 else "miner-bot-secret"

    bot = Agent(exe, host, port, name, secret)
    try:
        info = bot.start()
        print("connected to", info.get("serverInfo", {}).get("name", "econagent"))

        tools = [t["name"] for t in bot.request("tools/list")["tools"]]
        print("tools:", ", ".join(tools))

        world = bot.call("observe", detail="full")
        print(world)

        field = first_id(world, "field")
        station = first_id(world, "station")
        if field is None or station is None:
            print("no asteroid field or no station in this system -- nothing to do here")
            return 1

        print("mining field #%d until the hold is full" % field)
        print(" ", bot.call("mine", field_id=field, until_full=True))
        if bot.wait_for("order_done", "cargo_full", "order_failed") in (None, "order_failed"):
            return 1

        print("docking at station #%d" % station)
        print(" ", bot.call("dock", station_id=station))
        if bot.wait_for("docked", "order_failed") != "docked":
            return 1

        hold = bot.call("observe", detail="full")
        for resource in cargo_resources(hold):
            print("selling resource", resource, "->", bot.call("sell_cargo", resource=resource))

        print(bot.call("observe"))
        return 0
    finally:
        bot.close()


if __name__ == "__main__":
    sys.exit(main())
