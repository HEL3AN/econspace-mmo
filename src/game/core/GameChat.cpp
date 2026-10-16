// Game -- the chat window (#347): the local channel of the system the ship is in, private
// lines to and from one pilot, and the server's operator. Presentation only; who hears what is
// the server's (sim/Chat.h). Part of the Game class; see Game.cpp.
#include "core/Game.h"

#include "sim/Chat.h"
#include "ui/Theme.h"
#include "ui/Units.h"

#include <cctype>
#include <ctime>
#include <string>

namespace
{
// How many lines the log keeps: an evening of talk, not a transcript.
constexpr size_t CHAT_KEEP = 300;

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

// "HH:MM" in the player's own time.
std::string Clock(long long at)
{
    if (at <= 0)
        return std::string();
    const std::time_t t = (std::time_t)at;
    const std::tm*    tm = std::localtime(&t);
    if (tm == nullptr)
        return std::string();
    char buf[8];
    std::strftime(buf, sizeof(buf), "%H:%M", tm);
    return buf;
}
}  // namespace

void Game::AddChatLine(const Proto::ChatLine& line)
{
    // Whom /r answers: the last pilot to write to this one.
    if (line.channel == Chat::PRIVATE && !line.from.empty() &&
        Lower(line.from) != Lower(pilotName_))
        chatReplyTo_ = line.from;
    chatLines_.push_back(line);
    while (chatLines_.size() > CHAT_KEEP)
        chatLines_.pop_front();
}

void Game::ChatNotice(const std::string& text)
{
    Proto::ChatLine l;
    l.channel = Chat::NOTICE;
    l.text = text;
    l.at = (long long)std::time(nullptr);
    AddChatLine(l);
}

void Game::SendChat(const std::string& typed)
{
    std::string text = Trim(typed);
    if (text.empty())
        return;
    Proto::ChatSay say;
    if (text[0] == '/')
    {
        const size_t      sp = text.find(' ');
        const std::string verb = Lower(text.substr(0, sp));
        const std::string rest = sp == std::string::npos ? std::string() : Trim(text.substr(sp));
        if (verb == "/w" || verb == "/msg" || verb == "/tell")
        {
            const size_t gap = rest.find(' ');
            if (gap == std::string::npos)
            {
                ChatNotice("To talk to one pilot: /w <pilot> <text>");
                return;
            }
            say.to = rest.substr(0, gap);
            text = Trim(rest.substr(gap));
        }
        else if (verb == "/r")
        {
            if (chatReplyTo_.empty())
            {
                ChatNotice("Nobody has written to you to answer");
                return;
            }
            say.to = chatReplyTo_;
            text = rest;
        }
        else
        {
            ChatNotice("/w <pilot> <text> talks to one pilot; /r <text> answers the last who "
                       "did. Anything else is said in local.");
            return;
        }
    }
    if (text.empty())
        return;
    say.text = text;
    clientLink_->Send(Proto::EncodeChatSay(say));
}

// The log on the left with the line being typed under it, and who is in local on the right.
// A line says when, who and what, coloured by what kind of line it is; the log follows the
// newest one until it is scrolled back, and follows again once scrolled to the end.
void Game::DrawChatContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = chatLayout_;

    L.Begin(f);
    if (chatFocusPending_ && !IsKeyPressed(KEY_ENTER) && !IsKeyPressed(KEY_KP_ENTER))
    {
        L.TakeFocus("line");
        chatFocusPending_ = false;
    }
    // Scrolled back by the wheel or the indicator: the log stays where the player put it.
    if ((f.Wheel() > 0.0f && L.Hovered("log")) || (f.Down() && L.Hovered("log~bar")))
        chatFollow_ = false;
    else if (L.ScrollAtEnd("log"))
        chatFollow_ = true;
    if (chatFollow_)
        L.ScrollToEnd("log");

    const std::string me = Lower(pilotName_);
    bool              send = false;
    int               memberClicked = -1;
    L.Row(
        Box().Grow().Gap(t.metrics.gap),
        [&]
        {
            L.Column(
                Box().Grow().Gap(t.metrics.gap),
                [&]
                {
                    L.Scroll(Box().Id("log").Grow().Gap(2.0f),
                             [&]
                             {
                                 if (chatLines_.empty())
                                     L.Text("Nothing said yet. Enter to talk.",
                                            TextStyle::Small().Tint(t.colors.dim));
                                 for (const Proto::ChatLine& l : chatLines_)
                                 {
                                     std::string s = Clock(l.at);
                                     if (!s.empty())
                                         s += "  ";
                                     Color c = t.colors.text;
                                     if (l.channel == Chat::LOCAL)
                                     {
                                         s += l.from + ": " + l.text;
                                         if (l.history)
                                             c = t.colors.dim;
                                         else if (Lower(l.from) == me)
                                             c = t.colors.titleText;
                                     }
                                     else if (l.channel == Chat::PRIVATE)
                                     {
                                         s += Lower(l.from) == me
                                                  ? "to " + l.to + ": " + l.text
                                                  : l.from + " (private): " + l.text;
                                         c = t.colors.accent;
                                     }
                                     else if (l.channel == Chat::SERVER)
                                     {
                                         s += "SERVER: " + l.text;
                                         c = t.colors.warn;
                                     }
                                     else
                                     {
                                         s += l.text;
                                         c = t.colors.dim;
                                     }
                                     L.Text(s, TextStyle::Small().Tint(c).Wrap());
                                 }
                             });
                    L.Row(
                        Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                        [&]
                        {
                            Ui::TextFieldOptions opt;
                            opt.placeholder = "Enter to talk in local; /w pilot to whisper";
                            opt.maxLength = Chat::MAX_CHARS;
                            const Ui::EditResult r = L.TextField("line", chatInput_, opt);
                            send = r.submitted;
                            if (L.Button("send", "Send", false, !Trim(chatInput_).empty()))
                                send = true;
                        });
                });
            // Who is in local: a name clicked starts a private line to them.
            L.Column(Box().Width(Size::Fixed(112.0f)).Height(Size::Grow()).Gap(2.0f),
                     [&]
                     {
                         L.Text(TextFormat("LOCAL  %d", (int)chatMembers_.pilots.size()),
                                TextStyle::Label());
                         L.Scroll(Box().Id("members").Grow().Gap(1.0f),
                                  [&]
                                  {
                                      for (size_t i = 0; i < chatMembers_.pilots.size(); i++)
                                      {
                                          const std::string& p = chatMembers_.pilots[i];
                                          const bool         self = Lower(p) == me;
                                          const bool hover = L.Hovered("member", (uint32_t)i);
                                          L.Row(Box()
                                                    .Id("member", (uint32_t)i)
                                                    .GrowX()
                                                    .Pad(4.0f, 1.0f)
                                                    .Fill(hover && !self ? t.colors.hover
                                                                         : Color{ 0, 0, 0, 0 }),
                                                [&]
                                                {
                                                    L.Text(p, TextStyle::Small().Tint(
                                                                  self ? t.colors.titleText
                                                                       : t.colors.text));
                                                });
                                          if (!self && L.Clicked("member", (uint32_t)i))
                                              memberClicked = (int)i;
                                          if (!self)
                                              L.Tooltip("member", "Click to talk to " + p,
                                                        (uint32_t)i);
                                      }
                                  });
                     });
        });
    L.End();
    L.Draw();

    if (send)
    {
        SendChat(chatInput_);
        chatInput_.clear();
        chatFollow_ = true;
    }
    if (memberClicked >= 0)
    {
        chatInput_ = "/w " + chatMembers_.pilots[(size_t)memberClicked] + " ";
        desk_.KeyboardFocus().Take(f.Id(), "line");
    }
}
