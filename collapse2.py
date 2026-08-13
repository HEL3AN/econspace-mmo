p='src/game/core/Game.cpp'
s=open(p,encoding='utf-8').read()
def sub(old,new):
    global s
    assert old in s, "MISSING: "+old[:70]
    s=s.replace(old,new,1)

sub("""    Proto::PlayerView& p = snapshot_.player;
    if (networked_)
    {
        // Network: RECONCILIATION""","""    Proto::PlayerView& p = snapshot_.player;
    {
        // RECONCILIATION""")

sub("""                if (networked_)  // account on the server — pay via command
                {
                    Proto::Command c;
                    c.payBountyFaction = (int)stationFaction;
                    clientLink_->Send(Proto::EncodeCommand(c));
                }
                else
                {
                    player_.AddMoney(-bounty);
                    player_.SetBounty(stationFaction, 0.0);
                }
                FlashMessage""","""                // The account is on the server — pay via command.
                Proto::Command c;
                c.payBountyFaction = (int)stationFaction;
                clientLink_->Send(Proto::EncodeCommand(c));
                FlashMessage""")

sub("""                               if (networked_)
                               {
                                   // Network: selling is an order to the server; ApplyTradeAcks
                                   // credits the revenue on acknowledgement (server price).
                                   Proto::Command c;
                                   c.sellType = (int)type;
                                   c.sellAmount = cargo;
                                   clientLink_->Send(Proto::EncodeCommand(c));
                                   return;
                               }
                               int amount = playerShip_->GetCargoAmount(type);
                               // Single-player: selling is a server mutation (market + hold) in
                               // the core; the client applies account effects to the revenue.
                               Simulation::PlayerSellResult sr =
                                   sim_.StepPlayerSell(sim_.Active(), (int)type, amount);
                               if (sr.sold > 0)
                               {
                                   // The trading skill and reputation increase the revenue.
                                   double revenue = sr.gross *
                                       player_.GetSkills().GetBonus(SkillType::Trading) * sellMul;
                                   player_.AddMoney(revenue);
                                   player_.GetSkills().AddXp(SkillType::Trading,
                                                             (float)(revenue * 0.05));
                                   // Trading strengthens reputation with the station's faction.
                                   player_.AddReputation(dockedStation_->GetFaction(),
                                                         (float)(revenue * 0.002));
                               }
                           });""","""                               // Selling is an order to the server; ApplyTradeAcks credits
                               // the revenue on acknowledgement (at the server's price).
                               Proto::Command c;
                               c.sellType = (int)type;
                               c.sellAmount = cargo;
                               clientLink_->Send(Proto::EncodeCommand(c));
                           });""")

sub("""                                 if (networked_)
                                 {
                                     Proto::Command c;
                                     c.refitShip = (int)i;
                                     clientLink_->Send(Proto::EncodeCommand(c));
                                 }
                                 else
                                 {
                                     sim_.RefitPlayer(GetShipCatalog()[i].stats);
                                 }
                                 currentShipIndex_""","""                                 Proto::Command c;
                                 c.refitShip = (int)i;
                                 clientLink_->Send(Proto::EncodeCommand(c));
                                 currentShipIndex_""")

sub("""                              if (networked_)
                              {
                                  // The purchase is server-authoritative: the server does the
                                  // charge/refit (BuyShip); the mirror updates the money. Ownership/index —
                                  // client-side display (optimistic).
                                  Proto::Command c;
                                  c.buyShip = (int)i;
                                  clientLink_->Send(Proto::EncodeCommand(c));
                              }
                              else
                              {
                                  player_.AddMoney(-price);
                                  sim_.RefitPlayer(st.stats);
                              }
                              ownedShips_[i] = true;""","""                              // The purchase is server-authoritative: the server charges and
                              // refits (BuyShip) and the mirror updates the money. Ownership and
                              // index are client-side display only — see #5.
                              Proto::Command c;
                              c.buyShip = (int)i;
                              clientLink_->Send(Proto::EncodeCommand(c));
                              ownedShips_[i] = true;""")

sub("""        if (networked_)  // accepting is a server mutation (missions are authoritative)
        {
            Proto::Command c;
            c.acceptOffer = toAccept;
            clientLink_->Send(Proto::EncodeCommand(c));
        }
        else
        {
            missions_.Accept(toAccept);
        }""","""        // Accepting is a server mutation (missions are authoritative).
        Proto::Command c;
        c.acceptOffer = toAccept;
        clientLink_->Send(Proto::EncodeCommand(c));""")

sub("""                // Cargo: over the network from the snapshot (playerShip_ isn't synced), otherwise local.
                int cur = playerShip_->GetCargoAmount(m.resource);
                if (networked_)
                {
                    int idx = 0;""","""                // Cargo comes from the snapshot: the predicted ship's hold is not synced.
                int cur = 0;
                {
                    int idx = 0;""")
open(p,'w',encoding='utf-8',newline='').write(s)
print("networked_ left:", s.count("networked_"))
