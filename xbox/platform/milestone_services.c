/* Unported services remain unresolved: the native loader reports the exact first import.
 * These are host link adapters, not successful guest implementations. */
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#define UNPORTED(name) uintptr_t runtime_##name##_resolve(const char *nid) { (void)nid; return 0; }
#ifndef UNCHARTED_LINK_RENDERER
UNPORTED(audio)
UNPORTED(ajm)
UNPORTED(videodec)
UNPORTED(pad)
void runtime_audio_report(void) { puts("UWP milestone: audio not ported"); }
void runtime_ajm_report(void) { puts("UWP milestone: media decoding not ported"); }
void runtime_pad_report(void) { puts("UWP milestone: guest controller API not ported"); }
int runtime_pad_describe(void) { return 0; }
void bbgpu_register_kernel(void) { puts("UWP milestone: GPU and GPU event queues not ported"); }
int bbgpu_init(const BbGpuConfig *config) { (void)config; return -1; }
uintptr_t bbgpu_resolve(const char *nid) { (void)nid; return 0; }
int bbgpu_handle_fault(void *context,void *address) { (void)context; (void)address; return 0; }
void bbgpu_dump_guest_writes(void *context) { (void)context; }
unsigned bbgpu_symbol_count(void) { return 0; }
int bbgpu_text_input_begin(const char *text,const char *prompt) { (void)text; (void)prompt; return 0; }
int bbgpu_text_input_poll(char *text,uint64_t size) { (void)text; (void)size; return 2; }
int bbgpu_choice_begin(const char *title,const char *const *items,const char *const *images,int count,int focus) { (void)title; (void)items; (void)images; (void)count; (void)focus; return 0; }
int bbgpu_choice_poll(void) { return -2; }
int bbgpu_overlay_captures_input(void) { return 0; }
/* Optional overlay hooks are never installed in --cpu-only mode. */
float ABI bbport_orbs_gain(void *player,float value) { (void)player; return value; }
void bbport_orbs_status(int supported) { (void)supported; }
uintptr_t bbport_cheat_flag(unsigned id) { (void)id; return 0; }
void bbport_cheat_status(unsigned id) { (void)id; }
uintptr_t bbport_actor_slots_address(void) { return 0; }
void bbport_actor_status(int supported) { (void)supported; }
uintptr_t bbport_damage_multiplier(int taken) { (void)taken; return 0; }
uintptr_t bbport_orb_pickup_multiplier(unsigned kind) { (void)kind; return 0; }
void bbport_stat_multipliers_status(int damage,int pickup) { (void)damage; (void)pickup; }
#endif
int vulkan_smoke(void) { fputs("Use the UWP graphics probe mode\n",stderr); return 21; }
