#pragma once

#include <string>

// Writing a tuned look back into data/archetypes.json without reformatting the file.
//
// The gallery (#118) exists so a look can be judged by eye, and judging is wasted if the
// numbers then have to be copied out by hand. The obvious implementation -- parse, edit,
// dump -- rewrites the whole file: nlohmann keeps neither the blank-line grouping the
// registry is written with nor the inline `"world": { ... }` objects, so a two-character
// change lands as a two-hundred-line diff. That happened once already and was reverted.
//
// So this edits the text instead. It replaces one value in place and leaves every other
// byte alone, which is what makes the result reviewable as what it is.
namespace ArchetypeEdit
{

// Replaces the value of `key` in the archetype object whose "id" is `id` with `valueJson`,
// written verbatim -- `"#"`, `[253, 249, 0, 255]`, `600`. The caller formats the value
// because only the caller knows what the key means; nothing here parses JSON.
//
// The key is matched only at the archetype's own level, so `size` is the archetype's size
// and never one nested inside `world` or `components`. A key the object does not have yet
// is added after the last member.
//
// Returns false and leaves `text` untouched when there is no such archetype or the text is
// not shaped like the file -- a caller that gets false should refuse to save rather than
// write something it does not understand.
bool SetField(std::string& text, const std::string& id, const std::string& key,
              const std::string& valueJson);

}  // namespace ArchetypeEdit
