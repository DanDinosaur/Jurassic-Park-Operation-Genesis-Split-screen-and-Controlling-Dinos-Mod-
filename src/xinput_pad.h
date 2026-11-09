// xinput_pad.h - thin XInput wrapper: poll one pad, edge-detect buttons,
// deadzoned sticks/triggers, rumble. Plug-and-play (connect/disconnect per poll).
#pragma once
#include <cstdint>

enum PadButton {
    PAD_A, PAD_B, PAD_X, PAD_Y,
    PAD_LB, PAD_RB, PAD_BACK, PAD_START,
    PAD_LS, PAD_RS,
    PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT,
    PAD_COUNT
};

// Debug virtual pads: when active, XPad index 0/1 reads jpogmod_pad.txt / jpogmod_pad2.txt instead of XInput.
struct VirtualPadState {
    bool  active = false;
    bool  down[PAD_COUNT]{};
    float lx=0, ly=0, rx=0, ry=0, lt=0, rt=0;
    unsigned untilTick = 0;   // FOR=<ms>: state reverts to neutral after this tick (0 = hold)
};
namespace virtualpad {
    // Re-reads <gamefolder>\jpogmod_pad.txt if it changed. Call ~10x/s.
    // Format: space/line separated tokens. Button names present = held
    // (A B X Y LB RB BACK START LS RS UP DOWN LEFT RIGHT); axes as LX=0.5 LY=-1
    // RX= RY= LT= RT=, FOR=<ms> (pulse: neutral after ms). File missing/empty = inactive.
    void Refresh();
    const VirtualPadState& Get(int index = 0);   // 0 = jpogmod_pad.txt, 1 = jpogmod_pad2.txt
}

// Players follow CONNECTED controllers, not fixed XInput slots (owner rule: ROG Ally turns its
// built-in pad off when a Bluetooth pad connects, and the BT pad can land in any slot 0-3).
//   Player 1 = first connected slot, Player 2 = the next one.
//   If Player 1's pad disappears and another pad is (or becomes) connected, that pad is Player 1
//   immediately - same mapping, no remap, no second player.
namespace padslots {
    void Update();            // scan slots 0-3 (cheap: empty slots re-checked every 500 ms); thread-safe
    int  SlotOf(int player);  // XInput slot for player 0/1, -1 = none
    unsigned Since(int player); // GetTickCount() when that player's pad was (re)assigned, 0 = none
}

class XPad {
public:
    explicit XPad(int player) : m_index(player) {}   // player 0 = P1, 1 = P2 (not a slot)

    // Call once per frame. Returns true if a controller is connected.
    bool Poll(float deadzone, float triggerThreshold);
    bool Connected() const { return m_connected; }

    bool Down(PadButton b)    const { return m_down[b]; }       // held
    bool Pressed(PadButton b) const { return m_down[b] && !m_prev[b]; } // this-frame edge
    bool Released(PadButton b)const { return !m_down[b] && m_prev[b]; }

    float LX() const { return m_lx; }  // -1..1 (deadzoned)
    float LY() const { return m_ly; }
    float RX() const { return m_rx; }
    float RY() const { return m_ry; }
    float LT() const { return m_lt; }  // 0..1
    float RT() const { return m_rt; }

    // JPOG63: OR keyboard-driven buttons / axes into this poll's state (call right after Poll; edges still work).
    void Merge(const bool* down, float lx, float ly, float rx, float ry, float lt, float rt) {
        for (int i = 0; i < PAD_COUNT; ++i) m_down[i] = m_down[i] || down[i];
        if (lx) m_lx = lx; if (ly) m_ly = ly; if (rx) m_rx = rx; if (ry) m_ry = ry;
        if (lt > m_lt) m_lt = lt; if (rt > m_rt) m_rt = rt;
    }
    // Rumble 0..1 (low=big motor, high=small motor).
    void SetRumble(float low, float high);

private:
    int   m_index;
    bool  m_connected = false;
    bool  m_down[PAD_COUNT]{};
    bool  m_prev[PAD_COUNT]{};
    float m_lx=0,m_ly=0,m_rx=0,m_ry=0,m_lt=0,m_rt=0;
};
