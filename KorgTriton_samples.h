#pragma once
#include <Arduino.h>
#include <synth_wavetable.h>
#include <cstdint>

extern const AudioSynthWavetable::sample_data KorgTriton_samples[26];
PROGMEM const uint8_t KorgTriton_ranges[] = {24, 30, 35, 40, 43, 46, 51, 52, 55, 58, 62, 64, 68, 71, 73, 75, 78, 80, 83, 85, 88, 90, 92, 95, 100, 127, };

PROGMEM const AudioSynthWavetable::instrument_data KorgTriton = {26, KorgTriton_ranges, KorgTriton_samples };


extern const uint32_t sample_0_KorgTriton_SC55PianoC1n[39680];

extern const uint32_t sample_1_KorgTriton_SC55PianoC2n[43776];

extern const uint32_t sample_2_KorgTriton_SC55PianoC2n[43776];

extern const uint32_t sample_3_KorgTriton_SC55PianoF2n[55424];

extern const uint32_t sample_4_KorgTriton_SC55PianoA2n[49024];

extern const uint32_t sample_5_KorgTriton_SC55PianoE3n[34176];

extern const uint32_t sample_6_KorgTriton_SC55PianoE3n[34176];

extern const uint32_t sample_7_KorgTriton_SC55PianoA3n[38144];

extern const uint32_t sample_8_KorgTriton_SC55PianoA3n[38144];

extern const uint32_t sample_9_KorgTriton_SC55PianoC4n[32128];

extern const uint32_t sample_10_KorgTriton_SC55PianoE4n[20224];

extern const uint32_t sample_11_KorgTriton_SC55PianoG4n[31360];

extern const uint32_t sample_12_KorgTriton_SC55PianoG4n[31360];

extern const uint32_t sample_13_KorgTriton_SC55PianoC5n[30464];

extern const uint32_t sample_14_KorgTriton_SC55PianoC5n[30464];

extern const uint32_t sample_15_KorgTriton_SC55PianoE5n[21248];

extern const uint32_t sample_16_KorgTriton_SC55PianoG5n[20480];

extern const uint32_t sample_17_KorgTriton_SC55PianoG5n[20480];

extern const uint32_t sample_18_KorgTriton_SC55PianoC6n[21504];

extern const uint32_t sample_19_KorgTriton_SC55PianoE6n[15872];

extern const uint32_t sample_20_KorgTriton_SC55PianoG6n[23808];

extern const uint32_t sample_21_KorgTriton_SC55PianoG6n[23808];

extern const uint32_t sample_22_KorgTriton_SC55PianoC7n[11008];

extern const uint32_t sample_23_KorgTriton_SC55PianoC7n[11008];

extern const uint32_t sample_24_KorgTriton_SC55PianoC1n[39680];

extern const uint32_t sample_25_KorgTriton_SC55PianoE6n[15872];
