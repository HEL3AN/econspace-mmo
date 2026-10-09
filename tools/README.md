# Tools

Scripts for working on EconSpace. Each one is run by CI as well, so none of them can quietly
stop working.

| Script | What it does |
|---|---|
| [`gen_agent_reference.py`](gen_agent_reference.py) | Generates [`docs/agents/reference.md`](../docs/agents/reference.md) from `econagent describe`, and with `--check` fails if the committed copy is stale or if a tool's schema is one an MCP client would not accept. Run it after changing a tool in `src/agent/main.cpp`. |

```sh
python tools/gen_agent_reference.py ./build/bin/agent/econagent.exe          # regenerate
python tools/gen_agent_reference.py ./build/bin/agent/econagent.exe --check  # what CI runs
```

A new tool is welcome here when it saves more time than it costs to keep working — and when
it does, give it a CI step so it keeps working.
