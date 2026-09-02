// The right-hand panel: the header line and the property editor for the selected
// object, field by field. One translation unit of Editor (#17).
//
// Every row edits systemJson_ directly, which is the editor's source of truth -- the
// entities are only what that JSON looks like.

#include "Editor.h"

#include "core/Faction.h"
#include "ui/UiTheme.h"
#include "ui/Button.h"
#include "raymath.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using nlohmann::json;

void Editor::DrawHud()
{
    Ui::Text("WORLD EDITOR", 16, 14, 26, Ui::ACCENT);

    // Info and hints in the top-left corner.
    if (mode_ == Mode::Gallery)
    {
        Ui::Text(TextFormat("GALLERY   archetypes: %d", (int)Archetypes::All().size()), 16, 50, 16,
                 Ui::TEXT);
        Ui::Text("click: select  ·  wheel: scroll  ·  F2: backend  ·  edit on the right", 16, 74,
                 13, Ui::TEXT_DIM);
    }
    else if (mode_ == Mode::Galaxy)
    {
        Ui::Text(TextFormat("GALAXY   systems: %d", (int)(universeJson_.contains("systems")
                                                              ? universeJson_["systems"].size()
                                                              : 0)),
                 16, 50, 16, Ui::TEXT);
        Ui::Text("drag: move  ·  click: select  ·  double-click: open system", 16, 74, 13,
                 Ui::TEXT_DIM);
    }
    else if (!universe_.systems.empty())
    {
        const WorldLoader::SystemInfo& s = universe_.systems[currentSystem_];
        Ui::Text(TextFormat("System:  %s  (%s)", s.name.c_str(), s.id.c_str()), 16, 50, 16,
                 Ui::TEXT);
        Ui::Text(TextFormat("Objects: %d", (int)entities_.size()), 16, 72, 14, Ui::TEXT_DIM);

        if (!placeArchetype_.empty())
        {
            const Archetype* pa = Archetypes::Find(placeArchetype_);
            Ui::Text(TextFormat("Placing: %s", pa ? pa->name.c_str() : placeArchetype_.c_str()), 16,
                     96, 14, Ui::ACCENT);
            Ui::Text("click in space to place  ·  Esc / RMB to cancel", 16, 114, 13, Ui::TEXT_DIM);
        }
        else if (selected_ >= 0)
            Ui::Text("drag: move  ·  Del: delete  ·  edit on the right", 16, 96, 13, Ui::TEXT_DIM);
        else
            Ui::Text("click: select  ·  drag empty: pan  ·  wheel: zoom", 16, 96, 13, Ui::TEXT_DIM);
    }

    // Mode toggle button (System / Galaxy). The gallery is not a place in the world, so
    // it does not sit on this toggle; it has its own button beside it.
    if (mode_ != Mode::Gallery)
    {
        Rectangle mb = ModeButtonRect();
        bool      overMb = CheckCollisionPointRec(GetMousePosition(), mb);
        DrawRectangleRec(mb, overMb ? Fade(Ui::ACCENT, 0.2f) : Ui::TITLE_BG);
        DrawRectangleLinesEx(mb, 1.0f, Ui::PANEL_BORDER);
        Ui::Text((mode_ == Mode::Galaxy) ? "→ System view" : "→ Galaxy map", (int)mb.x + 10,
                 (int)mb.y + 8, 14, Ui::TEXT);
        if (overMb && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            EnterGalaxyMode(mode_ != Mode::Galaxy);
    }

    Rectangle gb = GalleryButtonRect();
    bool      overGb = CheckCollisionPointRec(GetMousePosition(), gb);
    DrawRectangleRec(gb, overGb ? Fade(Ui::ACCENT, 0.2f) : Ui::TITLE_BG);
    DrawRectangleLinesEx(gb, 1.0f, (mode_ == Mode::Gallery) ? Ui::ACCENT : Ui::PANEL_BORDER);
    Ui::Text((mode_ == Mode::Gallery) ? "→ Back" : "→ Gallery", (int)gb.x + 10, (int)gb.y + 8, 14,
             (mode_ == Mode::Gallery) ? Ui::ACCENT : Ui::TEXT);
    if (overGb && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        EnterGalleryMode(mode_ != Mode::Gallery);

    // Save button (top center) with an indicator of unsaved edits.
    bool d = dirty_;
    if (mode_ == Mode::Galaxy)
        d = universeDirty_;
    else if (mode_ == Mode::Gallery)
        d = archetypesDirty_;
    Rectangle sb = SaveButtonRect();
    bool      overSb = CheckCollisionPointRec(GetMousePosition(), sb);
    DrawRectangleRec(sb, overSb ? Fade(Ui::ACCENT, 0.2f) : Ui::TITLE_BG);
    DrawRectangleLinesEx(sb, 1.0f, d ? Ui::ACCENT : Ui::PANEL_BORDER);
    Ui::Text(d ? "Save *  [Ctrl+S]" : "Saved   [Ctrl+S]", (int)sb.x + 12, (int)sb.y + 8, 14,
             d ? Ui::ACCENT : Ui::TEXT_DIM);
    if (overSb && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (mode_ == Mode::Galaxy)
            SaveUniverse();
        else if (mode_ == Mode::Gallery)
            SaveArchetypes();
        else
            SaveCurrentSystem();
    }
}

Rectangle Editor::PanelRect() const
{
    return { (float)(screenWidth_ - 300), 0.0f, 300.0f, (float)screenHeight_ };
}

// The field's current value as a string (for display and buffer initialization).
static std::string FieldValueStr(const nlohmann::json& obj, const char* key, bool numeric,
                                 bool asInt)
{
    if (!obj.contains(key))
        return "";
    const nlohmann::json& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number())
        return numeric && asInt ? std::to_string((long long)llround(v.get<double>()))
                                : TextFormat("%g", v.get<double>());
    return "";
}

// A field with manual entry: click to focus, type on the keyboard. For numbers the
// value is parsed and written "live"; empty/incomplete input doesn't overwrite the old.
bool Editor::FieldRow(Rectangle r, const char* label, nlohmann::json& obj, const char* key,
                      bool numeric, bool asInt, double step)
{
    Ui::Text(label, (int)r.x, (int)r.y + 5, 13, Ui::TEXT_DIM);
    // Numbers have -/+ buttons on the right, so the box is narrower.
    float     boxW = numeric ? (r.width - 110 - 62) : (r.width - 110);
    Rectangle box{ r.x + 110, r.y, boxW, 24 };

    // Focus on click (unless covered by an open dropdown).
    if (!anyDropdownOpen_ && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(GetMousePosition(), box))
    {
        activeField_ = key;
        editBuffer_ = FieldValueStr(obj, key, numeric, asInt);
        caretPos_ = (int)editBuffer_.size();
    }

    bool        active = (activeField_ == key);
    std::string disp = active ? editBuffer_ : FieldValueStr(obj, key, numeric, asInt);
    bool        changed = false;

    if (active)
    {
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (caretPos_ > (int)editBuffer_.size())
            caretPos_ = (int)editBuffer_.size();

        // Insert characters at the caret position.
        int c = GetCharPressed();
        while (c > 0)
        {
            bool ok = numeric ? ((c >= '0' && c <= '9') || c == '-' || (!asInt && c == '.'))
                              : (c >= 32 && c < 127);
            if (ok && editBuffer_.size() < 60)
            {
                editBuffer_.insert(editBuffer_.begin() + caretPos_, (char)c);
                caretPos_++;
            }
            c = GetCharPressed();
        }

        // Delete the word to the left of the caret.
        auto deleteWordLeft = [&]()
        {
            int i = caretPos_;
            while (i > 0 && editBuffer_[i - 1] == ' ')
                i--;
            while (i > 0 && editBuffer_[i - 1] != ' ')
                i--;
            editBuffer_.erase(editBuffer_.begin() + i, editBuffer_.begin() + caretPos_);
            caretPos_ = i;
        };

        if (IsKeyPressed(KEY_BACKSPACE))
        {
            if (ctrl)
                deleteWordLeft();
            else if (caretPos_ > 0)
            {
                editBuffer_.erase(editBuffer_.begin() + (caretPos_ - 1));
                caretPos_--;
            }
        }
        if (IsKeyPressed(KEY_DELETE) && caretPos_ < (int)editBuffer_.size())
            editBuffer_.erase(editBuffer_.begin() + caretPos_);
        if (IsKeyPressed(KEY_LEFT))
            caretPos_ = ctrl ? 0 : (caretPos_ > 0 ? caretPos_ - 1 : 0);
        if (IsKeyPressed(KEY_RIGHT))
            caretPos_ = ctrl ? (int)editBuffer_.size()
                             : (caretPos_ < (int)editBuffer_.size() ? caretPos_ + 1 : caretPos_);
        if (IsKeyPressed(KEY_HOME))
            caretPos_ = 0;
        if (IsKeyPressed(KEY_END))
            caretPos_ = (int)editBuffer_.size();
        if (IsKeyPressed(KEY_ENTER))
            activeField_.clear();

        // Write the value into the model.
        if (numeric)
        {
            try
            {
                if (asInt)
                    obj[key] = (int)std::stoll(editBuffer_);
                else
                    obj[key] = std::stod(editBuffer_);
                changed = true;
            }
            catch (...)
            {
            }  // empty / "-" / "." — don't write
        }
        else
        {
            obj[key] = editBuffer_;
            changed = true;
        }
        disp = editBuffer_;
    }

    DrawRectangleRec(box, Fade(Ui::TITLE_BG, 0.8f));
    DrawRectangleLinesEx(box, 1.0f, active ? Ui::ACCENT : Ui::PANEL_BORDER);
    Ui::Text(disp.c_str(), (int)box.x + 6, (int)box.y + 5, 14, Ui::TEXT);
    if (active)
    {
        int cp = caretPos_ < (int)disp.size() ? caretPos_ : (int)disp.size();
        int cx = (int)box.x + 6 + Ui::TextWidth(disp.substr(0, cp).c_str(), 14) + 1;
        DrawRectangle(cx, (int)box.y + 5, 2, 14, Ui::ACCENT);
    }

    // -/+ buttons for numbers (increment step). We take the current value from the
    // buffer (if the field is focused) or from the model.
    if (numeric)
    {
        double v = (obj.contains(key) && obj[key].is_number()) ? obj[key].get<double>() : 0.0;
        if (active)
            try
            {
                v = std::stod(editBuffer_);
            }
            catch (...)
            {
            }

        auto apply = [&](double nv)
        {
            if (nv < 0.0)
                nv = 0.0;
            if (asInt)
                obj[key] = (int)llround(nv);
            else
                obj[key] = nv;
            if (active)  // sync the input buffer
            {
                editBuffer_ = asInt ? std::to_string((long long)llround(nv))
                                    : std::string(TextFormat("%g", nv));
                caretPos_ = (int)editBuffer_.size();
            }
            changed = true;
        };
        // The buttons are active only if not covered by an open dropdown.
        Rectangle bm{ r.x + r.width - 58, r.y, 26, 24 };
        Rectangle bp{ r.x + r.width - 28, r.y, 26, 24 };
        if (!anyDropdownOpen_)
        {
            Button minus(bm, "-", [&]() { apply(v - step); });
            Button plus(bp, "+", [&]() { apply(v + step); });
            minus.Process();
            plus.Process();
        }
        else  // draw statically, without reacting
        {
            for (Rectangle b : { bm, bp })
            {
                DrawRectangleRec(b, Ui::TITLE_BG);
                DrawRectangleLinesEx(b, 1.0f, Ui::PANEL_BORDER);
            }
            Ui::Text("-", (int)bm.x + 11, (int)bm.y + 4, 16, Ui::TEXT_DIM);
            Ui::Text("+", (int)bp.x + 9, (int)bp.y + 4, 16, Ui::TEXT_DIM);
        }
    }
    return changed;
}

// Enumeration: a button with the current value; a click expands the list (drawn
// deferred, on top of everything else — see DrawPropertyPanel).
void Editor::DropdownRow(Rectangle r, const char* label, nlohmann::json& obj, const char* key,
                         const std::vector<std::string>& opts,
                         const std::vector<std::string>& labels)
{
    bool hasLabels = (!labels.empty() && labels.size() == opts.size());
    auto labelFor = [&](const std::string& val) -> std::string
    {
        if (hasLabels)
            for (size_t i = 0; i < opts.size(); i++)
                if (opts[i] == val)
                    return labels[i];
        return val;
    };

    std::string cur = (obj.contains(key) && obj[key].is_string())
                          ? obj[key].get<std::string>()
                          : (opts.empty() ? std::string() : opts[0]);
    Ui::Text(label, (int)r.x, (int)r.y + 5, 13, Ui::TEXT_DIM);

    Rectangle btn{ r.x + 110, r.y, r.width - 110, 24 };
    bool      isOpen = (openDropdown_ == key);
    bool      overBtn = CheckCollisionPointRec(GetMousePosition(), btn);

    // Toggle open only if no other dropdown is open (or it's this one).
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && overBtn && (!anyDropdownOpen_ || isOpen))
        openDropdown_ = isOpen ? std::string() : key;

    DrawRectangleRec(btn, Fade(Ui::TITLE_BG, 0.8f));
    DrawRectangleLinesEx(btn, 1.0f, isOpen ? Ui::ACCENT : Ui::PANEL_BORDER);
    Ui::Text(labelFor(cur).c_str(), (int)btn.x + 6, (int)btn.y + 5, 14, Ui::TEXT);
    Ui::Text("v", (int)(btn.x + btn.width) - 16, (int)btn.y + 5, 14, Ui::TEXT_DIM);

    // Remember the open dropdown for deferred rendering on top of everything else.
    if (openDropdown_ == key)
    {
        dropObj_ = &obj;
        dropKey_ = key;
        dropOpts_ = opts;
        dropLabels_ = hasLabels ? labels : std::vector<std::string>{};
        dropAnchor_ = btn;
    }
}

// Property panel for the selected object: the set of fields depends on its category.
void Editor::DrawPropertyPanel()
{
    if (mode_ == Mode::Galaxy)
    {
        DrawGalaxyPanel();
        return;
    }
    if (selected_ < 0 || selected_ >= (int)handles_.size())
        return;

    anyDropdownOpen_ = !openDropdown_.empty();
    dropObj_ = nullptr;  // filled in if an open dropdown is encountered among the rows

    Rectangle panel = PanelRect();
    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawLineEx({ panel.x, 0.0f }, { panel.x, (float)screenHeight_ }, 1.0f, Ui::PANEL_BORDER);

    const ObjHandle& h = handles_[selected_];
    nlohmann::json&  obj =
        (h.category == "star") ? systemJson_["star"] : systemJson_[h.category][h.index];

    int x = (int)panel.x + 14;
    int y = 16;
    Ui::Text("PROPERTIES", x, y, 18, Ui::ACCENT);
    y += 26;
    Ui::Text(h.category.c_str(), x, y, 14, Ui::TEXT_DIM);
    y += 26;

    float w = panel.width - 28.0f;
    bool  changed = false;
    auto  row = [&](float rh) -> Rectangle
    {
        Rectangle r{ (float)x, (float)y, w, rh };
        y += (int)rh + 8;
        return r;
    };

    if (h.category == "star")
    {
        DropdownRow(row(24), "type", obj, "type", { "Yellow", "Red", "Blue" });
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
    }
    else if (h.category == "planets")
    {
        DropdownRow(row(24), "type", obj, "type", { "Rocky", "Gas", "Ice", "Lava", "Oceanic" });
        DropdownRow(row(24), "deposit", obj, "deposit", { "Iron", "Ice", "Crystal" });
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
        changed |= FieldRow(row(24), "orbitSpeed", obj, "orbitSpeed", true, true, 20);
        changed |= FieldRow(row(24), "orbitRadius", obj, "orbitRadius", true, true, 200);
    }
    else if (h.category == "stations")
    {
        changed |= FieldRow(row(24), "name", obj, "name", false, false);
        // The faction list comes from the registry (lawful, no pirates): id as the
        // value, name as the label.
        std::vector<std::string> facIds, facNames;
        for (int fi = 0; fi < Factions::Count(); fi++)
        {
            FactionId f = (FactionId)fi;
            if (!Factions::IsLawful(f))
                continue;
            facIds.push_back(Factions::Id(f));
            facNames.push_back(FactionName(f));
        }
        DropdownRow(row(24), "faction", obj, "faction", facIds, facNames);
        DropdownRow(row(24), "role", obj, "role",
                    { "TradeHub", "MiningOutpost", "Shipyard", "Military" });
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
    }
    else if (h.category == "asteroidFields")
    {
        changed |= FieldRow(row(24), "name", obj, "name", false, false);
        DropdownRow(row(24), "resource", obj, "resource", { "Iron", "Ice", "Crystal" });
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
        changed |= FieldRow(row(24), "ore", obj, "ore", true, true, 20);
    }
    else if (h.category == "nebulae")
    {
        changed |= FieldRow(row(24), "name", obj, "name", false, false);
        changed |= FieldRow(row(24), "radius", obj, "radius", true, true, 200);
    }
    else if (h.category == "derelicts")
    {
        changed |= FieldRow(row(24), "name", obj, "name", false, false);
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
        changed |= FieldRow(row(24), "reward", obj, "reward", true, true, 100);
    }
    else if (h.category == "gates")
    {
        changed |= FieldRow(row(24), "name", obj, "name", false, false);
        changed |= FieldRow(row(24), "size", obj, "size", true, true, 10);
        std::vector<std::string> ids, names;
        for (const auto& s : universe_.systems)
        {
            ids.push_back(s.id);
            names.push_back(s.name);
        }
        DropdownRow(row(24), "dest", obj, "destination", ids, names);
    }

    // Deferred rendering of the open dropdown on top of everything + selection handling.
    if (dropObj_ != nullptr)
    {
        float     ih = 24.0f;
        Rectangle list{ dropAnchor_.x, dropAnchor_.y + dropAnchor_.height, dropAnchor_.width,
                        ih * dropOpts_.size() };
        DrawRectangleRec(list, Ui::PANEL_BG);
        DrawRectangleLinesEx(list, 1.0f, Ui::ACCENT);

        Vector2 m = GetMousePosition();
        for (size_t i = 0; i < dropOpts_.size(); i++)
        {
            Rectangle ir{ list.x, list.y + ih * i, list.width, ih };
            bool      hov = CheckCollisionPointRec(m, ir);
            if (hov)
                DrawRectangleRec(ir, Fade(Ui::ACCENT, 0.2f));
            const std::string& shown =
                (dropLabels_.size() == dropOpts_.size()) ? dropLabels_[i] : dropOpts_[i];
            Ui::Text(shown.c_str(), (int)ir.x + 6, (int)ir.y + 5, 14, hov ? Ui::ACCENT : Ui::TEXT);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                (*dropObj_)[dropKey_] = dropOpts_[i];
                openDropdown_.clear();
                changed = true;
            }
        }
        // A click outside the list and the button closes it.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(m, list) &&
            !CheckCollisionPointRec(m, dropAnchor_))
            openDropdown_.clear();
    }

    if (changed)
    {
        dirty_ = true;
        RebuildEntities();
    }

    // Delete button (the star can't be deleted). The deletion itself happens after
    // the panel, so we don't touch obj once the array has changed.
    if (h.category != "star" && !anyDropdownOpen_)
    {
        Button del(Rectangle{ panel.x + 14, (float)(screenHeight_ - 44), panel.width - 28, 30 },
                   "Delete object  [Del]", [this]() { deleteRequested_ = true; });
        del.Process();
    }
    if (deleteRequested_)
    {
        deleteRequested_ = false;
        DeleteSelected();
    }
}

// The palette's label button at the bottom-left.
