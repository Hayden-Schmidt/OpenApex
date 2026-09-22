#pragma once

#include "screen.hpp"

// design reference/2. Idle Screen/2. idle screen.md
class IdleScreen : public Screen {
public:
    IdleScreen();
    void update(const terminal_view_state_t &state) override;
};
