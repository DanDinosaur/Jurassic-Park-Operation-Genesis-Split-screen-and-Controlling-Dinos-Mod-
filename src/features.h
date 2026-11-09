// features.h - the gameplay feature modules, called by the state machine.
// These are SCAFFOLDS: interfaces + the exact game.h addresses and [CONFIRM]
// reads each needs. They compile and run as safe no-ops (logging what they'd do)
// so the loader + controller work before any feature is implemented. Implement
// them milestone by milestone (see MILESTONES.md); each references its design-doc
// section (docs/05_PLAYER_MODE_AND_CONTROLLER_DESIGN.md).
#pragma once
#include "playermode.h"

namespace features {
    void Init();
    void Shutdown();

    namespace avatar {      // design sec 4  (AUnitStaff ranger body)
        bool Spawn();       // spawn/repurpose an AUnitStaff at the camera focus
        void Despawn();
        void Tick(float dt); // move/animate from input  [CONFIRM unit pos offset]
    }
    namespace camera {      // design sec 5  (3rd-person / scope / first-person)
        void Tick(float dt, Mode m);
        void CycleView();   // ots -> scope -> firstperson
    }
    namespace shooter {     // design sec 7  (reuse AMiniGameState + HurtUnit)
        void Tick(float dt, Mode m);
        void SwapWeapon();  // tranq <-> lethal
    }
    namespace vehicle {     // design sec 9  (player steer + spawn menu)
        void OpenSpawnMenu();  // car / helicopter
        void Tick(float dt);
    }
    namespace splitscreen { // design sec 10.5  (two-view co-op)
        void Tick(float dt);
        void SetEnabled(bool on);
        void SetOrientationVertical(bool vertical);
    }
}
