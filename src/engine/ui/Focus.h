#pragma once

#include <string>
#include <string_view>
#include <vector>

// Who the keyboard belongs to (#297): at most one text field in the whole client at a time.
//
// The mouse has an owner chosen afresh every frame by where the cursor is; the keyboard
// cannot work that way, because typing does not point at anything. So a field *takes* the
// focus when it is clicked (or when the game hands it over, as N does on the map), keeps it
// while it is drawn, and loses it to Enter, Esc, a press anywhere else, its window closing,
// or simply not being drawn any more. While a field holds it, letters are text, not hotkeys:
// the game asks Taken() before reading a key.
//
// No raylib here: the desk feeds it, the layout asks it, and a test holds it.
namespace Ui
{

class Focus
{
public:
    // Why a field last lost the focus. A number field keeps what was typed on Elsewhere and
    // Submit and throws it away on Cancel.
    enum class Blur
    {
        None,
        Submit,     // Enter
        Cancel,     // Esc
        Elsewhere,  // a press somewhere else, its window closed, or it stopped being drawn
    };

    // `owner` is the window's id on the desk, `field` the field's id inside it.
    void               Take(std::string_view owner, std::string_view field);
    bool               Holds(std::string_view owner, std::string_view field) const;
    bool               Taken() const { return !owner_.empty(); }
    const std::string& Owner() const { return owner_; }

    void Release(Blur why);
    void ReleaseOwner(std::string_view owner, Blur why);  // its window is closing

    // A field that holds the focus says so every frame it is drawn; EndFrame lets go of a
    // focus nobody claimed, so a field that vanished does not keep eating the keyboard. The
    // desk calls it at the start of each frame, for the frame before.
    void Seen(std::string_view owner, std::string_view field);
    void EndFrame();

    // Why this field lost the focus this frame, once: asking clears the answer.
    Blur TakeBlur(std::string_view owner, std::string_view field);

private:
    struct Lost
    {
        std::string owner, field;
        Blur        why = Blur::None;
    };

    std::string       owner_, field_;
    bool              seen_ = false;
    std::vector<Lost> lost_;
};

// What was typed this frame, for the field that holds the focus.
struct KeyInput
{
    std::string chars;  // printable characters, in order
    int         backspace = 0;
    bool        enter = false;
};

// What an edit did.
struct EditResult
{
    bool changed = false;
    bool submitted = false;  // Enter
};

// Applies one frame's typing to `text`: printable ASCII is appended up to `maxLength`, each
// backspace removes one character, and Enter submits. `accept` filters characters (digits
// only, for a number); null takes every printable one. The rule a text field follows,
// without a window.
EditResult EditText(std::string& text, const KeyInput& in, size_t maxLength,
                    bool (*accept)(char) = nullptr);

// Reads this frame's typing from raylib. Consumes the character queue, so only the field
// that holds the focus calls it.
KeyInput ReadKeys();

}  // namespace Ui
