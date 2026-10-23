# Changelog

What changed in each release of EconSpace, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/) loosely while the game is a prototype -- every 0.x
release may break saves and the wire, and a client plays only on a server of the same release.

A pull request that changes something a player, a host or a bot author would notice adds a line
under **Unreleased**. GitHub also drafts each release's notes from the merged pull requests
([`.github/release.yml`](.github/release.yml)); this file is the short version a person reads.

## [Unreleased]

`main` is far past 0.1: the protocol, the world save and the generator have all changed, so a
0.1 client cannot join and the next playtest starts on a fresh world.

### World, knowledge and territory
- Every pilot has their own map: public charts, their own finds and their corporation's (#443).
- Factions chart what they reach, and first contact reveals native names (#453); native names follow their culture (#448).
- The discoverer names a system and the admin moderates (#459).
- The map remembers what you saw, and when (#457); a region's charts go into the archive and come back with it (#462).
- A corporation holds a system by influence and a claim (#449), and only from next door (#454).
- Building in a faction's space takes a licence, by standing and rent (#456).
- Sell charts to a faction from the map, docked at its station: it pays by what its own picture lacks -- a system it never saw, what was built since its last look, your scanner finds -- and plans on what it bought (#479).
- Defences mend by tier: damage below the repair rate does nothing (#450).
- Security settles near its base, and the banner and the map show the same number (#434).

### Law and combat
- PvP under the law: criminal flags and security status (#452).
- Police answer a crime from real forces, by the system's security (#447); security is bought with police, and it matters (#460).
- NPCs and players fight by one combat model: an NPC fires its design's real fit (#463).
- A siege hits as hard as the faction's warships do (#458).

### Corporations and structures
- A corporation keeps its own AI patrols (#465): weighed by sieges, bought, fully set, and unseen members counted (#467).
- Attacks on your structures reach you offline, in your journal (#445).
- Players build outposts and grow them along the chain (#437); outposts keep goods, come apart for every tier and dock corporation mates (#461).
- Factions and players build shipyards by need (#441).

### Routes and jobs
- One route planner, with EVE's preferences (shortest, safer, less secure) and waypoints (#440); on the galaxy map with a star menu and favourites (#446).
- A command moves nothing while a standing order flies the ship (#444).
- Supply job cargo is delivered on arrival, and the job says where it goes (#435).
- Escort jobs show their convoy: where it is, where it goes, its ships (#436).

### The look
- The screen treatment no longer leaves a black curved frame or a red and cyan line at the screen's edge (#426).
- Planets are round at any zoom, and their bands end at the limb rather than in a straight edge (#426).
- A planet's orbit line fades out as you close in on the planet, and the system's edge is a HUD line again (#426).
- Ships are never drawn smaller than you can find them, at the zoom a station is flown at (#426).
- Asteroid fields and motherlodes show their rocks on a gallery card and from far away (#426).

### CI, tooling and docs
- CI: two clients no longer share one output pipe (#451).
- `.gitignore` covers per-machine settings and the rest of a world folder (#470).
- The design for knowledge, territory and law (#439); the top-level documents and design notes brought up to date with `main` (#469, #471); README screenshots from the 0.1 build (#424).
- Releases ship a macOS archive beside Windows and Linux: universal (Apple silicon and Intel), macOS 11 or later, unsigned; every package now carries the world editor (#478).

## [0.1.0] - 2026-10-17 "Anchor"

The first playable build: an early test release for small, invited groups. Windows zip and Linux
tarball; [release notes](https://github.com/HEL3AN/econspace-mmo/releases/tag/v0.1.0).

### Added
- **A world that grows by itself.** Each server generates a galaxy from a seed: civilisation centres to start from, and frontier regions that open over time as the tide rises. Native factions, stations and markets in every new region from the first minute; a mouth into a new region collapses unless someone builds an Anchor on it.
- **Roaming wormholes** with mass and ship-class limits, and a scanner (P) that finds them and hidden pirate bases.
- **An economy.** Ten goods in three tiers produced and consumed by recipe; prices follow stock and distance; NPC haulers carry goods between systems; belts run out and grow back; a 2% fee to the market's owner.
- **Work the world posts.** Faction plans post supply, escort and siege jobs; short markets post delivery contracts priced from the real shortage; convoys live or die by their own strength.
- **Risk.** Losing a ship loses the hull, its fit and its cargo, and the wreck can be looted; insurance per hull; you wake at your home shipyard.
- **Ships and people.** Refits at shipyards and a design library; local chat, private messages, server broadcasts and moderation; corporations with members, roles, a channel and structures that trust the corporation.
- **Agents.** `econagent`, an MCP server: an AI agent connects as a pilot and uses the same verbs as a person.
- **Hosting.** A portable package; `econserver init`, an allowlist and bans, a console and `econserver admin`, backups, a systemd unit and a hosting guide. A client is let in only by a server of the same version with the same data.

### Known limitations
- No macOS package (it builds and passes its tests in CI).
- Traffic is not encrypted; only the login is protected.
- Builds are not code-signed.
- No in-game server browser or connect screen.

[Unreleased]: https://github.com/HEL3AN/econspace-mmo/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/HEL3AN/econspace-mmo/releases/tag/v0.1.0
