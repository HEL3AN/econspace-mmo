#pragma once

#include <string>

// Why the client could not get into the game, or why it left it, in a window that stays
// until the player closes it (#400).
//
// The reason used to go to stderr and the process ended. Started from a shortcut or a .bat,
// that is a window flashing open and vanishing: a friend whose secret is wrong, who is not on
// the allowlist or whose copy is a release behind sees nothing at all. Every way of not
// getting in -- nothing answering, the server's Bye before or after the login, the connection
// dropping in flight -- ends here instead, and the line still goes to stderr.
namespace ErrorScreen
{

// What the screen says: a short title ("Could not connect") and the reason in full, which
// may run to several sentences and carry a link (a Bye naming the release to get).
struct Message
{
    std::string title;
    std::string text;
};

// The first http(s) link in `text`, without the punctuation that ends a sentence; empty if
// there is none. Shown on a line of its own with a button that copies only it.
std::string FindLink(const std::string& text);

// Shows `message` until the player closes it: the Close button, Esc, or the window's own
// close button. Copy puts the whole message on the clipboard. Uses the window that is open
// -- the game's, with its theme loaded -- or opens one for itself, with the player's theme
// from data/, and closes it again. Writes the message to stderr first.
//
// `shotPath`: a scripted run (--shot) saves the screen there after `shotFrames` frames and
// returns, as the game itself does (#293).
void Show(const Message& message, const std::string& shotPath = std::string(), int shotFrames = 3);

}  // namespace ErrorScreen
