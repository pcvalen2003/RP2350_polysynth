#ifndef SYNTH_H
#define SYNTH_H

#include <stdint.h>
#include <stdio.h>
#include <math.h>

void SYNTH_init();

void SYNTH_MIDI_msg(uint8_t msg[4]);

int16_t SYNTH_get_audio_sample();

#endif