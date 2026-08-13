import re
p='src/game/core/Game.cpp'
s=open(p,encoding='utf-8').read()

def sub(old,new,count=1):
    global s
    assert old in s, "MISSING: "+old[:70]
    s=s.replace(old,new,count)

# --- ternaries ---
sub("const std::string& active = networked_ ? snapshot_.systemId : sim_.ActiveId();",
    "const std::string& active = snapshot_.systemId;")
sub("std::string activeSys = networked_ ? snapshot_.systemId : sim_.ActiveId();",
    "std::string activeSys = snapshot_.systemId;")
sub("""    // Galactic news feed (system captures/reconquests). Source: over the network —
    // the server's galaxy snapshot, single-player — the local event log.
    const std::vector<std::string>& news = networked_ ? galaxyState_.events : sim_.Events();""",
    """    // Galactic news feed (system captures/reconquests), from the server's galaxy snapshot.
    const std::vector<std::string>& news = galaxyState_.events;""")
sub("complete = networked_ ? m.completable : (cur >= m.targetCount);",
    "complete = m.completable;")

# --- simple guards that become unconditional ---
sub("""        cmd_.toggleWeapon = true;
        if (networked_)
            weaponOn_ = !weaponOn_;  // over the network, keep the indicator/reticle optimistic""",
    """        cmd_.toggleWeapon = true;
        weaponOn_ = !weaponOn_;  // optimistic: the snapshot confirms it""")
sub("""                              // Over the network, enable the weapon via command (if it was off).
                              if (networked_ && !weaponOn_)
                                  cmd_.toggleWeapon = true;""",
    """                              if (!weaponOn_)
                                  cmd_.toggleWeapon = true;""")
sub("""    // Over the network, undocking is server-authoritative: send the order, the server clears the
    // dock and confirms via snapshot. Locally we leave optimistically (instant response).
    if (networked_)
    {
        Proto::Command c;
        c.undock = true;
        clientLink_->Send(Proto::EncodeCommand(c));
    }""",
    """    // Undocking is server-authoritative: send the order, the server clears the dock and
    // confirms via snapshot. We leave optimistically so the response feels instant.
    Proto::Command c;
    c.undock = true;
    clientLink_->Send(Proto::EncodeCommand(c));""")
sub("""    if (networked_)
    {
        for (const auto& e : clientWorld_)
            if (e->GetId() == id)
                return e.get();
        return nullptr;
    }
    return nullptr;
}""",
    """    for (const auto& e : clientWorld_)
        if (e->GetId() == id)
            return e.get();
    return nullptr;
}""")
sub("""            if (networked_)
            {
                ApplyTradeAcks(s);  // credit sales revenue (client account)
                for (const std::string& m : s.messages)  // server notifications (M4f-4)
                    FlashMessage(m);
            }
            incoming = std::move(s);""",
    """            ApplyTradeAcks(s);  // credit sales revenue (client account)
            for (const std::string& m : s.messages)  // server notifications (M4f-4)
                FlashMessage(m);
            incoming = std::move(s);""")
sub("""        // Network: a buffer of snapshots with arrival timestamps — for interpolating non-own
        // entities (entity interpolation, Gambetta). We draw them "in the past", smoothing
        // out snapshot jitter. The own ship is NOT touched by interpolation (prediction).
        if (networked_)
        {
            snapBuffer_.push_back({ GetTime(), snapshot_.entities });
            double cutoff = GetTime() - 0.5;  // keep ~0.5 s of history
            while (snapBuffer_.size() > 2 && snapBuffer_.front().t < cutoff)
                snapBuffer_.pop_front();
            if (snapBuffer_.size() > 120)
                snapBuffer_.pop_front();
        }""",
    """        // A buffer of snapshots with arrival timestamps — for interpolating non-own
        // entities (entity interpolation, Gambetta). We draw them "in the past", smoothing
        // out snapshot jitter. The own ship is NOT touched by interpolation (prediction).
        snapBuffer_.push_back({ GetTime(), snapshot_.entities });
        double cutoff = GetTime() - 0.5;  // keep ~0.5 s of history
        while (snapBuffer_.size() > 2 && snapBuffer_.front().t < cutoff)
            snapBuffer_.pop_front();
        if (snapBuffer_.size() > 120)
            snapBuffer_.pop_front();""")
sub("""        if (!networked_)
        {
            proxy->SetPosition(es.pos);
            if (NpcShip* n = dynamic_cast<NpcShip*>(proxy))
                n->SetHeading(es.heading);
        }
    }""",
    """    }""")
sub("""    // 2) Network: positions of non-own entities are taken "from the past", interpolating between two
    // buffer snapshots around renderTime. Smooths out snapshot jitter (the own
    // ship runs on prediction — it's not in clientWorld_).
    if (networked_)
    {""",
    """    // 2) Positions of non-own entities are taken "from the past", interpolating between two
    // buffer snapshots around renderTime. Smooths out snapshot jitter (the own
    // ship runs on prediction — it's not in clientWorld_).
    {""")
open(p,'w',encoding='utf-8',newline='').write(s)
print("collapsed; networked_ left:", s.count("networked_"))
