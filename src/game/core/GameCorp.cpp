// Game -- the corporation (#347): the window where a pilot founds one, answers an invite,
// sees who is in theirs and, as its CEO or a director, runs it; and what the rest of the
// client knows of who is in which -- a ticker before a name, a corp mate's ship counted a
// friend's. Presentation only: who may do what is the server's (sim/Corporation.h), and the
// answer to every verb is a notice in chat. Part of the Game class; see Game.cpp.
#include "core/Game.h"
#include "sim/Corporation.h"

#include "ui/Theme.h"

#include <cctype>
#include <ctime>
#include <string>

namespace
{
std::string Lower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos)
        return std::string();
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

// The first word of `s`, and what follows it.
std::string Word(const std::string& s, std::string* rest = nullptr)
{
    const std::string t = Trim(s);
    const size_t      gap = t.find(' ');
    if (rest != nullptr)
        *rest = gap == std::string::npos ? std::string() : Trim(t.substr(gap));
    return t.substr(0, gap);
}

std::string Date(long long at)
{
    if (at <= 0)
        return std::string();
    const std::time_t t = (std::time_t)at;
    const std::tm*    tm = std::localtime(&t);
    if (tm == nullptr)
        return std::string();
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
    return buf;
}

const char* RoleWord(const std::string& role)
{
    if (role == "ceo")
        return "CEO";
    if (role == "director")
        return "Director";
    return "Member";
}

// A ledger line as a player reads it (#509), and how long ago it was.
std::string LedgerWhat(const Proto::LedgerLine& l)
{
    const std::string times = l.count > 1 ? " x" + std::to_string(l.count) : std::string();
    if (l.kind == "deposit")
        return l.who + " deposited";
    if (l.kind == "withdraw")
        return l.who + " withdrew";
    if (l.kind == "fees")
        return "Market fees" + times;
    if (l.kind == "rent_paid")
        return "Licence rent to " + l.note + times;
    if (l.kind == "rent_received")
        return "Licence rent from " + l.note + times;
    if (l.kind == "police")
        return "Police in " + l.note + times;
    if (l.kind == "patrol")
        return "Patrol upkeep in " + l.note + times;
    if (l.kind == "ships")
        return "Patrol ships replaced in " + l.note + times;
    return l.kind + times;
}

std::string Ago(long long at)
{
    const long long s = (long long)std::time(nullptr) - at;
    if (at <= 0 || s < 0)
        return std::string();
    if (s < 90)
        return "now";
    if (s < 5400)
        return std::to_string((s + 30) / 60) + " min ago";
    if (s < 2 * 86400)
        return std::to_string((s + 1800) / 3600) + " h ago";
    return std::to_string((s + 43200) / 86400) + " d ago";
}

bool TickerChar(char c)
{
    return std::isalnum((unsigned char)c) != 0;
}
}  // namespace

std::string Game::TickerOf(const std::string& pilot) const
{
    const std::string p = Lower(pilot);
    if (corpView_.Has(p))
        return corpView_.ticker;
    for (size_t i = 0; i < chatMembers_.pilots.size() && i < chatMembers_.corps.size(); i++)
        if (chatMembers_.pilots[i] == p)
            return chatMembers_.corps[i];
    return std::string();
}

std::string Game::Tagged(const std::string& pilot) const
{
    const std::string t = TickerOf(pilot);
    return t.empty() ? pilot : "[" + t + "] " + pilot;
}

bool Game::InMyCorp(const std::string& pilot) const
{
    const std::string p = Lower(pilot);
    return !corpView_.id.empty() && p != Lower(pilotName_) && corpView_.Has(p);
}

bool Game::ManagesFor(const std::string& owner) const
{
    const std::string me = Lower(pilotName_);
    if (owner.empty() || me.empty())
        return false;
    if (Lower(owner) == me)
        return true;
    const std::string role = corpView_.RoleOf(me);
    return (role == "ceo" || role == "director") &&
           (corpView_.Has(Lower(owner)) || OursOwner(owner));
}

bool Game::OursOwner(const std::string& owner) const
{
    if (owner.empty() || pilotName_.empty())
        return false;
    return Lower(owner) == Lower(pilotName_) ||
           (!corpView_.id.empty() && owner == Corps::OwnerOf(corpView_.id));
}

bool Game::CommandsFor(const std::string& owner) const
{
    if (owner.empty() || pilotName_.empty())
        return false;
    if (Lower(owner) == Lower(pilotName_))
        return true;
    const std::string role = corpView_.RoleOf(Lower(pilotName_));
    return (role == "ceo" || role == "director") && !corpView_.id.empty() &&
           owner == Corps::OwnerOf(corpView_.id);
}

std::string Game::OwnerLabel(const Proto::EntityLayout& el) const
{
    if (!el.ownerName.empty() && el.ownerName != el.owner)
        return el.ownerName;
    return Tagged(el.owner);
}

void Game::SendCorpAct(const Proto::CorpAct& act)
{
    if (clientLink_)
        clientLink_->Send(Proto::EncodeCorpAct(act));
}

bool Game::CorpCommand(const std::string& text)
{
    std::string       rest;
    const std::string verb = Lower(Word(text, &rest));
    Proto::CorpAct    act;
    act.verb = verb;
    if (verb == "create" || verb == "found")
    {
        // The ticker first: a name may have spaces in it.
        act.verb = "create";
        act.ticker = Word(rest, &act.name);
        if (act.ticker.empty() || act.name.empty())
        {
            ChatNotice("To found a corporation: /corp create <ticker> <name>");
            return true;
        }
    }
    else if (verb == "invite" || verb == "kick")
        act.pilot = Word(rest);
    else if (verb == "transfer" || verb == "take")
    {
        // A structure handed over (#468): its id, then a pilot or a corporation's [TICKER];
        // where the pilot is, as an id is a system's.
        std::string to;
        act.value = Word(rest, &to);
        to = Trim(to);
        if (verb == "transfer" && to.size() > 2 && to.front() == '[' && to.back() == ']')
            act.ticker = to.substr(1, to.size() - 2);
        else if (verb == "transfer")
            act.pilot = to;
        if (act.value.empty() || (verb == "transfer" && to.empty()))
        {
            ChatNotice("To hand a structure over: /corp transfer <structure id> <pilot>|[TICKER]; "
                       "to take an abandoned one beside you: /corp take <structure id>");
            return true;
        }
    }
    else if (verb == "join" || verb == "decline")
        act.ticker = Word(rest);
    else if (verb == "role")
        act.pilot = Word(rest, &act.role);
    else if (verb == "tax" || verb == "docking" || verb == "deposit" || verb == "withdraw")
        act.value = Word(rest);
    else if (verb == "police")
    {
        // The budget last: a system's name may have spaces in it.
        std::string  where = Trim(rest);
        const size_t cut = where.find_last_of(' ');
        if (cut == std::string::npos)
        {
            ChatNotice("To pay for police: /corp police <system> <credits an hour>|none");
            return true;
        }
        act.value = Trim(where.substr(cut + 1));
        act.system = Trim(where.substr(0, cut));
    }
    else if (verb == "patrol")
    {
        // design=<name> and engage=<mode> anywhere, the ships last: a system's name may have
        // spaces in it (#431 slice 12).
        std::string where;
        std::string word, more = Trim(rest);
        while (!(word = Word(more, &more)).empty())
        {
            if (Lower(word).rfind("design=", 0) == 0)
                act.design = word.substr(7);
            else if (Lower(word).rfind("engage=", 0) == 0)
                act.engage = word.substr(7);
            else if (Lower(word).rfind("threshold=", 0) == 0)
                act.threshold = word.substr(10);
            else if (Lower(word).rfind("allow=", 0) == 0)
                act.allow = word.substr(6);  // comma separated, no spaces; "none" clears it
            else
                where += (where.empty() ? "" : " ") + word;
        }
        const size_t cut = where.find_last_of(' ');
        if (cut == std::string::npos)
        {
            ChatNotice("To keep a patrol: /corp patrol <system> <ships>|none [design=<name>] "
                       "[engage=hold|return|enemies|strangers] [threshold=<standing>] "
                       "[allow=<name,name>|none]");
            return true;
        }
        act.value = Trim(where.substr(cut + 1));
        act.system = Trim(where.substr(0, cut));
    }
    else if (verb != "leave")
    {
        ChatNotice("/corp create <ticker> <name> | invite <pilot> | join <ticker> | decline "
                   "<ticker> | leave | kick <pilot> | role <pilot> ceo|director|member | tax "
                   "<percent>|standard | docking own|allies|all | deposit <cr> | withdraw <cr> | "
                   "police <system> <cr an "
                   "hour>|none | patrol <system> <ships>|none [design=<name>] "
                   "[engage=<mode>] | transfer <structure id> <pilot>|[TICKER] | take "
                   "<structure id>; /c <text> talks to the corporation");
        return true;
    }
    SendCorpAct(act);
    return true;
}

// In a corporation: who it is, its members with their roles and who is online, and what this
// pilot's role lets them do -- invite, remove, give roles, leave. In none: the invites
// waiting for an answer, and founding one.
void Game::DrawCorpContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = corpLayout_;

    const std::string           me = Lower(pilotName_);
    const std::string           myRole = corpView_.RoleOf(me);
    const bool                  leads = myRole == "ceo" || myRole == "director";
    const bool                  ceo = myRole == "ceo";
    std::vector<Proto::CorpAct> acts;  // what was clicked, sent after the frame is laid out

    L.Begin(f);
    L.Column(
        Box().Grow().ScrollY().Id("corp").Gap(t.metrics.gap),
        [&]
        {
            if (!corpView_.id.empty())
            {
                L.Text(corpView_.name + "  [" + corpView_.ticker + "]", TextStyle::Title());
                L.Field("Your role", RoleWord(myRole), t.colors.titleText);
                L.Field("Founded", Date(corpView_.founded), t.colors.dim);
                L.Tabs("corpTabs", { "Corporation", "Treasury" }, corpTab_);
                if (corpTab_ == 1)
                {
                    DrawTreasuryTab(L, leads, acts);
                    return;
                }
                // Its money, what it holds, and the holder's policies (#431 slice 5).
                L.Field("Treasury", TextFormat("%.0f cr", corpView_.treasury), t.colors.text);
                std::string held;
                for (const std::string& id : corpView_.holds)
                    held += (held.empty() ? "" : ", ") + MapSystemName(id);
                L.Field("Holds", held.empty() ? std::string("no system") : held,
                        held.empty() ? t.colors.dim : t.colors.text);
                L.Field("Tax", TextFormat("%.1f%% of every trade", corpView_.tax * 100.0f),
                        t.colors.text);
                L.Field("Docking",
                        corpView_.docking == "own"      ? "members only"
                        : corpView_.docking == "allies" ? "members and allow-lists"
                                                        : "anyone",
                        t.colors.text);
                // The police it pays for in each system it holds, and the security there
                // (#431 slice 11): security is bought, out of the treasury.
                for (const std::string& id : corpView_.holds)
                {
                    const auto  it = corpView_.police.find(id);
                    const float paid = it != corpView_.police.end() ? (float)it->second : 0.0f;
                    float       security = -1.0f;
                    for (const Proto::GalaxySystemStat& g : galaxyState_.systems)
                        if (g.id == id)
                            security = g.security;
                    std::string text = paid > 0.0f ? TextFormat("%.0f cr an hour", paid)
                                                   : std::string("none paid");
                    if (security >= 0.0f)
                        text += TextFormat(", security %.1f", security);
                    L.Field("Police, " + MapSystemName(id), text,
                            paid > 0.0f ? t.colors.text : t.colors.dim);
                    if (!leads)
                        continue;
                    L.Row(
                        Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                        [&]
                        {
                            std::string&         field = corpPolice_[id];
                            Ui::TextFieldOptions opt;
                            opt.placeholder = "cr an hour";
                            opt.maxLength = 7;
                            const Ui::EditResult r = L.TextField("police_" + id, field, opt);
                            const bool set = L.Button("setpolice_" + id, "Pay police", false,
                                                      !Trim(field).empty()) ||
                                             r.submitted;
                            if (set && !Trim(field).empty())
                            {
                                acts.push_back({ "police", "", "", "", "", Trim(field), id });
                                field.clear();
                            }
                            if (paid > 0.0f && L.Button("nopolice_" + id, "Stop"))
                                acts.push_back({ "police", "", "", "", "", "none", id });
                        });
                    L.Tooltip("setpolice_" + id,
                              "Credits an hour from the treasury for the world's police here: "
                              "they raise security, fly patrols and answer crimes under the "
                              "corporation's law. They stand down when the treasury runs dry");
                }
                // Its own patrols (#431 slice 12): what each keeps and what stands there now.
                // Set with /corp patrol; an officer can stand one down from here.
                for (const Proto::PatrolView& p : corpView_.patrols)
                {
                    std::string text =
                        TextFormat("%d of %s, %.1f there, %s, %.0f cr an hour", p.ships,
                                   p.design.c_str(), p.present, p.engage.c_str(), p.upkeep);
                    if (p.engage == "enemies")
                        text += TextFormat(", below %.0f", p.threshold);
                    if (!p.allow.empty())
                        text += ", allows " + p.allow;
                    L.Field("Patrol, " + MapSystemName(p.system), text,
                            p.present > 0.0f ? t.colors.text : t.colors.dim);
                    if (!leads)
                        continue;
                    // Its threshold and allow-list (#431 slice 12), the rules a structure keeps.
                    L.Row(
                        Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                        [&]
                        {
                            std::string&         th = corpPatrolThreshold_[p.system];
                            std::string&         allow = corpPatrolAllow_[p.system];
                            Ui::TextFieldOptions opt;
                            opt.placeholder = TextFormat("%.0f", p.threshold);
                            opt.maxLength = 6;
                            const Ui::EditResult r1 = L.TextField("patrolth_" + p.system, th, opt);
                            Ui::TextFieldOptions wide;
                            wide.placeholder = p.allow.empty() ? "allow: names" : p.allow;
                            wide.maxLength = 120;
                            const Ui::EditResult r2 =
                                L.TextField("patrolallow_" + p.system, allow, wide);
                            const bool any = !Trim(th).empty() || !Trim(allow).empty();
                            if ((L.Button("setpatrol_" + p.system, "Set", false, any) ||
                                 r1.submitted || r2.submitted) &&
                                any)
                            {
                                Proto::CorpAct a;
                                a.verb = "patrol";
                                a.value = std::to_string(p.ships);
                                a.system = p.system;
                                a.threshold = Trim(th);
                                a.allow = Trim(allow);
                                acts.push_back(a);
                                th.clear();
                                allow.clear();
                            }
                            if (L.Button("nopatrol_" + p.system, "Stand down"))
                            {
                                Proto::CorpAct a;
                                a.verb = "patrol";
                                a.value = "none";
                                a.system = p.system;
                                acts.push_back(a);
                            }
                        });
                    L.Tooltip("setpatrol_" + p.system,
                              "The standing below which it fires on a faction (on enemies), and "
                              "whom it lets be on strangers: pilots and factions, comma "
                              "separated, or none");
                }
                if (leads)
                    L.Text("/corp patrol <system> <ships> [design=<name>] [engage=<mode>] keeps "
                           "a patrol where the corporation holds or has a structure",
                           TextStyle::Label());
                if (leads)
                {
                    L.Row(
                        Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                        [&]
                        {
                            Ui::TextFieldOptions opt;
                            opt.placeholder = "tax, %";
                            opt.maxLength = 6;
                            const Ui::EditResult r = L.TextField("tax", corpTax_, opt);
                            if ((L.Button("settax", "Set tax", false, !Trim(corpTax_).empty()) ||
                                 r.submitted) &&
                                !Trim(corpTax_).empty())
                            {
                                acts.push_back({ "tax", "", "", "", "", Trim(corpTax_) });
                                corpTax_.clear();
                            }
                            if (L.Button("stdtax", "Standard"))
                                acts.push_back({ "tax", "", "", "", "", "standard" });
                        });
                    L.Tooltip("settax", "The fee on trades at every market in the systems the "
                                        "corporation holds; it goes to the treasury");
                    L.Row(Box().GrowX().Gap(t.metrics.rowGap),
                          [&]
                          {
                              for (const char* d : { "own", "allies", "all" })
                                  if (L.Button(TextFormat("dock_%s", d),
                                               d == std::string("own")      ? "Dock: members"
                                               : d == std::string("allies") ? "Dock: allies"
                                                                            : "Dock: anyone",
                                               corpView_.docking == d))
                                      acts.push_back({ "docking", "", "", "", "", d });
                          });
                    L.Tooltip("dock_allies", "Members, and whoever each structure's rules of "
                                             "engagement hold fire for");
                }
                // The licences it holds, in others' space (#431 slice 6, #509): bought where
                // they are granted, so here they are listed and renewed, not offered.
                DrawLicencesHeld(L, leads);
                int online = 0;
                for (const Proto::CorpMember& m : corpView_.members)
                    online += m.online ? 1 : 0;
                L.Text(TextFormat("MEMBERS  %d, %d online", (int)corpView_.members.size(), online),
                       TextStyle::Label());

                Ui::TableSpec table;
                table.id = "members";
                table.columns = { { "pilot", Size::Grow(), Ui::Align::Start, false, true },
                                  { "role", Size::Fit(0.0f, 96.0f), Ui::Align::Start } };
                table.rows = (int)corpView_.members.size();
                table.cell = [&](int r, int c)
                {
                    const Proto::CorpMember& m = corpView_.members[(size_t)r];
                    if (c == 1)
                        return Ui::Cell{ RoleWord(m.role), t.colors.dim };
                    Ui::Cell cell{ m.pilot, m.pilot == me ? t.colors.titleText
                                            : m.online    ? t.colors.text
                                                          : t.colors.dim };
                    // Online, in the instruments' friendly colour, and a ring for whoever is not.
                    cell.chip = m.online ? t.standing.friendly : t.colors.dim;
                    cell.chipMark = m.online ? Ui::Mark::Circle : Ui::Mark::Ring;
                    return cell;
                };
                table.rowFill = [&](int r)
                {
                    return corpView_.members[(size_t)r].pilot == corpChosen_ ? t.colors.selected
                                                                             : Color{ 0, 0, 0, 0 };
                };
                table.tooltip = [&](int r)
                {
                    const Proto::CorpMember& m = corpView_.members[(size_t)r];
                    return m.pilot + (m.online ? " -- online" : " -- offline");
                };
                const Ui::TableEvents ev = L.Table(table);
                if (ev.clicked >= 0)
                    corpChosen_ = corpView_.members[(size_t)ev.clicked].pilot;

                // What this pilot's role lets them do to the member chosen.
                const std::string theirRole = corpView_.RoleOf(corpChosen_);
                if (!corpChosen_.empty() && corpChosen_ != me && !theirRole.empty() && leads)
                {
                    L.Text(corpChosen_, TextStyle::Strong());
                    L.Row(
                        Box().GrowX().Gap(t.metrics.rowGap),
                        [&]
                        {
                            if (ceo &&
                                L.Button("director",
                                         theirRole == "director" ? "Make member" : "Make director"))
                                acts.push_back({ "role", "", "", corpChosen_,
                                                 theirRole == "director" ? "member" : "director" });
                            if (ceo && L.Button("ceo", "Make CEO"))
                                acts.push_back({ "role", "", "", corpChosen_, "ceo" });
                            const bool mayRemove = ceo || theirRole == "member";
                            if (L.Button("kick", "Remove", false, mayRemove))
                                acts.push_back({ "kick", "", "", corpChosen_, "" });
                        });
                    if (ceo)
                        L.Tooltip("ceo", "Hand the corporation over: you become a director");
                }

                if (leads)
                    L.Row(
                        Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                        [&]
                        {
                            Ui::TextFieldOptions opt;
                            opt.placeholder = "pilot to invite (online)";
                            opt.maxLength = 32;
                            const Ui::EditResult r = L.TextField("invitee", corpInvitee_, opt);
                            if ((L.Button("invite", "Invite", false, !Trim(corpInvitee_).empty()) ||
                                 r.submitted) &&
                                !Trim(corpInvitee_).empty())
                            {
                                acts.push_back({ "invite", "", "", Trim(corpInvitee_), "" });
                                corpInvitee_.clear();
                            }
                        });
                L.Divider();
                L.Text("/c <text> in chat talks to the corporation. Its members' batteries hold "
                       "fire for you and guard your ship.",
                       TextStyle::Small().Tint(t.colors.dim).Wrap());
                const bool alone = corpView_.members.size() == 1;
                if (L.Button("leave", alone ? "Disband" : "Leave", false, alone || !ceo))
                    acts.push_back({ "leave", "", "", "", "" });
                if (ceo && !alone)
                    L.Tooltip("leave", "Make another member CEO before leaving");
            }
            else
            {
                L.Text("You are in no corporation.", TextStyle::Body().Tint(t.colors.dim));
                for (size_t i = 0; i < corpView_.invites.size(); i++)
                {
                    const Proto::CorpInvite& inv = corpView_.invites[i];
                    L.Text(inv.name + "  [" + inv.ticker + "] invites you (" + inv.by + ")",
                           TextStyle::Body().Wrap());
                    L.Row(Box().GrowX().Gap(t.metrics.rowGap),
                          [&]
                          {
                              if (L.Button(TextFormat("join%d", (int)i), "Join", true))
                                  acts.push_back({ "join", "", inv.ticker, "", "" });
                              if (L.Button(TextFormat("decline%d", (int)i), "Decline"))
                                  acts.push_back({ "decline", "", inv.ticker, "", "" });
                          });
                }
                DrawLicencesHeld(L, true);
                L.Divider();
                L.Text("Found a corporation", TextStyle::Title());
                L.FormRow("Name",
                          [&]
                          {
                              Ui::TextFieldOptions opt;
                              opt.placeholder = "3 to 24 letters";
                              opt.maxLength = 24;
                              L.TextField("name", corpName_, opt);
                          });
                L.FormRow("Ticker",
                          [&]
                          {
                              Ui::TextFieldOptions opt;
                              opt.placeholder = "2 to 5";
                              opt.maxLength = 5;
                              opt.accept = TickerChar;
                              L.TextField("ticker", corpTicker_, opt);
                          });
                const bool ready = !Trim(corpName_).empty() && corpTicker_.size() >= 2;
                if (L.Button("found", "Found", true, ready))
                    acts.push_back({ "create", Trim(corpName_), corpTicker_, "", "" });
            }
        });
    L.End();
    L.Draw();

    for (const Proto::CorpAct& a : acts)
        SendCorpAct(a);
    if (!acts.empty() && acts.back().verb == "create")
        corpName_.clear(), corpTicker_.clear();
    if (!acts.empty() && acts.back().verb == "kick")
        corpChosen_.clear();
}

// The treasury (#509): what it holds, the pilot's deposit and, for an officer, withdrawal, and
// the ledger -- the last flows in and out, the automatic ones beside the pilots' own.
void Game::DrawTreasuryTab(Ui::Layout& L, bool leads, std::vector<Proto::CorpAct>& acts)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    L.Field("Treasury", TextFormat("%.0f cr", corpView_.treasury), t.colors.money);
    L.Field("Your money", TextFormat("%.0f cr", snapshot_.player.money), t.colors.text);
    L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
          [&]
          {
              Ui::TextFieldOptions opt;
              opt.placeholder = "credits";
              opt.maxLength = 10;
              opt.accept = [](char c) { return c >= '0' && c <= '9'; };
              L.TextField("treasuryAmount", corpAmount_, opt);
              const bool ready = !Trim(corpAmount_).empty();
              if (L.Button("deposit", "Deposit", false, ready))
              {
                  acts.push_back({ "deposit", "", "", "", "", Trim(corpAmount_) });
                  corpAmount_.clear();
              }
              if (L.Button("withdraw", "Withdraw", false, ready && leads))
              {
                  acts.push_back({ "withdraw", "", "", "", "", Trim(corpAmount_) });
                  corpAmount_.clear();
              }
          });
    L.Tooltip("deposit", "Put some of your own money into the corporation's treasury, which "
                         "pays its licences, police and patrols");
    L.Tooltip("withdraw", "The CEO or a director takes money out of the treasury, to themselves");
    L.Text("LEDGER", TextStyle::Label());
    if (corpView_.ledger.empty())
    {
        L.Text("Nothing has gone in or out yet.", TextStyle::Small().Tint(t.colors.dim));
        return;
    }
    Ui::TableSpec table;
    table.id = "ledger";
    table.columns = { { "when", Size::Fit(0.0f, 96.0f), Ui::Align::Start },
                      { "what", Size::Grow(), Ui::Align::Start, false, true },
                      { "credits", Size::Fit(0.0f, 110.0f), Ui::Align::End } };
    table.rows = (int)corpView_.ledger.size();
    table.cell = [&](int r, int c)
    {
        const Proto::LedgerLine& l = corpView_.ledger[(size_t)r];
        if (c == 0)
            return Ui::Cell{ Ago(l.at), t.colors.dim };
        if (c == 1)
            return Ui::Cell{ LedgerWhat(l), t.colors.text };
        return Ui::Cell{ TextFormat("%s%.0f", l.amount >= 0.0 ? "+" : "-", std::fabs(l.amount)),
                         l.amount >= 0.0 ? t.colors.good : t.colors.bad };
    };
    L.Table(table);
}

// The licences this pilot's party holds (#509): a corporation's, or their own in none -- the
// system, who granted it, what it costs and until when -- with Renew, which buys another week
// where it is granted: docked at the holder's station there.
void Game::DrawLicencesHeld(Ui::Layout& L, bool mayPay)
{
    using Ui::Box;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    if (corpView_.licences.empty())
        return;
    const double              now = worldClock_.Now(GetTime());
    const Proto::LicenceView& here = snapshot_.player.licence;
    L.Divider();
    L.Text("LICENCES TO BUILD", TextStyle::Label());
    for (const Proto::LicenceView& l : corpView_.licences)
    {
        const bool        ok = l.state == "licensed";
        const double      days = (l.until - now) / 86400.0;
        const std::string name = MapSystemName(l.system);
        L.Field(name,
                l.state == "licensed"
                    ? TextFormat("%s, paid until %.1f d from now", l.holder.c_str(), days)
                : l.state == "unpaid"
                    ? TextFormat("%s, UNPAID: lapses in %.0f h", l.holder.c_str(), days * 24.0)
                    : TextFormat("%s, LAPSED: trespassing", l.holder.c_str()),
                ok ? t.colors.text : t.colors.bad);
        const bool dockedThere = here.atHolder && snapshot_.systemId == l.system;
        L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
              [&]
              {
                  L.Text(TextFormat("%.0f cr a week", l.rent),
                         TextStyle::Small().Tint(t.colors.money));
                  if (L.Button("renew_" + l.system, "Renew", false, dockedThere && mayPay))
                  {
                      Proto::Command c;
                      c.buyLicence = true;
                      clientLink_->Send(Proto::EncodeCommand(c));
                  }
              });
        std::string tip =
            !mayPay ? "Your corporation's CEO or a director pays its licences, from the treasury"
            : dockedThere
                ? "Pay a week ahead"
                : "A licence is renewed where it is granted: docked at " +
                      (l.where.empty() ? l.holder + "'s station" : l.where) + " in " + name;
        L.Tooltip("renew_" + l.system, tip);
    }
}

// What building in this system takes, offered where it is granted (#509): docked at the
// holder's station or structure here. Anywhere else it only says where to go.
void Game::DrawLicenceOffer(Ui::Layout& L)
{
    using Ui::TextStyle;
    const Ui::Theme&          t = Ui::CurrentTheme();
    const Proto::LicenceView& l = snapshot_.player.licence;
    if (l.holder.empty())
        return;  // nobody here asks for one
    const double now = worldClock_.Now(GetTime());
    L.Divider();
    L.Text("LICENCES", TextStyle::Label());
    if (!l.atHolder)
    {
        L.Text("Building in this system takes " + l.holder + "'s licence, " +
                   std::string(TextFormat("%.0f cr every %.0f days", l.rent, l.periodDays)) +
                   "; it is sold docked at " + (l.where.empty() ? "its stations" : l.where) + ".",
               TextStyle::Small().Tint(t.colors.dim).Wrap());
        return;
    }
    L.Field("Granted by", l.holder, t.colors.text);
    L.Field("Rent", TextFormat("%.0f cr every %.0f days", l.rent, l.periodDays), t.colors.money);
    const bool paid = l.state == "licensed";
    L.Field(l.party == "you" ? "Yours" : "Held by " + l.party,
            paid                  ? TextFormat("paid, %.0f h more", (l.until - now) / 3600.0)
            : l.state == "unpaid" ? TextFormat("UNPAID: lapses in %.0f h", (l.until - now) / 3600.0)
            : l.state == "trespass" ? std::string("LAPSED: what stands here trespasses")
                                    : std::string("none"),
            paid                ? t.colors.good
            : l.state == "none" ? t.colors.dim
                                : t.colors.bad);
    if (!l.admitted)
    {
        L.Text(l.why, TextStyle::Small().Tint(t.colors.bad).Wrap());
        return;
    }
    if (!l.canPay)
    {
        L.Text("Your corporation's CEO or a director pays its licences, from the treasury.",
               TextStyle::Small().Tint(t.colors.dim).Wrap());
        return;
    }
    if (L.Button("licence",
                 TextFormat("%s  %.0f cr", paid ? "Renew a week ahead" : "Buy a licence", l.rent)))
    {
        Proto::Command c;
        c.buyLicence = true;
        clientLink_->Send(Proto::EncodeCommand(c));
    }
    L.Tooltip("licence", "Building here takes the holder's licence, a rent taken each week. "
                         "Unpaid a day past due, what you have here trespasses: anyone may shoot "
                         "it, and the holder may evict it.");
}

void Game::DrawTransfer(Ui::Layout& L, const Entity& structure)
{
    using Ui::Box;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    if (!CommandsFor(structure.GetOwner()))
        return;
    L.Divider();
    L.Text("HAND IT OVER", TextStyle::Label());
    L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
          [&]
          {
              Ui::TextFieldOptions opt;
              opt.placeholder = "pilot or ticker";
              opt.maxLength = 32;
              L.TextField("transfer_to", structureTransfer_, opt);
              const std::string to = Trim(structureTransfer_);
              Proto::CorpAct    act;
              act.verb = "transfer";
              act.value = std::to_string(structure.GetId());
              if (L.Button("transfer_pilot", "To pilot", false, !to.empty()))
                  act.pilot = to;
              if (L.Button("transfer_corp", "To corporation", false, !to.empty()))
                  act.ticker = to;
              if (!act.pilot.empty() || !act.ticker.empty())
              {
                  SendCorpAct(act);
                  structureTransfer_.clear();
              }
          });
    L.Tooltip("transfer_pilot",
              "Hand it to a pilot who is online now, or to a corporation by its ticker. Its "
              "licence and what is owed on it, its storage and its rules of engagement go with "
              "it; the holder of this system must license the new owner, or it stays yours.");
}
