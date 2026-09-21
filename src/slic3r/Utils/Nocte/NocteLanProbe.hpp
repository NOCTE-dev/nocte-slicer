// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The headless diagnostics probe for LAN Developer Mode, the C++ counterpart of
// tools/nocte/lan/a4_probe.py. It exercises every leg of the protocol the agent depends on —
// TCP, TLS, MQTT CONNACK/SUBACK/report flow, FTPS listing, upload and delete — and prints a
// checklist in which no address, serial or access code can appear.
//
// It never sends a print command, and it has no way to reach the agent's print gate.
//
// Selected by NOCTE_LAN_PROBE=1 in the environment, because the CLI option table lives in a file
// this change does not touch. Credentials come from NOCTE_LAN_IP, NOCTE_LAN_SERIAL and
// NOCTE_LAN_ACCESS_CODE and from nowhere else.

#ifndef slic3r_Utils_Nocte_NocteLanProbe_hpp_
#define slic3r_Utils_Nocte_NocteLanProbe_hpp_

namespace Slic3r {
namespace Nocte {

// Runs the probe and returns a process exit code: 0 when every mandatory check passed, 1 when
// any did. The checklist goes to stdout and, always, to ./nocte_lan_probe.txt — the Windows
// build is a GUI-subsystem binary whose stdout may go nowhere.
int run_lan_probe();

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Utils_Nocte_NocteLanProbe_hpp_
