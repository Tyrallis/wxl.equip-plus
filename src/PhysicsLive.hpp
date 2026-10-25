// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// wxl.equip-plus: live retail physics for equipped pieces (plumes, ribbons, chains, cloth flaps).
//
// The physics engine itself (physics/physics_domino.*) is Kirazul's port of the retail client's
// engine and is NOT covered by this notice; see physics/README.md.

#pragma once

#include <cstdint>
#include <string>

struct wxl_m2_api;

namespace equip_plus::physics_live
{
    /// Reads [Physics] Enabled from the ini files (the mod's, then the client's). Main thread.
    void Init(const std::string& modIni, const std::string& clientIni);

    bool Enabled();
    void SetEnabled(bool on);

    /// Reads DBFilesClient\WXLEquipPlusPhysics.phy2 through the client's storage and starts
    /// watching the bones of every model it has a row for. Main thread, once storage is up.
    void LoadTable();

    /// Stops watching and forgets every simulation. Main thread.
    void Shutdown();

    struct Stats
    {
        uint32_t tableRows = 0;   // rows in the loaded table
        uint32_t files     = 0;   // model files seen that have a row
        uint32_t live      = 0;   // model instances being simulated now
        uint32_t steps     = 0;   // simulation steps run (all instances)
        uint32_t faults    = 0;   // rows that did not fit their file, NaNs refused
        uint32_t dropped   = 0;   // instances forgotten after 2 s unseen
        double   lastFrameMs  = 0;
        double   worstFrameMs = 0;
    };
    Stats GetStats();

    /// Where diagnostics go (Equip Plus's log).
    void SetLogger(void (*log)(const char* line));
}
