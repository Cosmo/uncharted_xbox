#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// The View button in uncharted_uwp_dialog_gamepad's buttons: the port's menus, never the game.
#define UNCHARTED_UWP_VIEW 0x80000000u
// The PS4 pad's buttons and axes for the game; touch: a finger on the touchpad (View + left stick)
// from 0 to 1 from the top left, or -1 for none.
int uncharted_uwp_gamepad(uint32_t* buttons,uint8_t axes[6],float touch[2]);
int uncharted_uwp_dialog_gamepad(uint32_t* buttons,uint8_t axes[6]);
int uncharted_uwp_dialog_key(uint32_t key,int down,int repeat);
void uncharted_uwp_vibration(uint8_t large_motor,uint8_t small_motor);
#ifdef __cplusplus
}
#endif
