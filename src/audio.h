#ifndef AUDIO_H
#define AUDIO_H

#include "types.h"

void audio_init(audio_state_t* audio);
void audio_clear_voices(audio_state_t* audio);
void audio_step(audio_state_t* audio, float dt);
void audio_generate_stream(audio_state_t* audio);

void audio_play_mining_tick(audio_state_t* audio);
void audio_play_fracture(audio_state_t* audio, asteroid_tier_t parent_tier);
void audio_play_pickup(audio_state_t* audio, float ore, int fragments);
void audio_play_dock(audio_state_t* audio);
void audio_play_launch(audio_state_t* audio);
void audio_play_sale(audio_state_t* audio);
void audio_play_repair(audio_state_t* audio);
void audio_play_upgrade(audio_state_t* audio, ship_upgrade_t upgrade);
void audio_play_damage(audio_state_t* audio, float damage);

#endif
