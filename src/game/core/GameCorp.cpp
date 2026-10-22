// Game -- the corporation (#347): the window where a pilot founds one, answers an invite,
// sees who is in theirs and, as its CEO or a director, runs it; and what the rest of the
// client knows of who is in which -- a ticker before a name, a corp mate's ship counted a
// friend's. Presentation only: who may do what is the server's (sim/Corporation.h), and the
// answer to every verb is a notice in chat. Part of the Game class; see Game.cpp.
#include "core/Game.h"

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
    return (role == "ceo" || role == "director") && corpView_.Has(Lower(owner));
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
    else if (verb == "join" || verb == "decline")
        act.ticker = Word(rest);
    else if (verb == "role")
        act.pilot = Word(rest, &act.role);
    else if (verb == "tax" || verb == "docking")
        act.value = Word(rest);
    else if (verb != "leave")
    {
        ChatNotice("/corp create <ticker> <name> | invite <pilot> | join <ticker> | decline "
                   "<ticker> | leave | kick <pilot> | role <pilot> ceo|director|member | tax "
                   "<percent>|standard | docking own|allies|all; /c <text> talks to the "
                   "corporation");
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
                // Its licences to build in others' space (#431 slice 6), the rent from the
                // treasury.
                const double now = worldClock_.Now(GetTime());
                for (const Proto::LicenceView& l : corpView_.licences)
                {
                    const bool ok = l.state == "licensed";
                    L.Field("Licence in " + MapSystemName(l.system),
                            l.state == "licensed" ? TextFormat("%s, %.0f h paid", l.holder.c_str(),
                                                               (l.until - now) / 3600.0)
                            : l.state == "unpaid"
                                ? TextFormat("%s, UNPAID: lapses in %.0f h", l.holder.c_str(),
                                             (l.until - now) / 3600.0)
                                : TextFormat("%s, LAPSED: trespassing", l.holder.c_str()),
                            ok ? t.colors.text : t.colors.bad);
                }
                DrawLicenceHere(L);
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

void Game::DrawLicenceHere(Ui::Layout& L)
{
    using Ui::TextStyle;
    const Ui::Theme&          t = Ui::CurrentTheme();
    const Proto::LicenceView& l = snapshot_.player.licence;
    if (l.holder.empty())
        return;  // nobody here asks for one
    const double now = worldClock_.Now(GetTime());
    L.Divider();
    L.Text("LICENCE TO BUILD HERE", TextStyle::Label());
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
