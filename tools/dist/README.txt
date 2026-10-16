EconSpace -- portable build
===========================

This folder runs from anywhere: the programs read the game data from data/ beside them
(or from the folder given with --data <dir>).

  econspace   the game client
  econserver  the server -- the world lives here; every player connects to one
  econagent   an MCP server, so an AI agent can play (see docs/agents in the repository)

Host a server
-------------
  econserver host 50800             this machine only
  econserver host 50800 --public    reachable from other machines (open TCP 50800)

The server keeps the world (world.json, regions/), its accounts (accounts/) and who may
come in (allowlist.txt, bans.txt) in this folder. Copy them somewhere safe before
replacing the folder with a newer build.

Connect
-------
  econspace connect <server address> 50800 <name> <secret>

The first login with a name creates that account and sets its secret. There is no
encryption on the connection: use a secret you use nowhere else.

On Windows, start-server.bat and connect-example.bat do the same with a double-click.
