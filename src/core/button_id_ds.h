// src/core/button_id_ds.h
//
// Bit layout for MergedCore::InputState::buttons when driving a DS
// title (MelonDSCore). Referenced by melon_ds_core.cpp (translates
// these into melonDS's own key mask) and by any frontend that wants
// to feed a DS session input (see ds_player_window.cpp).

#pragma once

#include <cstdint>

namespace MergedCore {

enum DSButton : uint32_t {
    DS_BTN_A = 1u << 0,
    DS_BTN_B = 1u << 1,
    DS_BTN_X = 1u << 2,
    DS_BTN_Y = 1u << 3,
    DS_BTN_L = 1u << 4,
    DS_BTN_R = 1u << 5,
    DS_BTN_START = 1u << 6,
    DS_BTN_SELECT = 1u << 7,
    DS_BTN_UP = 1u << 8,
    DS_BTN_DOWN = 1u << 9,
    DS_BTN_LEFT = 1u << 10,
    DS_BTN_RIGHT = 1u << 11,
};

} // namespace MergedCore
