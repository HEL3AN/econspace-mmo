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

One tool is the exception to "CI runs it", because a CI runner has no screen:

| Script | What it does |
|---|---|
| [`capture-window.ps1`](capture-window.ps1) | Saves a PNG of a running game or editor window (Windows). Synthetic input does not reach a raylib window, so set the view up with arguments and capture it with this. |

```sh
./build/bin/server/econserver.exe host 50800 &
./build/bin/game/econspace.exe connect 127.0.0.1 50800 look hunter2 --zoom 0.0005 --shapes &
sleep 8 && powershell -File tools/capture-window.ps1 -Out system.png
```

`--zoom Z` starts the camera at a zoom (1 means one world unit is one pixel; 0.0005 shows a
whole system) and `--shapes` starts on the shape backend that F2 switches to. The binaries
carry MinGW's runtime inside them, so they start from any shell or from Explorer.

A new tool is welcome here when it saves more time than it costs to keep working — and when
it does, give it a CI step so it keeps working.
