# Example bots

Bots that play EconSpace through its MCP server, `econagent`. The guide that explains them is
[docs/agents/](../../docs/agents/README.md).

| Example | Needs | What it does |
|---|---|---|
| [`mining_bot.py`](mining_bot.py) | Python 3, nothing else | mines a belt until the hold is full, docks and sells — the smallest complete bot, with no model in it |

```sh
./build/bin/server/econserver.exe host 50800
python examples/agents/mining_bot.py ./build/bin/agent/econagent.exe 127.0.0.1 50800
```

CI runs every example here against a real server, so they cannot quietly stop working. A new
example is welcome — add it to the table, and to the `The example bot plays` step in
`.github/workflows/build.yml` if it needs no model and no key.
