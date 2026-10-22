#pragma once

#include "entities/Defence.h"
#include "entities/Entity.h"
#include <string>

struct Blueprint;

// Something a player put into the world (#39). What it is and what it can do come from its
// archetype entirely -- a beacon and a depot are the same class -- so a new kind of
// deployable is a line of data, not a subclass. The class adds only what an archetype
// cannot say: the name its owner gave it, and its time line.
//
// The time line is three instants of the world's clock, not a timer (#136). From
// `startedAt` until `completesAt` it is a construction site: it wears the site's archetype
// and can do nothing yet. After that it is what it was built to be, until `expiresAt`, if
// it has one. Nothing accumulates, so a site is as far along on every client as on the
// server, and a restart in the middle of building costs nothing.
// Whether a player's structure stands by the leave of the system's holder (#431 slice 6),
// as the server last worked it out from its owner's licence. Nothing of it is saved: it is
// derived each pass, and sent with the layout so everyone can tell a trespasser.
enum class LicenceState
{
    None,      // needs none: nobody's system, the frontier, the lawless', or its owner's own
    Licensed,  // the rent is paid
    Unpaid,    // the rent is due and unpaid; it lapses at the instant given
    Trespass,  // no licence, or a lapsed one: lawless to shoot, and the holder may evict it
};

class Structure : public Entity
{
public:
    // How every site looks, whatever it will become.
    static constexpr const char* SITE_ARCHETYPE = "structure.site";

    Structure(Vector2 pos, float size, std::string name, std::string builds);

    std::unique_ptr<Entity> Clone() const override { return std::make_unique<Structure>(*this); }
    std::string             GetName() const override { return name_; }

    // The archetype it is, or will be once built.
    const std::string& GetBuilds() const { return builds_; }

    // Makes it a site until `completesAt`. Wears the site's look meanwhile.
    void StartBuilding(double startedAt, double completesAt);
    // An upgrade (#342 S6): from now on it is building `builds`, from blueprint `blueprint`, and
    // is called `name`. The caller starts the building; the damage it has taken stays.
    void Retask(std::string builds, std::string blueprint, std::string name);
    // The site is finished at `now`: from then on it is what it was built to be, and mends.
    void Complete(double now);
    bool IsBuilding() const { return completesAt_ > 0.0; }
    // 0..1 at world time `now`; 1 for something already built.
    float Progress(double now) const;

    double GetStartedAt() const { return startedAt_; }
    double GetCompletesAt() const { return completesAt_; }

    // When it is taken out of the world by itself (#41's decay, in its simplest form); 0
    // means it stays until somebody removes it.
    double GetExpiresAt() const { return expiresAt_; }
    void   SetExpiresAt(double t) { expiresAt_ = t; }

    // The blueprint it was made from (#39): what it cost, so what dismantling returns, and
    // how much it takes to destroy. Set when it is laid down and kept with it; one that does
    // not say (a save from before) is taken to be from the first blueprint that builds its
    // archetype. Null when no blueprint builds it any more.
    void               SetBlueprint(std::string id) { blueprint_ = std::move(id); }
    const std::string& GetBlueprintId() const { return blueprint_; }
    const Blueprint*   GetBlueprint() const;

    // Its hull at world time `now`. A site's grows with it, from the destruction rules' share
    // of the finished thing's to all of it, so this is f(time) like the rest of the time
    // line; the damage taken is the only thing that accumulates, and it is state.
    float MaxHull(double now) const;

    // Damage, and its mending (#431 D1). A finished structure mends `MendRate` hull a second
    // from the instant its damage was last set, so what it has taken at any moment is
    // f(time) like the rest of the time line: the same on every client and in a system
    // nobody is in, with no word from the server until somebody hits it again. A hit settles
    // the mending up to its instant and adds to what is left.
    float  GetDamage(double now) const;
    float  GetDamageSet() const { return damage_; }     // as of GetDamagedAt -- for the wire
    double GetDamagedAt() const { return damagedAt_; }  // and the save
    void   SetDamage(float d, double at);
    void   TakeDamage(float d, double now) { SetDamage(GetDamage(now) + d, now); }
    // Hull a second it mends now: its tier's share of its blueprint's hull (Blueprint::mend)
    // once finished, nothing while a site or while its owner does not supply it.
    float MendRate() const;
    // A faction's outpost its faction does not supply does not mend (upkeep decays it
    // instead); everything else does. Settles the mending up to `now` before it changes.
    bool Mends() const { return mends_; }
    void SetMends(bool on, double now);
    // 0..1 of its hull still standing at `now`; 0 is destroyed.
    float HullFraction(double now) const;

    // Its owner's rules of engagement (#340). Meaningful for a player's armed structure; a
    // faction's keeps its doctrine whatever this says.
    const Defence& GetDefence() const { return defence_; }
    void           SetDefence(Defence d) { defence_ = std::move(d); }

    // Its licence (#431 slice 6), set by the server: the state, and the instant that goes
    // with it -- paid to while licensed, lapsing at while unpaid; 0 otherwise.
    LicenceState GetLicence() const { return licence_; }
    double       GetLicenceUntil() const { return licenceUntil_; }
    void         SetLicence(LicenceState l, double until)
    {
        licence_ = l;
        licenceUntil_ = until;
    }

private:
    std::string  name_;
    std::string  builds_;
    std::string  blueprint_;
    float        damage_ = 0.0f;
    double       damagedAt_ = 0.0;  // the instant `damage_` was set; mending counts from here
    bool         mends_ = true;
    double       startedAt_ = 0.0;
    double       completesAt_ = 0.0;  // 0: built
    double       expiresAt_ = 0.0;    // 0: permanent
    Defence      defence_;
    LicenceState licence_ = LicenceState::None;
    double       licenceUntil_ = 0.0;
};
