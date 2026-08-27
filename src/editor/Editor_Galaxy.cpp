// Galaxy mode: the map of systems and the links between them, plus the panel that
// edits the selected node. One translation unit of Editor (#17).
//
// What this file draws and reads; what it changes goes through Editor_Universe, which
// owns the shape of universe.json.

#include "Editor.h"

#include "ui/UiTheme.h"
#include "ui/Button.h"
#include "raymath.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using nlohmann::json;

#include "UniverseJson.h"

Rectangle Editor::ModeButtonRect() const
{
    return { screenWidth_ / 2.0f - 151.0f, 12.0f, 150.0f, 30.0f };  // left one in the group
}

void Editor::EnterGalaxyMode(bool on)
{
    galaxyMode_ = on;
    selected_ = -1;
    gSelected_ = -1;
    gLinksOwner_.clear();
    placeArchetype_.clear();
    activeField_.clear();
    openDropdown_.clear();
    panning_ = false;
    gGrabbed_ = false;

    if (on)
    {
        camera_.target = { 0.0f, 0.0f };
        camera_.zoom = 1.5f;  // map coordinates are on the order of hundreds
    }
    else
    {
        RefreshUniverseStruct();
        camera_.target = { 0.0f, 0.0f };
        camera_.zoom = 0.03f;
        if (currentSystem_ >= (int)universe_.systems.size())
            currentSystem_ = 0;
        if (!universe_.systems.empty())
            LoadSystemAt(currentSystem_);
    }
}

// A system's map position from the JSON.

void Editor::HandleGalaxyInput()
{
    Vector2 m = GetMousePosition();
    bool    overPanel = CheckCollisionPointRec(m, PanelRect());
    bool    overSave = CheckCollisionPointRec(m, SaveButtonRect());
    bool    overMode = CheckCollisionPointRec(m, ModeButtonRect());
    bool    overUi = overPanel || overSave || overMode;

    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && !overUi)
        camera_.zoom = Clamp(camera_.zoom * (1.0f + wheel * 0.12f), 0.2f, 6.0f);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        activeField_.clear();

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !overUi && universeJson_.contains("systems"))
    {
        Vector2     wm = GetScreenToWorld2D(m, camera_);
        int         hit = -1;
        float       pick = 14.0f / camera_.zoom;
        const json& sys = universeJson_["systems"];
        for (int i = 0; i < (int)sys.size(); i++)
            if (CheckCollisionPointCircle(wm, MapPos(sys[i]), pick))
                hit = i;

        if (hit >= 0)
        {
            // Double-click a node — open the system in edit mode.
            double t = GetTime();
            if (hit == gLastClickNode_ && (t - gLastClickTime_) < 0.35)
            {
                OpenSystem(hit);
                return;
            }
            gLastClickTime_ = t;
            gLastClickNode_ = hit;

            gSelected_ = hit;
            gGrabbed_ = true;
            gDragging_ = false;
            pressPos_ = m;
            Vector2 p = MapPos(sys[hit]);
            gGrabAnchor_ = { p.x - wm.x, p.y - wm.y };
        }
        else
        {
            gSelected_ = -1;
            panning_ = true;
            dragLast_ = m;
        }
    }
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        if (gGrabbed_ && gSelected_ >= 0)
        {
            // Movement starts only after the cursor actually moves — the first
            // click just selects the system (and doesn't mark it "unsaved").
            if (!gDragging_ && fabsf(m.x - pressPos_.x) + fabsf(m.y - pressPos_.y) > 4.0f)
                gDragging_ = true;
            if (gDragging_)
            {
                Vector2 wm = GetScreenToWorld2D(m, camera_);
                universeJson_["systems"][gSelected_]["map"] = json::array(
                    { (int)roundf(wm.x + gGrabAnchor_.x), (int)roundf(wm.y + gGrabAnchor_.y) });
                universeDirty_ = true;
            }
        }
        else if (panning_)
        {
            camera_.target.x -= (m.x - dragLast_.x) / camera_.zoom;
            camera_.target.y -= (m.y - dragLast_.y) / camera_.zoom;
            dragLast_ = m;
        }
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        gGrabbed_ = false;
        gDragging_ = false;
        panning_ = false;
    }
}

void Editor::DrawGalaxy()
{
    if (!universeJson_.contains("systems"))
        return;
    const json& sys = universeJson_["systems"];

    BeginMode2D(camera_);

    // Links.
    if (universeJson_.contains("links"))
        for (const auto& l : universeJson_["links"])
        {
            if (!l.is_array() || l.size() < 2)
                continue;
            std::string a = l[0], b = l[1];
            Vector2     pa{ 0, 0 }, pb{ 0, 0 };
            bool        fa = false, fb = false;
            for (const auto& s : sys)
            {
                if (s.value("id", std::string()) == a)
                {
                    pa = MapPos(s);
                    fa = true;
                }
                if (s.value("id", std::string()) == b)
                {
                    pb = MapPos(s);
                    fb = true;
                }
            }
            if (fa && fb)
                DrawLineEx(pa, pb, 1.5f / camera_.zoom, Fade(Ui::PANEL_BORDER, 0.9f));
        }

    // Nodes.
    float r = 8.0f / camera_.zoom;
    for (int i = 0; i < (int)sys.size(); i++)
    {
        Vector2 p = MapPos(sys[i]);
        bool    cur = (i == gSelected_);
        DrawCircleV(p, r, cur ? Ui::ACCENT : Ui::TEXT_DIM);
        if (cur)
            DrawCircleLines((int)p.x, (int)p.y, r + 6.0f / camera_.zoom, Ui::ACCENT);
    }
    EndMode2D();

    // System labels — in screen coordinates (constant size).
    for (const auto& s : sys)
    {
        Vector2 sp = GetWorldToScreen2D(MapPos(s), camera_);
        Ui::Text(s.value("name", std::string("?")).c_str(), (int)sp.x + 12, (int)sp.y - 8, 14,
                 Ui::TEXT);
    }
}

void Editor::DrawGalaxyPanel()
{
    anyDropdownOpen_ = !openDropdown_.empty();
    dropObj_ = nullptr;

    Rectangle panel = PanelRect();
    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawLineEx({ panel.x, 0.0f }, { panel.x, (float)screenHeight_ }, 1.0f, Ui::PANEL_BORDER);

    int   x = (int)panel.x + 14;
    float w = panel.width - 28.0f;
    int   y = 16;
    Ui::Text("GALAXY", x, y, 18, Ui::ACCENT);
    y += 30;

    Button add(Rectangle{ panel.x + 14, (float)y, w, 28 }, "+ Add system",
               [this]() { AddSystem(); });
    add.Process();
    y += 40;

    if (gSelected_ < 0 || !universeJson_.contains("systems") ||
        gSelected_ >= (int)universeJson_["systems"].size())
    {
        Ui::Text("Select a system node", x, y, 14, Ui::TEXT_DIM);
        return;
    }

    json& obj = universeJson_["systems"][gSelected_];
    bool  changed = false;
    auto  row = [&](float rh) -> Rectangle
    {
        Rectangle r{ (float)x, (float)y, w, rh };
        y += (int)rh + 8;
        return r;
    };

    // Open the system in object-editing mode.
    if (!anyDropdownOpen_)
    {
        int    sel = gSelected_;
        Button open(Rectangle{ (float)x, (float)y, w, 26.0f }, "Open in system view  (dbl-click)",
                    [this, sel]() { OpenSystem(sel); });
        open.Process();
    }
    y += 34;

    changed |= FieldRow(row(24), "name", obj, "name", false, false);

    // The id drives the file name. We remember the old values on focus, and when
    // editing ends we rename the file on disk and update references to the old id.
    bool idActive = (activeField_ == "id");
    if (idActive && !gIdEditing_)
    {
        gIdEditing_ = true;
        gOldId_ = obj.value("id", std::string());
        gOldFile_ = obj.value("file", std::string());
    }
    if (FieldRow(row(24), "id", obj, "id", false, false))
    {
        obj["file"] = obj.value("id", std::string()) + ".json";  // auto file name
        changed = true;
    }
    if (!idActive && gIdEditing_)
    {
        gIdEditing_ = false;
        std::string newId = obj.value("id", std::string());
        std::string newFile = obj.value("file", std::string());

        // Rename the system's file on disk (no orphaned copies).
        if (newFile != gOldFile_ && !gOldFile_.empty())
        {
            std::string op = dataDir_ + "systems/" + gOldFile_;
            std::string np = dataDir_ + "systems/" + newFile;
            if (FileExists(op.c_str()) && !FileExists(np.c_str()))
                std::rename(op.c_str(), np.c_str());
        }
        // Update references to the old id (links and the start system).
        if (newId != gOldId_ && !gOldId_.empty())
        {
            if (universeJson_.contains("links"))
                for (auto& l : universeJson_["links"])
                    for (auto& e : l)
                        if (e == gOldId_)
                            e = newId;
            if (universeJson_.value("start", std::string()) == gOldId_)
                universeJson_["start"] = newId;
            gLinksOwner_.clear();
        }
    }

    // The file name is read-only (managed through the id).
    Ui::Text("file", x, y + 5, 13, Ui::TEXT_DIM);
    Ui::Text(obj.value("file", std::string()).c_str(), x + 110, y + 5, 14, Ui::TEXT_DIM);
    y += 32;

    // Security level 0..1 (affects pirate spawning in the game).
    changed |= FieldRow(row(24), "security", obj, "security", true, false, 0.1);

    Vector2 mp = MapPos(obj);
    Ui::Text(TextFormat("map: %d, %d", (int)mp.x, (int)mp.y), x, y, 13, Ui::TEXT_DIM);
    y += 26;

    Ui::Text("LINKS", x, y, 14, Ui::TEXT_DIM);
    y += 22;

    std::string selfId = obj.value("id", std::string());
    const json& sys = universeJson_["systems"];

    // We rebuild the selected system's link list when the system changes; we always
    // show at least one (empty) row.
    if (gLinksOwner_ != selfId)
    {
        gLinksJson_ = json::object();
        int k = 0;
        if (universeJson_.contains("links"))
            for (const auto& l : universeJson_["links"])
                if (l.is_array() && l.size() >= 2)
                {
                    std::string la = l[0], lb = l[1];
                    if (la == selfId)
                        gLinksJson_[std::to_string(k++)] = lb;
                    else if (lb == selfId)
                        gLinksJson_[std::to_string(k++)] = la;
                }
        if (k == 0)
            gLinksJson_["0"] = "";
        gLinksOwner_ = selfId;
    }

    // The current value of a link row by index.
    auto curVal = [&](int kk) -> std::string
    {
        std::string key = std::to_string(kk);
        return (gLinksJson_.contains(key) && gLinksJson_[key].is_string())
                   ? gLinksJson_[key].get<std::string>()
                   : std::string();
    };

    int  count = (int)gLinksJson_.size();
    int  removeIdx = -1;
    bool linksChanged = false;  // touch the global links only on a real edit
    for (int k = 0; k < count; k++)
    {
        std::string key = std::to_string(k);
        if (!gLinksJson_.contains(key))
            continue;

        // Options: other systems not yet used by OTHER link rows
        // (one system can't be picked in several links).
        std::vector<std::string> ids, names;
        for (const auto& s : sys)
        {
            std::string oid = s.value("id", std::string());
            if (oid == selfId)
                continue;
            bool used = false;
            for (int j = 0; j < count; j++)
                if (j != k && curVal(j) == oid)
                    used = true;
            if (used)
                continue;
            ids.push_back(oid);
            names.push_back(s.value("name", oid));
        }

        DropdownRow(Rectangle{ (float)x, (float)y, w - 30.0f, 24.0f }, "", gLinksJson_, key.c_str(),
                    ids, names);
        Rectangle rb{ (float)x + w - 26.0f, (float)y, 24.0f, 24.0f };
        if (!anyDropdownOpen_)
        {
            Button rm(rb, "x", [&removeIdx, k]() { removeIdx = k; });
            rm.Process();
        }
        else
        {
            DrawRectangleRec(rb, Ui::TITLE_BG);
            DrawRectangleLinesEx(rb, 1.0f, Ui::PANEL_BORDER);
            Ui::Text("x", (int)rb.x + 8, (int)rb.y + 4, 14, Ui::TEXT_DIM);
        }
        y += 28;
    }

    if (!anyDropdownOpen_)
    {
        Button addl(Rectangle{ (float)x, (float)y, w, 24.0f }, "+ link", [this]()
                    { gLinksJson_[std::to_string((int)gLinksJson_.size())] = std::string(); });
        addl.Process();
    }
    y += 30;

    // Deferred rendering of the open dropdown (for the link/enum selections above).
    if (dropObj_ != nullptr)
    {
        float     ih = 24.0f;
        Rectangle list{ dropAnchor_.x, dropAnchor_.y + dropAnchor_.height, dropAnchor_.width,
                        ih * dropOpts_.size() };
        DrawRectangleRec(list, Ui::PANEL_BG);
        DrawRectangleLinesEx(list, 1.0f, Ui::ACCENT);
        Vector2 mm = GetMousePosition();
        for (size_t i = 0; i < dropOpts_.size(); i++)
        {
            Rectangle ir{ list.x, list.y + ih * i, list.width, ih };
            bool      hov = CheckCollisionPointRec(mm, ir);
            if (hov)
                DrawRectangleRec(ir, Fade(Ui::ACCENT, 0.2f));
            const std::string& shown =
                (dropLabels_.size() == dropOpts_.size()) ? dropLabels_[i] : dropOpts_[i];
            Ui::Text(shown.c_str(), (int)ir.x + 6, (int)ir.y + 5, 14, hov ? Ui::ACCENT : Ui::TEXT);
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                (*dropObj_)[dropKey_] = dropOpts_[i];
                openDropdown_.clear();
                linksChanged = true;
            }
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mm, list) &&
            !CheckCollisionPointRec(mm, dropAnchor_))
            openDropdown_.clear();
    }

    // Deleting a link row (with re-indexing of keys).
    if (removeIdx >= 0)
    {
        json rebuilt = json::object();
        int  nk = 0;
        for (int k = 0; k < count; k++)
            if (k != removeIdx)
            {
                std::string key = std::to_string(k);
                if (gLinksJson_.contains(key))
                    rebuilt[std::to_string(nk++)] = gLinksJson_[key];
            }
        gLinksJson_ = rebuilt;
        linksChanged = true;
    }

    // We rebuild the global links ONLY on a real edit (otherwise merely selecting a
    // system would normalize the order and falsely mark it "unsaved").
    if (linksChanged)
    {
        std::vector<std::string> partners;
        for (int k = 0; k < (int)gLinksJson_.size(); k++)
        {
            std::string key = std::to_string(k);
            if (!gLinksJson_.contains(key) || !gLinksJson_[key].is_string())
                continue;
            std::string p = gLinksJson_[key];
            if (p.empty() || p == selfId)
                continue;
            bool dup = false;
            for (const std::string& q : partners)
                if (q == p)
                    dup = true;
            if (!dup)
                partners.push_back(p);
        }
        json oldLinks = universeJson_.contains("links") ? universeJson_["links"] : json::array();

        // This system's old partners (before the edit) — for diffing the gates.
        std::vector<std::string> oldPartners;
        for (const auto& l : oldLinks)
            if (l.is_array() && l.size() >= 2)
            {
                std::string la = l[0], lb = l[1];
                if (la == selfId)
                    oldPartners.push_back(lb);
                else if (lb == selfId)
                    oldPartners.push_back(la);
            }
        auto has = [](const std::vector<std::string>& v, const std::string& s)
        {
            for (const std::string& q : v)
                if (q == s)
                    return true;
            return false;
        };

        // Added links → gates in both systems; removed links → remove the gates.
        for (const std::string& p : partners)
            if (!has(oldPartners, p))
            {
                SyncGate(selfId, p, true);
                SyncGate(p, selfId, true);
            }
        for (const std::string& p : oldPartners)
            if (!has(partners, p))
            {
                SyncGate(selfId, p, false);
                SyncGate(p, selfId, false);
            }

        json newLinks = json::array();
        for (const auto& l : oldLinks)
            if (l.is_array() && l.size() >= 2 && l[0] != selfId && l[1] != selfId)
                newLinks.push_back(l);
        for (const std::string& p : partners)
            newLinks.push_back(json::array({ selfId, p }));

        if (newLinks != oldLinks)
        {
            universeJson_["links"] = newLinks;
            universeDirty_ = true;
        }
    }

    if (changed)
        universeDirty_ = true;

    // Deleting the system (deferred).
    if (!anyDropdownOpen_)
    {
        Button del(Rectangle{ panel.x + 14, (float)(screenHeight_ - 44), w, 30 }, "Delete system",
                   [this]() { deleteRequested_ = true; });
        del.Process();
    }
    if (deleteRequested_)
    {
        deleteRequested_ = false;
        DeleteSystem(gSelected_);
    }
}
