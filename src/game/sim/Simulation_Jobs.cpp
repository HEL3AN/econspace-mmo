// Jobs a faction's plans post (#345): the living world pays the player. One translation unit
// of Simulation (#17).
//
// Factions settle, grow and besiege whether or not anybody is there (#295, #342 S6). Each of
// those plans has work in it a pilot could do, and now it says so: a site going up wants
// goods, a builder convoy wants an escort, an outpost under siege wants the besiegers shot
// down, and the besiegers want its defenders shot down (#407). The job is posted on the
// boards of the poster's stations near what the plan is about, paid out of the same stock the
// plan was paid from, and over when the plan is -- it is never a template with nothing behind
// it.
//
// What a pilot does feeds the plan through the arithmetic the plan already runs on, the same
// hot or cold: a delivery takes a share off the time the site has left, a pilot with a convoy
// is escort strength in the sums that decide its fate, a besieger shot down is one fewer in
// the siege's force (#295), and a defender shot down one fewer in the garrison it wears
// against -- the job only pays for it and points pilots at it.
#include "sim/Simulation.h"
#include "sim/ClientSession.h"
#include "sim/WorldRules.h"

#include "core/Blueprint.h"
#include "economy/Market.h"
#include "economy/Resource.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::string Money(double cr)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f cr", cr);
    return buf;
}

// Every system within `depth` jumps of `from`, by how many.
std::map<std::string, int> JumpsFrom(const Simulation& sim, const std::string& from, int depth)
{
    std::map<std::string, int> found;
    if (!sim.HasSystem(from))
        return found;
    found[from] = 0;
    std::vector<std::string> ring{ from };
    for (int d = 1; d <= depth && !ring.empty(); d++)
    {
        std::vector<std::string> next;
        for (const std::string& id : ring)
            for (const std::string& n : sim.Neighbors(id))
                if (sim.HasSystem(n) && found.emplace(n, d).second)
                    next.push_back(n);
        ring = std::move(next);
    }
    return found;
}

// The structure a save knows by `key` in a system, read-only.
const Structure* StructureAt(const SystemState& st, const std::string& key)
{
    if (key.empty())
        return nullptr;
    int id = 0;
    for (const auto& kv : st.keys)
        if (kv.second == key)
            id = kv.first;
    for (const auto& e : st.entities)
        if (id != 0 && e->GetId() == id && e->GetKind() == EntityKind::Structure)
            return static_cast<const Structure*>(e.get());
    return nullptr;
}

// The largest line of what a blueprint costs: what its site is mostly built of. Ties go to
// the first line.
bool MainGood(const Blueprint* bp, ResourceType& good, int& amount)
{
    amount = 0;
    if (bp == nullptr)
        return false;
    for (const auto& c : bp->cost)
        if (c.second > amount)
        {
            good = c.first;
            amount = c.second;
        }
    return amount > 0;
}

// Whether a plan's site is the kind a delivery helps: one going up, laid by a settlement, an
// Anchor or an upgrade that has come to its site.
bool Sited(const Plan& p)
{
    return p.kind == Plan::Kind::Settle ||
           ((p.kind == Plan::Kind::Anchor || p.kind == Plan::Kind::Upgrade) && p.arrived);
}

bool Holds(const ClientSession& s, int planId, MissionType type)
{
    for (const Mission& m : s.missions.Active())
        if (m.planId == planId && m.type == type)
            return true;
    return false;
}

// The pilot is flying in `systemId`: undocked and alive.
bool FlyingIn(const ClientSession& s, const std::string& systemId)
{
    return s.ship && s.ship->IsAlive() && !s.IsDocked() && s.systemId == systemId;
}
}  // namespace

const Structure* Simulation::JobSite(const Plan& p) const
{
    if (!Sited(p))
        return nullptr;
    const SystemState* st = SystemById(p.target);
    const Structure*   t = st != nullptr ? StructureAt(*st, p.site) : nullptr;
    return t != nullptr && t->IsBuilding() ? t : nullptr;
}

std::vector<Mission> Simulation::JobsPostedAt(const ClientSession& s, int stationId) const
{
    std::vector<Mission>    out;
    const WorldRules::Jobs& r = WorldRules::JobRules();
    const SystemState*      here = SystemOf(s);
    const Station*          giver = nullptr;
    if (here == nullptr || r.perBoard <= 0)
        return out;
    for (const auto& e : here->entities)
        if (e->GetId() == stationId && e->GetKind() == EntityKind::Station)
            giver = static_cast<const Station*>(e.get());
    if (giver == nullptr)
        return out;
    const FactionId                  f = giver->GetFaction();
    const std::map<std::string, int> near = JumpsFrom(*this, here->id, r.reach);
    const float repMul = MissionRewardMultiplier(Factions::TierOf(s.account.GetReputation(f)));
    // What the faction can still pay for, in credits: a board shows nothing it could not.
    double purse = (double)minds_[f].stock * r.creditsPerStock;

    std::vector<std::pair<int, Mission>> found;  // by how far, then by plan
    for (const auto& kv : plans_)
    {
        const Plan& p = kv.second;
        const auto  at = near.find(p.target);
        int         jumps = at != near.end() ? at->second : -1;
        // A convoy's escort is wanted where the convoy is as well as where it is bound (#407):
        // that is where a pilot meets it -- and it sets out from the faction's stations.
        const std::string convoyAt = p.OnTheWay() ? p.ConvoyAt() : std::string();
        if (!convoyAt.empty())
        {
            const auto there = near.find(convoyAt);
            if (there != near.end() && (jumps < 0 || there->second < jumps))
                jumps = there->second;
        }
        if (jumps < 0)
            continue;
        Mission m;
        m.faction = f;
        m.giverStationId = stationId;
        m.planId = p.id;
        m.systemId = p.target;
        m.systemName = SystemName(p.target);
        m.rewardRep = (float)r.rep;
        const Structure* site = p.faction == f ? JobSite(p) : nullptr;
        ResourceType     good{};
        int              most = 0;
        if (site != nullptr && p.supplied < r.supplyMax &&
            MainGood(site->GetBlueprint(), good, most))
        {
            m.type = MissionType::Supply;
            m.resource = good;
            m.targetCount = std::max(1, (int)std::lround(r.supplyShare * most));
            m.deadline = site->GetCompletesAt();
            m.rewardMoney = m.targetCount * Market::DefaultBasePrice(good) * r.supplyMarkup;
            m.title = "Supply: " + site->GetName();
            m.description = "Deliver " + std::to_string(m.targetCount) + " " + ResourceName(good) +
                            " to the " + site->GetName() + " going up in " + m.systemName +
                            ". Every delivery brings it closer to done.";
        }
        else if (!convoyAt.empty() && p.faction == f)
        {
            // ...on its way, or held back waiting for better odds -- which a pilot with it
            // gives it (#407).
            m.type = MissionType::Escort;
            m.targetCount = 1;
            m.stage = p.setbacks;
            m.deadline = p.dueAt + r.escortSlack;
            const int jumps = std::max(1, (int)p.route.size() - 1);
            m.rewardMoney = r.escortBase + r.escortPerJump * jumps;
            m.rewardRep = (float)(r.rep * 1.5);
            m.title = "Escort: convoy to " + m.systemName;
            m.description =
                "Fly with the " + FactionName(f) + " builder convoy bound for " + m.systemName +
                (p.leg >= 0 ? ", now in " : ", waiting to set out from ") + SystemName(convoyAt) +
                ". Meet it there; while you are with it you are part of its escort. "
                "Paid when it arrives.";
        }
        else if (p.kind == Plan::Kind::Siege && p.against == f)
        {
            m.type = MissionType::Siege;
            m.against = p.faction;
            m.targetCount = std::clamp((int)std::lround(p.force), 1, std::max(1, r.siegeKills));
            m.deadline = p.dueAt;
            m.rewardMoney = r.siegePerKill * m.targetCount;
            m.rewardRep = (float)(r.rep * 1.5);
            m.title = "Break the siege of " + m.systemName;
            m.description = "Shoot down " + std::to_string(m.targetCount) + " of the " +
                            FactionName(p.faction) + " warships " +
                            (p.arrived ? "besieging" : "on their way to besiege") + " the " +
                            FactionName(f) + " outpost in " + m.systemName + ".";
        }
        else if (p.kind == Plan::Kind::Siege && p.faction == f && DefendersShown(p) >= 1.0f)
        {
            // The besieger's side of it (#407): its stations pay for the defenders shot down,
            // each one fewer in the garrison the siege's sums wear against.
            m.type = MissionType::Besiege;
            m.against = p.against;
            m.targetCount =
                std::clamp((int)std::lround(DefendersShown(p)), 1, std::max(1, r.siegeKills));
            m.deadline = p.dueAt;
            m.rewardMoney = r.siegePerKill * m.targetCount;
            m.rewardRep = (float)(r.rep * 1.5);
            m.title = "Join the siege of " + m.systemName;
            m.description = "Shoot down " + std::to_string(m.targetCount) + " of the " +
                            FactionName(p.against) + " ships defending its outpost in " +
                            m.systemName + ", which " + FactionName(f) +
                            (p.arrived ? " is besieging" : " is on its way to besiege") + ". " +
                            FactionName(p.against) + " will hold it against you.";
        }
        else
            continue;
        if (Holds(s, p.id, m.type))
            continue;  // one of each a plan's, a pilot
        m.rewardMoney *= repMul;
        m.rewardRep *= repMul;
        found.push_back({ jumps, std::move(m) });
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& kv : found)
    {
        if ((int)out.size() >= r.perBoard)
            break;
        if (kv.second.rewardMoney > purse)
            continue;
        purse -= kv.second.rewardMoney;
        out.push_back(std::move(kv.second));
    }
    // And the contracts its markets near have posted (#343, #407): paid out of the treasury.
    ContractsPostedAt(s, *giver, near, out);
    return out;
}

bool Simulation::PlanJobReadyNow(const ClientSession& s, const Mission& m) const
{
    if (!IsPlanJob(m.type) || !s.ship)
        return false;
    if (m.type != MissionType::Supply)
        return m.progress >= m.targetCount;
    // Beside the site, in flight, with the goods aboard.
    const auto       it = plans_.find(m.planId);
    const Structure* site = it != plans_.end() ? JobSite(it->second) : nullptr;
    if (site == nullptr || !FlyingIn(s, m.systemId) ||
        s.ship->GetCargoAmount(m.resource) < m.targetCount)
        return false;
    const Vector2 a = site->GetPosition(), b = s.ship->GetPosition();
    const float   d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    return d <= site->GetSize() + (float)WorldRules::JobRules().supplyRange;
}

int Simulation::JobSiteId(const ClientSession& s, const Mission& m) const
{
    if (m.type != MissionType::Supply || s.systemId != m.systemId)
        return 0;
    const auto       it = plans_.find(m.planId);
    const Structure* site = it != plans_.end() ? JobSite(it->second) : nullptr;
    return site != nullptr ? site->GetId() : 0;
}

void Simulation::ConvoyView(const ClientSession& s, const Mission& m, Proto::MissionView& v) const
{
    if (m.type != MissionType::Escort)
        return;
    v.rode = m.rode;
    const auto it = plans_.find(m.planId);
    if (it == plans_.end() || !it->second.OnTheWay())
        return;
    const Plan&       p = it->second;
    const std::string at = p.ConvoyAt();
    if (at.empty())
        return;
    v.convoySystemId = at;
    v.convoySystemName = SystemName(at);
    v.convoyMoving = p.leg >= 0;
    // Waiting, the way it will take begins where it waits; on its way, the next system along.
    const size_t next = p.leg >= 0 ? (size_t)p.leg + 1 : 1;
    if (next < p.route.size())
        v.convoyNextName = SystemName(p.route[next]);
    // Its ships are real only where somebody is to see them: the recipient's system, hot.
    if (p.hauler == 0 || p.fieldedIn != at || s.systemId != at)
        return;
    const SystemState* st = SystemById(at);
    if (st == nullptr)
        return;
    for (const auto& e : st->entities)
    {
        if (e->GetKind() != EntityKind::Npc)
            continue;
        const NpcShip& n = static_cast<const NpcShip&>(*e);
        if (n.GetTask() != p.id || !n.IsAlive())
            continue;
        if (n.GetId() == p.hauler)
            v.haulerId = n.GetId();
        else
            v.convoyEscortIds.push_back(n.GetId());
    }
}

void Simulation::SpeedSite(Plan& p)
{
    const double cut = WorldRules::JobRules().supplyCut;
    SystemState* st = SystemById(p.target);
    Structure*   t = st != nullptr ? StructureByKey(*st, p.site) : nullptr;
    if (t == nullptr || !t->IsBuilding() || cut <= 0.0)
        return;
    const double left = std::max(0.0, t->GetCompletesAt() - time_);
    const double done = time_ + left * (1.0 - cut);
    // Its time line is the truth, so it is the time line that moves: the site is further along
    // on every client, and finishes -- and the plan resolves -- when it now says.
    t->StartBuilding(t->GetStartedAt(), std::max(done, time_ + 1.0e-3));
    ScheduleStructure(st->id, *t);
    MarkStaticChanged(*st, t->GetId());
    if (due_.erase({ p.dueAt, p.id }) > 0)
    {
        p.dueAt = std::min(p.dueAt, t->GetCompletesAt());
        due_.insert({ p.dueAt, p.id });
    }
}

bool Simulation::CompleteJob(ClientSession& s, int index)
{
    std::vector<Mission>& active = s.missions.Active();
    const Mission         m = active[(size_t)index];
    const std::string     what = m.title;
    if (!PlanJobReadyNow(s, m))
    {
        s.RecordEvent(Ev::Kind::Notice, "Hand-in refused -- " + what + " is not done yet");
        return false;
    }
    std::string fed;
    if (m.type == MissionType::Supply)
    {
        s.ship->RemoveCargo(m.resource, m.targetCount);
        Plan& p = plans_[m.planId];
        p.supplied++;
        SpeedSite(p);
        fed = "; the site is further along";
    }
    // The faction pays out of the stock its plans are paid from: what it gives a pilot it does
    // not spend on its own expansion.
    float& stock = minds_[m.faction].stock;
    stock = std::max(0.0f, stock - (float)(m.rewardMoney / WorldRules::JobRules().creditsPerStock));
    s.account.AddMoney(m.rewardMoney);
    s.account.AddReputation(m.faction, m.rewardRep);
    active.erase(active.begin() + index);
    s.RecordEvent(Ev::Kind::Notice, "Job done -- " + what + ", paid " + Money(m.rewardMoney) +
                                        " by " + FactionName(m.faction) + fed);
    return true;
}

void Simulation::FailJob(ClientSession& s, int index, const std::string& why, bool penalty)
{
    std::vector<Mission>& active = s.missions.Active();
    const Mission         m = active[(size_t)index];
    active.erase(active.begin() + index);
    std::string cost;
    if (penalty && WorldRules::JobRules().failRep > 0.0)
    {
        s.account.AddReputation(m.faction, -(float)WorldRules::JobRules().failRep);
        char buf[64];
        std::snprintf(buf, sizeof buf, " (%s standing -%.1f)", FactionName(m.faction).c_str(),
                      WorldRules::JobRules().failRep);
        cost = buf;
    }
    s.RecordEvent(Ev::Kind::Notice, "Job over -- " + m.title + ": " + why + cost);
}

float Simulation::EscortsWith(const Plan& p, const std::string& systemId) const
{
    int n = 0;
    for (const auto& kv : sessions_)
        if (FlyingIn(kv.second, systemId))
            for (const Mission& m : kv.second.missions.Active())
                if (m.type == MissionType::Escort && m.planId == p.id && m.stage == p.setbacks)
                {
                    n++;
                    break;
                }
    return (float)n * (float)WorldRules::JobRules().escortForce;
}

float Simulation::DefendersShown(const Plan& p) const
{
    // Those a pilot can find there: the pirates as pirates, a lawful garrison as the police the
    // director flies under its flag. Anyone else's strength there is numbers, not ships.
    const SystemState* st = SystemById(p.target);
    if (st == nullptr || p.kind != Plan::Kind::Siege)
        return 0.0f;
    const bool shown = p.against == Factions::Pirates() ||
                       (Factions::IsLawful(p.against) && st->agg.policeFaction == p.against);
    return shown ? st->agg.presence[p.against] : 0.0f;
}

void Simulation::CreditJobKill(ClientSession& s, const NpcShip& killed)
{
    if (killed.GetTask() != 0)
        return;  // on an errand of its own: a convoy's, say
    for (Mission& m : s.missions.Active())
    {
        if (!IsSiegeJob(m.type) || m.progress >= m.targetCount)
            continue;
        const auto it = plans_.find(m.planId);
        if (it == plans_.end() || it->second.target != s.systemId)
            continue;
        // A besieger: a warship of the besieging faction -- what Simulation_Sieges counts as
        // the siege's. A defender (#407): a ship of the besieged faction's garrison, its police
        // or its pirates, which the recount takes off its strength there, and so off what
        // holds the besiegers back.
        const bool counts =
            m.type == MissionType::Siege
                ? killed.GetRole() == NpcRole::Warship && killed.GetFaction() == it->second.faction
                : (killed.GetRole() == NpcRole::Police || killed.GetRole() == NpcRole::Pirate) &&
                      killed.GetFaction() == it->second.against;
        if (!counts)
            continue;
        m.progress++;
        if (m.progress >= m.targetCount)
            s.RecordEvent(Ev::Kind::Notice,
                          "Job ready -- " + m.title + ": hand it in, anywhere, to be paid");
    }
}

void Simulation::SiegeOver(const Plan& p)
{
    SystemState* st = SystemById(p.target);
    const bool   stands = st != nullptr && StructureByKey(*st, p.site) != nullptr;
    for (auto& kv : sessions_)
    {
        ClientSession&        s = kv.second;
        std::vector<Mission>& active = s.missions.Active();
        for (int i = (int)active.size() - 1; i >= 0; i--)
        {
            Mission& m = active[(size_t)i];
            if (!IsSiegeJob(m.type) || m.planId != p.id || m.progress >= m.targetCount)
                continue;
            if (m.type == MissionType::Besiege)
            {
                // The besieger's side: done when the outpost fell and this pilot had a hand in
                // it (#407).
                if (!stands && m.progress > 0)
                {
                    m.progress = m.targetCount;
                    s.RecordEvent(Ev::Kind::Notice,
                                  "Job ready -- " + m.title +
                                      ": the outpost has fallen; hand it in to be paid");
                }
                else
                    FailJob(s, i,
                            stands ? "the siege was given up" : "the outpost fell without you",
                            false);
                continue;
            }
            if (stands && m.progress > 0)
            {
                // Lifted with the outpost standing, and this pilot had a hand in it.
                m.progress = m.targetCount;
                s.RecordEvent(Ev::Kind::Notice, "Job ready -- " + m.title +
                                                    ": the siege is broken; hand it in to be paid");
            }
            else
                FailJob(s, i,
                        stands ? "the siege was lifted without you" : "the outpost has fallen",
                        false);
        }
    }
}

void Simulation::StepJobs()
{
    const WorldRules::Jobs& r = WorldRules::JobRules();
    for (auto& kv : sessions_)
    {
        ClientSession&        s = kv.second;
        std::vector<Mission>& active = s.missions.Active();
        for (int i = (int)active.size() - 1; i >= 0; i--)
        {
            Mission& m = active[(size_t)i];
            if (!IsPlanJob(m.type) || m.progress >= m.targetCount)
                continue;  // not a plan's, or done and waiting to be handed in
            const auto  it = plans_.find(m.planId);
            const Plan* p = it != plans_.end() ? &it->second : nullptr;
            if (m.type == MissionType::Escort)
            {
                if (p == nullptr || p->setbacks != m.stage)
                {
                    FailJob(s, i, "the convoy did not get through", false);
                    continue;
                }
                if (p->arrived)
                {
                    if (!m.rode)
                    {
                        FailJob(s, i, "the convoy arrived without you", true);
                        continue;
                    }
                    m.progress = m.targetCount;
                    s.RecordEvent(Ev::Kind::Notice,
                                  "Job ready -- " + m.title +
                                      ": the convoy is in; hand it in to be paid");
                    continue;
                }
                if (!m.rode && p->leg >= 0 && FlyingIn(s, p->route[(size_t)p->leg]))
                {
                    m.rode = true;
                    s.RecordEvent(Ev::Kind::Notice,
                                  "With the convoy -- " + m.title + ": stay with it to its site");
                }
            }
            else if (m.type == MissionType::Supply)
            {
                if (p == nullptr || JobSite(*p) == nullptr)
                {
                    FailJob(s, i, "the site is no longer going up", false);
                    continue;
                }
                if (p->supplied >= r.supplyMax)
                {
                    FailJob(s, i, "the site has all it needs", false);
                    continue;
                }
            }
            else if (p == nullptr)
            {
                FailJob(s, i, "the siege is over", false);
                continue;
            }
            if (m.deadline > 0.0 && time_ > m.deadline)
                FailJob(s, i, "its time ran out", true);
        }
    }
}
