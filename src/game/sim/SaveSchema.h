#pragma once

// Versions for the files the server keeps, and what a load can conclude about one (#20).
//
// Decoding is permissive per field on purpose -- see Protocol.h -- and for a save that is
// exactly the wrong policy. A file written by a build that stored something differently
// loads "successfully" with defaults quietly filled in, and the player's progress degrades
// without anything failing. The version is what turns that into a decision.
namespace Save
{

// Bump when the meaning of a field changes or one is removed. Adding a field that older
// readers can ignore, and newer readers can default, does not need a bump -- that is what
// the permissive reader is for.
// 2: the world carries the seed and the rules' version of the region beyond the wormhole
//    (#140). A version 1 file has neither, and is read as the same galaxy with a region
//    made from a new seed.
// 3: the world carries what players changed in each system's static layer (#38). Older
//    files have none and load as the world the data describes. The bump is what keeps a
//    version 2 build from reading a newer file, ignoring the changes, and writing the world
//    back without them -- every structure built and every wreck searched, quietly undone.
// 4: an added object may be a structure (#39), written under "structures" with its time
//    line. A version 3 build would not rebuild one, drop it with a warning, and write the
//    world back without it -- every site and beacon gone after one checkpoint.
// 5: the world carries what each faction knows, the surveys under way and the history
//    (#295). A version 4 build would load the world without them and write it back so,
//    and every faction would forget what it had found. An older file loads with factions
//    that know their holdings and what is next door, as a new world's do.
// 6: a faction has a stock, a plan may be a settlement naming its outpost site, and a
//    structure's owner may be a faction (#295, slice 3). A version 5 build would drop the
//    settlements and the stock, leave the sites standing with nobody waiting on them, and
//    write the world back so. An older file loads with factions that have saved nothing.
// 7: a plan may be a siege of another faction's outpost, a faction keeps the outposts it can
//    no longer supply and where it lost one, and an outpost's damage may be decay (#295,
//    slice 5). A version 6
//    build would drop the sieges with their committed force still counted as presence, and
//    write the world back so. An older file loads with no siege under way.
// 8: a siege's force is lent by several holdings and travels (#295): it names who lent what
//    and when it arrives, and a plan may be what is left of it going home. A version 7 build
//    would drop the returns -- every ship coming home, lost -- and read a siege still on its
//    way as standing at the outpost already. An older file's sieges are read as lent by the
//    holding they set out from, a jump away, and already there.
// 9: the wreck of a structure decays at an instant it carries (#41). A version 8 build would
//    keep the wrecks for ever and write them back without it. An older file's wrecks are
//    given their time from the moment it loads.
// 10: ships leave a system that has too many for a neighbour that lacks them (#295), and those
//    on their way are counted only in the world's "migrants". A version 9 build would drop
//    them -- every ship between systems, gone -- and write the world back so. An older file
//    has nobody on the way.
// 11: a system's controller and every presence are by faction id, not by index (#355). A
//    version 10 build would read the ids as nothing and hand every system to the
//    unaffiliated faction. An older file's numbers are read in the order the factions were
//    compiled in: Independent, TradersGuild, Syndicate, Pirates.
// 12: the world carries its opening log, the tide of each edge region, the cap and whether
//    growth is paused (#342 S3), and every region it lists is sealed in regions/<slot>.json
//    beside it. A version 11 build would remake only the regions a new world opens with and
//    write the world back without the rest -- every region the tide opened, and everything
//    built in it, gone. An older file loads as the regions a new world opens with, sealed
//    from then on, with a tide that starts now.
// 13: a mouth is unstable until anchored, and collapses (#342 S4): the log says each region's
//    state, when its mouth closes, who anchored it and when anyone was last in it; drifted
//    regions are cut off, and regions that drifted away at the cap are in archive-<id>.json
//    beside the sealed ones. A version 12 build would read an unstable mouth as a permanent
//    one and a drifted or archived region as open. An older file's mouths all stay.
// 14: an Anchor or an upgrade is built from what a convoy brings (#342 S6): a plan says where
//    its convoy is on its way, what it builds and its escort; a faction remembers the mouths it
//    has heard of; an outpost may be the site of the market or the station it grows into. A
//    version 13 build would lay a convoy's Anchor at once and lose an upgrade with its site.
// 15: admin packs (#342 S7): the log may hold a region a pack wove in (`by: "pack"`, no
//    parent), and `packs` says where each pack went and what it hashed to. A version 14 build
//    would try to open a pack's region as a frontier from nowhere, fail, and save the world
//    without it. An older file has no packs.
inline constexpr int WORLD_VERSION = 15;

// 2: a ship's position is in a system a million units across (#159). An older one is
//    read for everything else, and the ship is placed beside a station.
// 3: a mission names its stations by system and station rather than by entity id, which
//    holds for one server run only (#227). An older one keeps its ids, as it always did.
// 4: reputation and bounty are objects by faction id, and a mission names its faction by id
//    (#355). An older file's lists and numbers are read in the order the factions were
//    compiled in: Independent, TradersGuild, Syndicate, Pirates.
// 5: an account's ships each fly a design (#279 fitting), written as `ships: [{hull,
//    design}]`. An older file's `ships` is a list of hull indices, and each is read as that
//    hull's stock design -- which is what every ship was. A version 4 build reading a version
//    5 file would find no hull it knows, hand the player the starter alone and save over
//    every ship they had.
// 6: the hold may carry any good of data/goods.json (#343), by its id. A version 5 build reads
//    every name it does not know as Iron, so a hold of Medicine would load as a hold of ore --
//    and be saved so.
// 7: a mission may be a faction plan's job (#345) -- supply, escort, break a siege -- with the
//    plan it is for. A version 6 build knows no such mission: it would keep one it could never
//    hand in, holding a slot for good, and save it so.
inline constexpr int ACCOUNT_VERSION = 7;
// designs.json, the designs players' ships fly (#279 fitting). 1: the first.
inline constexpr int DESIGNS_VERSION = 1;

// A file with no version at all: everything written before this existed. Those files are
// a strict subset of version 1, so they are read as-is rather than migrated.
inline constexpr int UNVERSIONED = 0;

enum class Result
{
    Ok,       // read, and the version is one this build understands
    Missing,  // no such file -- a new galaxy, or a player who has never played
    Corrupt,  // unreadable or not the shape a save has
    TooNew    // written by a later build; NOT read, and must not be written over
};

}  // namespace Save
