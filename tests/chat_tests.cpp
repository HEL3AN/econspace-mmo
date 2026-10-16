#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "sim/Chat.h"
#include "sim/Protocol.h"

// Local chat, private lines and the operator's word (#347): who hears a line, what a line may
// hold, and how fast a pilot may talk.

namespace
{

std::vector<int> Recipients(const std::vector<Chat::Delivery>& out)
{
    std::vector<int> ids;
    for (const Chat::Delivery& d : out)
        ids.push_back(d.to);
    return ids;
}

Proto::ChatSay Local(const std::string& text)
{
    Proto::ChatSay s;
    s.text = text;
    return s;
}

Proto::ChatSay To(const std::string& who, const std::string& text)
{
    Proto::ChatSay s;
    s.to = who;
    s.text = text;
    return s;
}

const Chat::Pilot              ANNA{ 1, "anna", "sol" };
const Chat::Pilot              BORIS{ 2, "boris", "sol" };
const Chat::Pilot              CARA{ 3, "cara", "vega" };
const std::vector<Chat::Pilot> ONLINE{ ANNA, BORIS, CARA };

}  // namespace

TEST_CASE("a local line is heard in the sender's system and nowhere else (#347)")
{
    Chat::Hub                         hub;
    const std::vector<Chat::Delivery> out = hub.Say(ANNA, Local("hello"), ONLINE, 1000.0);
    CHECK(Recipients(out) == std::vector<int>{ 1, 2 });
    for (const Chat::Delivery& d : out)
    {
        CHECK(d.line.channel == std::string(Chat::LOCAL));
        CHECK(d.line.from == "anna");
        CHECK(d.line.system == "sol");
        CHECK(d.line.text == "hello");
        CHECK(d.line.at == 1000);
        CHECK_FALSE(d.line.history);
    }
    // Cara in another system hears nothing, and Vega has nothing to tell an arrival.
    CHECK(hub.History("vega", 1000.0).empty());
    REQUIRE(hub.History("sol", 1000.0).size() == 1);
    CHECK(hub.History("sol", 1000.0)[0].history);
}

TEST_CASE("a private line goes to the pilot named and back to the sender (#347)")
{
    Chat::Hub                   hub;
    std::vector<Chat::Delivery> out = hub.Say(ANNA, To("Cara", "psst"), ONLINE, 10.0);
    CHECK(Recipients(out) == std::vector<int>{ 3, 1 });
    CHECK(out[0].line.channel == std::string(Chat::PRIVATE));
    CHECK(out[0].line.to == "cara");          // names are compared in lower case, as accounts are
    CHECK(hub.History("sol", 10.0).empty());  // not kept: it was not said in a system

    out = hub.Say(ANNA, To("dmitri", "anyone?"), ONLINE, 11.0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].to == 1);
    CHECK(out[0].line.channel == std::string(Chat::NOTICE));
    CHECK(out[0].line.text.find("dmitri") != std::string::npos);
}

TEST_CASE("the operator's line reaches everyone online (#347)")
{
    Chat::Hub                         hub;
    const std::vector<Chat::Delivery> out = hub.Broadcast("restart in 5 min", ONLINE, 5.0);
    CHECK(Recipients(out) == std::vector<int>{ 1, 2, 3 });
    CHECK(out[0].line.channel == std::string(Chat::SERVER));
    CHECK(out[0].line.from.empty());
}

TEST_CASE("a pilot who talks too fast is told to wait, and may talk again later (#347)")
{
    Chat::Limits limits;
    limits.burst = 3;
    limits.refill = 2.0;
    Chat::Hub hub(limits);
    for (int i = 0; i < 3; i++)
        CHECK(hub.Say(ANNA, Local("line"), ONLINE, 100.0).size() == 2);
    std::vector<Chat::Delivery> out = hub.Say(ANNA, Local("one more"), ONLINE, 100.5);
    REQUIRE(out.size() == 1);
    CHECK(out[0].to == 1);
    CHECK(out[0].line.channel == std::string(Chat::NOTICE));
    // Somebody else is not slowed down by Anna.
    CHECK(hub.Say(BORIS, Local("hi"), ONLINE, 100.5).size() == 2);
    // One line back every two seconds.
    CHECK(hub.Say(ANNA, Local("again"), ONLINE, 102.0).size() == 2);
    CHECK(hub.Say(ANNA, Local("and again"), ONLINE, 102.1).size() == 1);
    // A line that comes to nothing costs nothing and says nothing.
    CHECK(hub.Say(BORIS, Local(" \t\x01 "), ONLINE, 103.0).empty());
}

TEST_CASE("a muted pilot is heard by nobody until the mute ends (#347)")
{
    Chat::Hub hub;
    hub.Mute("Anna", 200.0);
    std::vector<Chat::Delivery> out = hub.Say(ANNA, Local("hello?"), ONLINE, 100.0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].line.channel == std::string(Chat::NOTICE));
    CHECK(hub.Say(ANNA, To("boris", "hello?"), ONLINE, 100.0).size() == 1);
    CHECK(hub.Say(ANNA, Local("now?"), ONLINE, 201.0).size() == 2);

    hub.Mute("anna", 0.0);  // until unmuted
    CHECK(hub.Muted("anna", 1e9));
    CHECK(hub.Unmute("ANNA"));
    CHECK_FALSE(hub.Unmute("anna"));
    CHECK(hub.Say(ANNA, Local("free"), ONLINE, 300.0).size() == 2);
}

TEST_CASE("lines are cleaned: control characters, broken UTF-8, length (#347)")
{
    CHECK(Chat::Sanitize("  hello   world  ") == "hello world");
    CHECK(Chat::Sanitize("a\tb\r\nc") == "a b c");
    CHECK(Chat::Sanitize("bell\x07 and \x1b[31mred") == "bell and [31mred");
    CHECK(Chat::Sanitize("del\x7f") == "del");
    // UTF-8 is kept when it is valid...
    CHECK(Chat::Sanitize("\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82") ==
          "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82");
    CHECK(Chat::Sanitize("\xf0\x9f\x9a\x80") == "\xf0\x9f\x9a\x80");
    // ...and a broken sequence, an overlong form or a surrogate is dropped, not guessed at.
    CHECK(Chat::Sanitize("a\xc3") == "a");
    CHECK(Chat::Sanitize("a\xff"
                         "b") == "ab");
    CHECK(Chat::Sanitize("\xc0\xaf") == "");  // '/' spelt overlong
    CHECK(Chat::Sanitize("\xed\xa0\x80"
                         "x") == "x");  // a lone surrogate
    // C1 controls, a direction override and zero-width characters are taken out.
    CHECK(Chat::Sanitize("a\xc2\x85"
                         "b") == "a b");  // NEL is a line break
    CHECK(Chat::Sanitize("a\xc2\x9b"
                         "b") == "ab");  // CSI
    CHECK(Chat::Sanitize("x\xe2\x80\xae"
                         "gpj") == "xgpj");                 // U+202E
    CHECK(Chat::Sanitize("ad\xe2\x80\x8bmin") == "admin");  // U+200B
    // The cap counts characters, not bytes, and never cuts one in half.
    CHECK(Chat::CountChars(Chat::Sanitize(std::string(500, 'x'))) == Chat::MAX_CHARS);
    std::string cyr;
    for (int i = 0; i < 300; i++)
        cyr += "\xd0\xb6";
    const std::string cut = Chat::Sanitize(cyr);
    CHECK(Chat::CountChars(cut) == Chat::MAX_CHARS);
    CHECK(cut.size() == Chat::MAX_CHARS * 2);
    CHECK(Chat::Sanitize("abcdef", 3) == "abc");
    CHECK(Chat::Sanitize("ab   cd", 3) == "ab");  // no trailing space left by the cut
}

TEST_CASE("a pilot arriving is told who is there and what was said lately (#347)")
{
    Chat::Limits limits;
    limits.historyAge = 600.0;
    Chat::Hub hub(limits);

    // Anna comes in alone: the members, and no history.
    std::vector<Chat::Presence> p = hub.Track({ ANNA }, 0.0);
    REQUIRE(p.size() == 1);
    CHECK(p[0].to == 1);
    CHECK(p[0].members.system == "sol");
    CHECK(p[0].members.pilots == std::vector<std::string>{ "anna" });
    CHECK(p[0].history.empty());
    CHECK(hub.Track({ ANNA }, 1.0).empty());  // nothing changed, nothing sent

    hub.Say(ANNA, Local("old news"), { ANNA }, 10.0);
    hub.Say(ANNA, Local("news"), { ANNA }, 500.0);

    // Boris arrives: he gets the members and the history, Anna the members only, and Cara in
    // Vega -- a system nobody else came to -- nothing beyond her own arrival.
    p = hub.Track({ ANNA, BORIS, CARA }, 700.0);
    REQUIRE(p.size() == 3);
    CHECK(p[0].to == 1);
    CHECK(p[0].members.pilots == std::vector<std::string>{ "anna", "boris" });
    CHECK(p[0].history.empty());
    CHECK(p[1].to == 2);
    REQUIRE(p[1].history.size() == 1);  // "old news" is older than ten minutes
    CHECK(p[1].history[0].text == "news");
    CHECK(p[1].history[0].history);
    CHECK(p[2].to == 3);
    CHECK(p[2].members.pilots == std::vector<std::string>{ "cara" });

    // Boris jumps to Vega: both systems' members change.
    Chat::Pilot borisInVega = BORIS;
    borisInVega.system = "vega";
    p = hub.Track({ ANNA, borisInVega, CARA }, 710.0);
    REQUIRE(p.size() == 3);
    CHECK(p[0].members.pilots == std::vector<std::string>{ "anna" });
    CHECK(p[1].members.system == "vega");
    CHECK(p[1].members.pilots == std::vector<std::string>{ "boris", "cara" });
    CHECK(p[2].members.pilots == std::vector<std::string>{ "boris", "cara" });

    // Expiring forgets the history that is too old to tell.
    hub.Expire(2000.0);
    CHECK(hub.History("sol", 2000.0).empty());
}

TEST_CASE("chat messages round-trip, and a say from anyone is read safely (#347)")
{
    Proto::ChatSay say;
    say.to = "boris";
    say.text = "hi";
    Proto::ChatSay back;
    REQUIRE(Proto::DecodeChatSay(Proto::EncodeChatSay(say), back));
    CHECK(back.to == "boris");
    CHECK(back.text == "hi");
    CHECK(Proto::MessageType(Proto::EncodeChatSay(say)) == "say");

    Proto::ChatLines lines;
    Proto::ChatLine  l;
    l.channel = Chat::LOCAL;
    l.from = "anna";
    l.system = "sol";
    l.text = "o7";
    l.at = 1760000000;
    l.history = true;
    lines.lines.push_back(l);
    Proto::ChatLines lback;
    REQUIRE(Proto::DecodeChatLines(Proto::EncodeChatLines(lines), lback));
    REQUIRE(lback.lines.size() == 1);
    CHECK(lback.lines[0].from == "anna");
    CHECK(lback.lines[0].system == "sol");
    CHECK(lback.lines[0].at == 1760000000);
    CHECK(lback.lines[0].history);

    Proto::ChatMembers m;
    m.system = "sol";
    m.pilots = { "anna", "boris" };
    Proto::ChatMembers mback;
    REQUIRE(Proto::DecodeChatMembers(Proto::EncodeChatMembers(m), mback));
    CHECK(mback.pilots == m.pilots);

    // Fields of the wrong type read as nothing, not as an exception.
    const std::string odd = "{\"v\":" + std::to_string(Proto::PROTO_VERSION) +
                            ",\"t\":\"say\",\"to\":7,\"text\":[1,2]}";
    Proto::ChatSay    oddBack;
    REQUIRE(Proto::DecodeChatSay(odd, oddBack));
    CHECK(oddBack.to.empty());
    CHECK(oddBack.text.empty());
}
