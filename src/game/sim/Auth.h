#pragma once

#include "net/Transport.h"

#include <string>

// What an account uses to prove it is itself (#106).
//
// The goal is narrow and worth stating so nobody assumes more: a stranger who knows that
// someone plays as `alice` must not be able to fly her ship. It is not "safe against
// somebody reading the network" — there is no transport encryption — and it is not "safe
// against somebody holding the account files", though the salt and the iteration count
// make that expensive rather than instant.
//
// The secret never travels after registration. The server sends a nonce, the client
// answers with a proof computed from it, and a listener who copies that proof finds it
// useless on the next connection because the nonce is different.
namespace Auth
{

// Hex SHA-256 of the bytes given.
std::string Sha256Hex(const std::string& data);

// A salt for a new account, as hex. Not a secret: it exists so that one table of guesses
// cannot cover every account on the server at once.
std::string MakeSalt();

// A nonce for one connection, as hex. Single use — reusing one would make a captured
// proof replayable, which is the whole thing this arrangement is for.
std::string MakeNonce();

// What the server keeps for an account: the secret, salted and stretched. Computed by the
// client too, on every login, because it is what the proof is made from.
std::string Stored(const std::string& salt, const std::string& secret);

// What the client sends to show it knows the secret, good for this nonce only.
std::string Proof(const std::string& nonce, const std::string& stored);

// The client half of the handshake, on an already-connected transport: says who we are,
// waits for the challenge, answers it. One implementation for the game client and the
// agent, because two would drift and the drift would look like a wrong password.
//
// Returns false with a reason only for what can be decided here -- no challenge arrived,
// or the server refused before sending one. Whether the proof was *accepted* is not known
// yet: the server answers by continuing, or by sending Bye, which both clients already
// surface. Nothing is polled after the answer is sent, so the layout and snapshot that
// follow are left for the caller.
bool ClientHandshake(ITransport& conn, const std::string& account, const std::string& secret,
                     double timeoutSeconds, std::string& error);

}  // namespace Auth
