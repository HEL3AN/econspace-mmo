// Writes the seed corpus for the fuzz targets (#518): real messages, made by the encoders,
// so the fuzzer starts from what the wire looks like and mutates outwards instead of
// spending its first hours rediscovering `{"t":"cmd","v":...}`.
//
//   fuzz_seeds <dir>   ->  <dir>/decode/*, <dir>/frames/*, <dir>/login/*
//
// Not a fuzz target: an ordinary executable, built with them (ECON_FUZZ) and run by
// .clusterfuzzlite/build.sh, which zips each folder next to its target.
#include "sim/Auth.h"
#include "sim/Protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

void Put(const fs::path& dir, int n, const std::string& bytes)
{
    fs::create_directories(dir);
    std::ofstream out(dir / ("seed" + std::to_string(n)), std::ios::binary);
    out.write(bytes.data(), (std::streamsize)bytes.size());
}

// The wire's framing: four bytes of length, network byte order, then the message.
std::string Frame(const std::string& msg)
{
    const uint32_t n = (uint32_t)msg.size();
    std::string    f;
    f.push_back((char)(n >> 24));
    f.push_back((char)(n >> 16));
    f.push_back((char)(n >> 8));
    f.push_back((char)n);
    return f + msg;
}

// fuzz_frames.cpp reads its input as chunks, each preceded by its length - 1.
std::string Chunked(const std::string& bytes, size_t chunk)
{
    std::string out;
    for (size_t at = 0; at < bytes.size(); at += chunk)
    {
        const size_t len = std::min(chunk, bytes.size() - at);
        out.push_back((char)(len - 1));
        out += bytes.substr(at, len);
    }
    return out;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: fuzz_seeds <dir>\n");
        return 2;
    }
    const fs::path root = argv[1];

    Proto::Hello hello;
    hello.account = "pilot";
    hello.build = "0.1";
    hello.data = { { "archetypes.json", "0123456789abcdef" } };

    const std::string stored = Auth::Stored("salt", "hunter2");
    Proto::Auth       auth;
    auth.stored = stored;
    auth.proof = Auth::Proof("00112233445566778899aabbccddeeff", stored);

    Proto::Command thrust;
    thrust.seq = 7;
    thrust.thrust = true;
    thrust.turn = 0.5f;
    Proto::Command nav;
    nav.seq = 8;
    nav.navMode = 2;
    nav.navTarget = { 1200.0f, -3400.0f };
    nav.navViaSet = true;
    Proto::Command trade;
    trade.seq = 9;
    trade.sellType = 1;
    trade.sellAmount = 10;
    trade.buyType = 2;
    trade.buyAmount = 5;
    trade.moveFrom = "hold";
    trade.moveTo = "storage";
    trade.moveType = 1;
    trade.moveAmount = 3;
    Proto::Command dock;
    dock.seq = 10;
    dock.dock = true;
    dock.targetId = 42;
    dock.jumpGateId = 3;

    Proto::ChooseStart start;
    start.system = "sol";
    Proto::Ack ack;
    ack.snapshot = 12;
    Proto::ChatSay say;
    say.to = "friend";
    say.text = "hello there";
    Proto::ChatSay sayCorp;
    sayCorp.channel = "corp";
    sayCorp.text = "o7";
    Proto::CorpAct found;
    found.verb = "found";
    found.name = "Deep Space Haulers";
    found.ticker = "DSH";
    Proto::CorpAct invite;
    invite.verb = "invite";
    invite.pilot = "friend";
    invite.role = "officer";
    Proto::CorpAct patrol;
    patrol.verb = "patrol";
    patrol.system = "sol";
    patrol.design = "frigate";
    patrol.engage = "enemies";
    patrol.threshold = "-2";
    patrol.allow = "none";
    patrol.value = "3";

    Proto::Bye bye;
    bye.reason = "Signed in from somewhere else";
    Proto::Challenge chal;
    chal.nonce = "00112233445566778899aabbccddeeff";
    chal.salt = "salt";
    Proto::Starts      starts;
    Proto::StartOption opt;
    opt.system = "sol";
    opt.name = "Sol";
    starts.options.push_back(opt);

    // Order matches fuzz_decode.cpp's switch.
    const std::vector<std::vector<std::string>> byDecoder = {
        { Proto::EncodeCommand(thrust), Proto::EncodeCommand(nav), Proto::EncodeCommand(trade),
          Proto::EncodeCommand(dock) },
        { Proto::EncodeHello(hello) },
        { Proto::EncodeAuth(auth) },
        { Proto::EncodeChooseStart(start) },
        { Proto::EncodeAck(ack) },
        { Proto::EncodeChatSay(say), Proto::EncodeChatSay(sayCorp) },
        { Proto::EncodeCorpAct(found), Proto::EncodeCorpAct(invite), Proto::EncodeCorpAct(patrol) },
        { Proto::EncodeCommand(thrust) },
        { Proto::EncodeBye(bye) },
        { Proto::EncodeStarts(starts) },
        { Proto::EncodeChallenge(chal) },
        { Proto::EncodeSnapshot(Proto::Snapshot{}) },
        { Proto::EncodeLayout(Proto::SystemLayout{}) },
        { Proto::EncodeLayoutDelta(Proto::LayoutDelta{}) },
        { Proto::EncodeDesignDef(Proto::DesignDef{}) },
        { Proto::EncodeGalaxy(Proto::GalaxyState{}) },
        { Proto::EncodeFactions(Proto::FactionList{}) },
        { Proto::EncodeChatLines(Proto::ChatLines{}) },
        { Proto::EncodeChatMembers(Proto::ChatMembers{}) },
        { Proto::EncodeCorpView(Proto::CorpView{}) },
        { Proto::EncodeUniverse(WorldLoader::Universe{}) },
        { Proto::EncodeSnapshot(Proto::Snapshot{}) },
    };

    int n = 0;
    for (size_t d = 0; d < byDecoder.size(); ++d)
        for (const std::string& m : byDecoder[d])
            Put(root / "decode", n++, std::string(1, (char)d) + m);

    // Frames: back-to-back messages in chunks of 256 and of 7, and a header claiming four
    // gigabytes.
    const std::string stream = Frame(Proto::EncodeHello(hello)) + Frame(Proto::EncodeAuth(auth)) +
                               Frame(Proto::EncodeCommand(thrust));
    n = 0;
    Put(root / "frames", n++, Chunked(stream, 256));
    Put(root / "frames", n++, Chunked(stream, 7));
    Put(root / "frames", n++, Chunked(std::string("\xff\xff\xff\xff", 4), 4));

    // Login: a two-byte length, the hello, then the answer.
    const std::string h = Proto::EncodeHello(hello);
    Put(root / "login", 0,
        std::string(1, (char)(h.size() >> 8)) + std::string(1, (char)h.size()) + h +
            Proto::EncodeAuth(auth));
    return 0;
}
