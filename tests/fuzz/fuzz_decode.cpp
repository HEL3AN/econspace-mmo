// Fuzz target 2 (#518): every decoder the server feeds with bytes a peer sent.
//
// The first byte picks one, so a corpus entry exercises one decoder and the fuzzer learns
// which prefix leads where. `Decode*` are permissive per field by design (CLAUDE.md), which
// is what makes them worth fuzzing: they must return false or a value, never throw, never
// read out of bounds, never loop.
//
// The server's own set comes first; the client's follows, because a client reads what a
// server sends it and a hostile server (or a man in the middle -- there is no transport
// encryption) is the same kind of stranger.
#include "sim/Protocol.h"

#include <cstdint>
#include <string>

namespace
{
template <typename T>
void Try(bool (*decode)(const std::string&, T&), const std::string& s)
{
    T out;
    (void)decode(s, out);
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
        return 0;
    const unsigned    which = data[0];
    const std::string s((const char*)data + 1, size - 1);

    switch (which % 22)
    {
        // What the server decodes.
        case 0: Try(Proto::DecodeCommand, s); break;
        case 1: Try(Proto::DecodeHello, s); break;
        case 2: Try(Proto::DecodeAuth, s); break;
        case 3: Try(Proto::DecodeChooseStart, s); break;
        case 4: Try(Proto::DecodeAck, s); break;
        case 5: Try(Proto::DecodeChatSay, s); break;
        case 6: Try(Proto::DecodeCorpAct, s); break;
        case 7:
            (void)Proto::MessageType(s);
            (void)Proto::MessageVersion(s);
            break;
        // What the client decodes.
        case 8: Try(Proto::DecodeBye, s); break;
        case 9: Try(Proto::DecodeStarts, s); break;
        case 10: Try(Proto::DecodeChallenge, s); break;
        case 11: Try(Proto::DecodeSnapshot, s); break;
        case 12: Try(Proto::DecodeLayout, s); break;
        case 13: Try(Proto::DecodeLayoutDelta, s); break;
        case 14: Try(Proto::DecodeDesignDef, s); break;
        case 15: Try(Proto::DecodeGalaxy, s); break;
        case 16: Try(Proto::DecodeFactions, s); break;
        case 17: Try(Proto::DecodeChatLines, s); break;
        case 18: Try(Proto::DecodeChatMembers, s); break;
        case 19: Try(Proto::DecodeCorpView, s); break;
        case 20: Try(Proto::DecodeUniverse, s); break;
        case 21:
        {
            Proto::SnapshotReceiver rx;
            Proto::Snapshot         snap;
            (void)rx.Decode(s, snap);
            (void)rx.Decode(s, snap);  // a delta is read against what the first one left
            break;
        }
    }
    return 0;
}
