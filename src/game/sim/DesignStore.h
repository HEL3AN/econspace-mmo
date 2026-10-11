#pragma once

#include "core/ShipDesign.h"
#include "sim/SaveSchema.h"

#include <map>
#include <set>
#include <string>

// The designs players made, kept by the server (#279 fitting, option B).
//
// A design is a shared, immutable type filed under a hash of its content (Ships::ContentId);
// a ship holds only its id. So the definition has to outlive every ship that flies it --
// including the ships of players who are not online, whose account files name the id and
// nothing else. The store keeps, for each design, which accounts' saves name it, and drops
// one nobody names and no live ship flies (Prune). Without that it would only ever grow:
// every refit files a design, and most are flown for an afternoon (#41).
//
// A stock design is never filed here. It is in ships.json on both sides of the wire, under
// the id the file gives it, and Find answers for it all the same.
class DesignStore
{
public:
    // Files a design and returns its id, the hash of its content -- the same id for the same
    // design, however many times and by whoever it is filed. Validated against the
    // catalogue and made known to this process (Archetypes::AddDesign); empty, with the
    // reason, for one that does not validate.
    std::string File(Ships::Design d, std::string& error);

    // A design by id: one filed here, or a stock one. Null for neither.
    const Ships::Design* Find(const std::string& id) const;
    // Whether an id is one this store holds (rather than a stock design).
    bool   Holds(const std::string& id) const { return designs_.count(id) != 0; }
    size_t Size() const { return designs_.size(); }

    // What an account's save names, replacing what it named before. Called whenever that
    // save is written, so the store's idea of who needs a design is the files' own.
    void SetUsers(const std::string& account, const std::set<std::string>& ids);

    // Drops every design no account's save names and nothing in `live` flies. Returns how
    // many went.
    int Prune(const std::set<std::string>& live);

    // designs.json beside world.json. A missing file is an empty store; one from a later
    // build is refused rather than read and written back without what it held (#20). A
    // design that no longer validates -- a module renamed in ships.json -- is not loaded,
    // and a ship that flew it returns to its hull's stock design when its account loads.
    bool         Save(const std::string& path) const;  // false: the file is as it was
    Save::Result Load(const std::string& path);

private:
    struct Entry
    {
        Ships::Design         design;
        std::set<std::string> users;  // account names whose save names it
    };
    std::map<std::string, Entry> designs_;
};
