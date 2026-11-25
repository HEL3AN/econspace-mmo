#pragma once

// The server's self-tests (#354), run by `econserver hosttest | accttest | worldtest |
// ordertest | opstest`. Each returns 0 on success.
int HostSelftest();
int HostNameSelftest();
int AccountSelftest();
int WorldSelftest();
int OrderSelftest();
int OpsSelftest();
