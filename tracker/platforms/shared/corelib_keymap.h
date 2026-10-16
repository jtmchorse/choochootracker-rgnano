#ifndef CORELIB_KEYMAP_H
#define CORELIB_KEYMAP_H

#if defined(RG35XX_BUILD)
// RG35xx: no SDL header needed, uses hardcoded j2k.so keycodes
#elif defined(MIYOOPORTS_BUILD)
#include <SDL/SDL.h>
#elif defined(RGNANO_BUILD)
#include <SDL2/SDL.h>
#else
#include <SDL2/SDL.h>
#endif

// Keyboard mapping for different platforms

#if defined(RG35XX_BUILD)
// RG35xx mapping (keycodes for j2k.so library)
#define BTN_UP          119
#define BTN_DOWN        115
#define BTN_LEFT        113
#define BTN_RIGHT       100
#define BTN_A           97
#define BTN_B           98
#define BTN_X           120
#define BTN_Y           121
#define BTN_L1          104
#define BTN_R1          108
#define BTN_L2          106
#define BTN_R2          107
#define BTN_SELECT      110
#define BTN_START       109
#define BTN_MENU        117

#elif defined(MIYOOPORTS_BUILD)
// MIYOO_PORTS mapping (SDL keycodes)
#define BTN_UP          SDLK_UP
#define BTN_DOWN        SDLK_DOWN
#define BTN_LEFT        SDLK_LEFT
#define BTN_RIGHT       SDLK_RIGHT
#define BTN_A           SDLK_SPACE
#define BTN_B           SDLK_LCTRL
#define BTN_X           SDLK_LSHIFT
#define BTN_Y           SDLK_LALT
#define BTN_L1          SDLK_e
#define BTN_R1          SDLK_t
#define BTN_L2          SDLK_TAB
#define BTN_R2          SDLK_BACKSPACE
#define BTN_SELECT      SDLK_RCTRL
#define BTN_START       SDLK_RETURN
#define BTN_MENU        0

#elif defined(RGNANO_BUILD)
// RG Nano (FunKey-OS). The buttons are a uinput keyboard ("ample") whose keys
// SDL's evdev backend turns into these letters (proven on the device).
// LSDJ style: A = edit, B = option, Start = play, Select (Q) = shift.
#define BTN_UP          (SDLK_u)
#define BTN_DOWN        (SDLK_d)
#define BTN_LEFT        (SDLK_l)
#define BTN_RIGHT       (SDLK_r)
#define BTN_A           (SDLK_a)
#define BTN_B           (SDLK_b)
#define BTN_X           (SDLK_x)
#define BTN_Y           (SDLK_y)
#define BTN_L1          (SDLK_m)
#define BTN_R1          (SDLK_n)
// No L2/R2 on the hardware: Fn+L arrives as V and Fn+R as O, the only free
// Fn chords. They feed the motion record/erase slots.
#define BTN_L2          (SDLK_v)
#define BTN_R2          (SDLK_o)
#define BTN_SELECT      (SDLK_q)
#define BTN_START       (SDLK_s)
// Fn alone (K) is the quit-chord modifier: it is swallowed, never mapped.
#define BTN_MENU        (SDLK_k)

#else
// Other platforms (Desktop, PortMaster, Android)
#define BTN_UP          (SDLK_UP)
#define BTN_DOWN        (SDLK_DOWN)
#define BTN_LEFT        (SDLK_LEFT)
#define BTN_RIGHT       (SDLK_RIGHT)
#define BTN_A           (SDLK_x)
#define BTN_B           (SDLK_z)
#define BTN_X           (SDLK_s)
#define BTN_Y           (SDLK_a)
#define BTN_L1          (SDLK_q)
#define BTN_R1          (SDLK_r)
#define BTN_L2          (SDLK_w)
#define BTN_R2          (SDLK_e)
#define BTN_SELECT      (SDLK_LSHIFT)
#define BTN_START       (SDLK_SPACE)
#define BTN_MENU        (SDLK_LCTRL)

#endif

#endif
