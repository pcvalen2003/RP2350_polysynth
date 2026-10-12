#include <synth.h>
#include <analog_modeling.c>
#include "pico/stdlib.h"
#include "cmsis_compiler.h"
#include <math.h>
// #include "hardware/rand.h"

#include <LUT/LFO.h>
#include <LUT/ENV.h>
#include <LUT/waveforms.h>

#ifndef SAMPLE_RATE
    #define SAMPLE_RATE 44100
#endif



// ADSR Envelope

typedef struct{
    volatile uint32_t attack;
    volatile uint32_t decay;
    volatile uint16_t sustain;
    volatile uint32_t release;
} ADSR_config_t;

ADSR_config_t vca_envelope = {
    0x0fff, 1, 0, 12,   // A D S R
};

ADSR_config_t morph_envelope = {
    64, 16, 0xffff/2, 1,    // A D S R
};



// Motor del ADSR

typedef enum{
    attack, decay, sustain, release, off
} ADSR_estate_t;

typedef struct{
    ADSR_estate_t estado;
    uint32_t count;
    uint16_t amp_to_release;
    uint16_t output;
} ADSR_engine_t;

uint16_t ADSR_advance(ADSR_engine_t* adsr, ADSR_config_t* cfg, uint32_t frame) {
    switch (adsr->estado) {
    case attack:
        if(adsr->count < 0x1ffff - cfg->attack) {
            adsr->count += cfg->attack;
            adsr->output = ENV_lut_exp[(uint8_t)(adsr->count >> 9)];
        } else {
            adsr->estado = (cfg->sustain == 0xffff)? sustain : decay;
            adsr->count = 0;
            adsr->output = 0xffff;
        }

        if(frame == 1){
            adsr->count = 0;
            adsr->estado = release;
            adsr->amp_to_release = adsr->output;
        }
        break;

    case decay:
        if(adsr->count < 0x1ffff - cfg->decay) {
            adsr->count += cfg->decay;
            adsr->output = 0xffff - (((uint32_t)(0xffff - cfg->sustain) * ENV_lut_exp[(uint8_t)(adsr->count >> 9)]) >> 16);
        } else {
            adsr->estado = sustain;
            adsr->count = 0;
            adsr->output = cfg->sustain;
        }
        //break; para que cascadee a la condición de release

    case sustain:
        if(frame == 1){
            adsr->count = 0;
            adsr->estado = release;
            adsr->amp_to_release = adsr->output;
        }
        break;

    case release:
        if(adsr->count < 0x1ffff - cfg->release) {
            adsr->count += cfg->release;
            adsr->output = adsr->amp_to_release - (((uint32_t)(adsr->amp_to_release) * ENV_lut_exp[(uint8_t)(adsr->count >> 9)]) >> 16);
        } else {
            adsr->estado = off;
            adsr->count = 0;
            adsr->output = 0;
        }
        break;

    case off:
        adsr->output = 0;
        break;
    }

    return adsr->output;

}


// Voces

#define MAX_OSCxVOICE 8

typedef struct{
    uint32_t frame;
    uint8_t note;

    uint32_t phase_acc[MAX_OSCxVOICE];
    uint32_t phase_inc;

    uint8_t velocity;

    ADSR_engine_t vca_env;
    ADSR_engine_t morph_env;
} voice_t;

// Arreglo de voces

#define MAX_VOICES 16
voice_t voices[MAX_VOICES];

uint32_t current_frame = 10;

float midi_to_freq(uint8_t note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}



// WAVETABLEs

typedef struct {
    const int16_t* LUT;

    uint8_t blend_double_freq;
    uint8_t x_warp;
    uint8_t bitcrush;
} wave_cfg;

wave_cfg wave0_cfg;
wave_cfg wave1_cfg;
uint8_t need2recalc = 0x01 | 0x02; // Necesito recalcular la wave0 (0x01) y/o la wave1 (0x02)

#define WAVE_SIZE 1024
int16_t wave0[WAVE_SIZE];
int16_t wave1[WAVE_SIZE];

// Parámetros globales

// Desviación Gaussiana
// Concentra las voces en la fundamental y expande las colas al aumentar N
int32_t osc_deviation[MAX_OSCxVOICE][MAX_OSCxVOICE] = 
{   {0},                                                 // 1 voz   (Max Z: 0)
    {440, -440},                                         // 2 voces (Max Z: 0.67)
    {0, 630, -630},                                      // 3 voces (Max Z: 0.97)
    {208, -208, 750, -750},                              // 4 voces (Max Z: 1.15)
    {0, 342, -342, 835, -835},                           // 5 voces (Max Z: 1.28)
    {137, -137, 440, -440, 901, -901},                   // 6 voces (Max Z: 1.38)
    {0, 239, -239, 516, -516, 956, -956},                // 7 voces (Max Z: 1.47)
    {102, -102, 319, -319, 579, -579, 1000, -1000}       // 8 voces (Max Z: 1.53)
};
uint8_t oscXvoice = 1;
uint8_t voices_spread = 0;
uint8_t voices_drive = 16;


uint16_t env1_depth;
uint16_t env1_bias;
uint8_t vel_to_morph = 0;


// Tablas de onda

#define LUT_SIZE 256 // hay que darle 1 muestra más a las LUTs para interpolar la última!!
extern int8_t display_wave[128];

void waveform_fill(int16_t* wavetable, wave_cfg* cfg){

    static int16_t buff0[1024];
    static int16_t buff1[1024];

    // Calculo toda la wavetable a frecuencia fundamental
    for(uint16_t i = 0; i < WAVE_SIZE; i++){
        buff0[i] = cfg->LUT[i >> 2] + (((i & 0x03) * ((int32_t)cfg->LUT[(i >> 2) + 1] - cfg->LUT[i >> 2])) >> 2);
    }

    for(uint16_t i = 0; i < WAVE_SIZE; i++){
        buff0[i] = cfg->LUT[i >> 2] + (((i & 0x03) * ((int32_t)cfg->LUT[(i >> 2) + 1] - cfg->LUT[i >> 2])) >> 2);
    }

    // Blend de doble frecuencia (buff0 -> buff1)
    for(uint16_t i = 0; i < WAVE_SIZE; i++){
        buff1[i] = ((int32_t)buff0[i] * (0xff - cfg->blend_double_freq)  +  (int32_t)buff0[(2*i) & 1023] * cfg->blend_double_freq) >> 8;
    }

    // X-Warp (buff1 -> buff0)
    for(uint16_t i = 0; i < WAVE_SIZE; i++){
        float x = (i / 511.5f) - 1.0f;
        float exponent = 1.0f + (15.0f * (cfg->x_warp / 255.0f));
        float sign = (x < 0.0f) ? -1.0f : 1.0f;
        
        uint16_t warped_i = (uint16_t)((sign*powf(fabsf(x), exponent) + 1.f) * 511.5f);
        
        // Blindaje contra errores de redondeo de punto flotante
        if(warped_i > 1023) warped_i = 1023;
        
        buff0[i] = buff1[warped_i];
    }
    
    // Bit-crusher (buff0 -> wavetable)
    for(uint16_t i = 0; i < WAVE_SIZE; i++){
        wavetable[i] = (buff0[i] >> cfg->bitcrush) << cfg->bitcrush;
    }


    // Display wave
    for(uint8_t i = 0; i < 128; i++){
        display_wave[i] = wavetable[i << 3] >> 11;
    }

}




// LFOs

typedef struct{
    volatile uint32_t phase_acc, phase_inc;
    const int16_t *LUT;
    volatile int32_t value;
} LFO_t;

LFO_t lfo_detune = {0, 486958, LFO_lut_sine, 0};

int32_t LFO_LUT(LFO_t* lfo){
    int16_t* lut = lfo->LUT;
    uint8_t k = (uint8_t)((lfo->phase_acc) >> 24);
    uint16_t t = (uint16_t)((lfo->phase_acc) >> 8);

    return (lut[k] << 5) + (int16_t)(((int32_t)(lut[k+1]-lut[k]) * t) >> 11);
}


    // FX: Delay + Chorus

#define DELAY_MASK 0xFFFF // Máscara para 65536 posiciones
static int16_t delay_line[DELAY_MASK + 1]; // 131 KB en RAM (Sección BSS)
static uint16_t delay_write_ptr = 0;

// Parámetros de control
volatile uint16_t delay_time_samples = 0; // 0 a 65535
volatile uint16_t delay_feedback = 0;     // 0 a ~30000 (Formato Q15)
volatile uint8_t  delay_mix = 0;          // 0 a 255 (Wet/Dry)

LFO_t lfo_chorus = {0, 0, LFO_lut_sine, 0}; // Usamos tu estructura de LFO
volatile uint16_t chorus_depth = 0;         // 0 a 127











// Gráficos en pantalla

void plot_curve(int16_t*, int16_t*, uint8_t);




// Funciones externas
 
void SYNTH_init(){
    for(uint8_t i = 0; i < MAX_VOICES; i++){
        voices[i].morph_env.estado = off;
        voices[i].vca_env.estado = off;
        voices[i].frame = 10;
    }

    wave0_cfg.LUT = sine256;
    wave1_cfg.LUT = sine256;
    wave1_cfg.blend_double_freq = 255;
    wave0_cfg.bitcrush = 0;
    wave1_cfg.bitcrush = 0;
}




extern volatile uint8_t parameter_to_ctrl;
#define _DO 0x01
#define _DOs 0x02
#define _RE 0x03
#define _REs 0x04
#define _MI 0x05
#define _FA 0x06
#define _FAs 0x07
#define _SOL 0x08
#define _SOLs 0x09
#define _LA 0x0A
#define _LAs 0x0B

void SYNTH_MIDI_msg(uint8_t msg[4]){
    uint8_t command = msg[1] & 0xF0;
    uint8_t note    = msg[2];
    uint8_t vel     = msg[3];
    

    if(command == 0x90 && vel > 0){
        // Note ON
        voice_t* v = &voices[0];

        for (uint8_t i = 0; i < MAX_VOICES; i++){
            if (voices[i].frame == 0){
                v = &voices[i];
                break;
            } else if (voices[i].frame < v->frame)
                v = &voices[i];
        }

        v->frame = current_frame;
        v->note = note;

        float freq = midi_to_freq(note);
        v->phase_inc = (uint32_t) (freq / SAMPLE_RATE * 0xffffffff);
        v->velocity = vel;

        // for(uint8_t i = 0; i < MAX_OSCxVOICE; i++)
        //     v->phase_acc[i] = get_rand_32();

        v->vca_env.estado = attack;
        v->vca_env.count = 0;
        v->morph_env.estado = attack;
        v->morph_env.count = 0;

    } else if (command == 0x80 || (command == 0x90 && vel == 0)) {
        // Note OFF
        for(uint8_t i = 0; i < MAX_VOICES; i++){
            if(voices[i].note == note) {
                voices[i].frame = 1;
            }
        }
    }

    if(command == 0xB0){
        if(note == 1){          // 1-Modulation Wheel
            uint16_t vel1 = vel << 9;
            uint16_t vel2 = (vel*vel) << 2;
            uint16_t vel3 = 128-vel;    // para los transitorios de los Envelope

            switch (parameter_to_ctrl) {
            case 0x10 | _DO:
                if(vel < 32)      wave0_cfg.LUT = sine256;
                else if(vel < 64) wave0_cfg.LUT = tri256;
                else if(vel < 96) wave0_cfg.LUT = saw256;
                else              wave0_cfg.LUT = sqr256;   need2recalc = 0x01;    break;
            case 0x10 | _DOs:
                wave0_cfg.x_warp = vel << 1;                need2recalc = 0x01;    break;
            case 0x10 | _RE:
                wave0_cfg.blend_double_freq = vel << 1;     need2recalc = 0x01;    break;
            case 0x10 | _SOL:
                wave0_cfg.bitcrush = (vel >> 3);            need2recalc = 0x01;    break; 

                
            case 0x30 | _DO:
                if(vel < 32)      wave1_cfg.LUT = sine256;
                else if(vel < 64) wave1_cfg.LUT = tri256;
                else if(vel < 96) wave1_cfg.LUT = saw256;
                else              wave1_cfg.LUT = sqr256;   need2recalc = 0x02;    break;
            case 0x30 | _DOs:
                wave1_cfg.x_warp = vel << 1;                need2recalc = 0x02;    break;
            case 0x30 | _RE:
                wave1_cfg.blend_double_freq = vel << 1;     need2recalc = 0x02;    break;


            // General Voices
            case 0x50 | _DO:
                oscXvoice = (vel >> 4) + 1;     break;
            case 0x50 | _DOs:
                voices_spread = vel;            break;
            case 0x50 | _RE:
                voices_drive = vel+16;          break;


            // ENV0 (VCA Envelope)
            case 0x60 | _DO: // Attack
                vca_envelope.attack = vel3;     break;
            case 0x60 | _RE: // Decay
                vca_envelope.decay = vel3;      break;
            case 0x60 | _MI: // Sustain
                vca_envelope.sustain = vel1;    break;  // este es lineal
            case 0x60 | _FA: // Release
                vca_envelope.release = vel3;    break;


            // ENV1 (Morph Envelope)
            case 0x80 | _DO: // Attack
                morph_envelope.attack = vel3;   break;
            case 0x80 | _RE: // Decay
                morph_envelope.decay = vel3;    break;
            case 0x80 | _MI: // Sustain
                morph_envelope.sustain = vel1;  break;
            case 0x80 | _FA: // Release
                morph_envelope.release = vel3;  break;

            case 0x80 | _DOs: // Bias
                env1_bias = vel1;       break;
            case 0x80 | _REs: // Depth
                env1_depth = vel1;      break;


            // FX
            case 0xA0 | _DO:
                delay_time_samples = vel * 516; break;
            case 0xA0 | _DOs:
                delay_feedback = vel * 245;     break;
            case 0xA0 | _RE:
                delay_mix = vel << 1;           break;
            case 0xA0 | _REs:
                lfo_chorus.phase_inc = // Escala el LFO de ~0.2 Hz a ~5.0 Hz
                    19000 + (vel * 3680);       break;
            case 0xA0 | _MI:
                chorus_depth = vel;             break;

            
            default:
                break;
            }

        }


    }
}

// Modificadores de envolventes
// Do  - Attack
//   Do#  - Depth (solo env1)
// Re  - Decay
//   Re#  - Bias (solo env1)
// Mi  - Sustain
// Fa  - Release
//   Fa#  - LFO1 to morph depth (solo env1)
// Sol - Velocity
//   Sol# - LFO1 to morph speed (solo env1)
// La  - 
//   La#  - 

// Modificadores de las tablas de onda:
// 1. Blend de doble frecuencia 
//      LUT[i] * (1-mix) + LUT[(i * 2) % 1024] * mix
// 2. X Warp (Phase Distortion)
//      i_nuevo = 1024 * (i/1024)^n
// 3. Formantes por Ventana (Windowed Sync)
//      Consiste en generar un seno de alta frecuencia (4 o 5 ciclos completos en 1024 puntos) que representa el formante, y 
// multiplicarla por una "ventana" de amplitud con forma de campana (Hanning o Gauss) que empieza en 0, sube al medio y termina
// en 0 en la posición 1023. Altera la frecuencia de esa senoidal interna y vas a mutar entre vocales ("A", "O", "U").
// 4. Wavefolding
// 5. Pulse Width (o pseudo Hard Sync)
//      wavetable[i] = LUT[(i * sync_factor) % 1024] para i * sync_factor < 1024,
//      wavetable[i] = 0  c.c.   ,  con 1 < sync_factor < 2
// La onda se comprime y reproduce un ciclo y monedas dentro de los 1024 puntos, cayendo a cero abruptamente al final.
// 6. Overdrive???
// 7. Bit-crusher
//      wavetable[i] = (LUT[i] >> reduccion) << reduccion

// Modificadores generales
// 1. OSCxVOZ
// 2. OSC spread
// 3. Vibrato speed (LFO0)
// 4. Vibrato depth (separado por voz?)
// 5. Vibrato attack
// 6. Portamento (polifónico?)

// FX
// 1. Drive
// 2. Delay time
// 3. Delay feedback
// 4. LP
// 5. HP
// 6. Chorus  (depth + 7. speed)
//      Basado en una línea de retardo corta modulada por LFO3
// 8. Tremolo (depth + 9. speed)
// 10. 
// 11. 


#ifndef SAMPLES
    #define SAMPLES 256
#endif

// Compensación de compromiso ( 1 / N^0.75 )
// Equilibrio entre conservación de volumen (RMS) y prevención de recortes (Headroom)
const uint32_t poly_compensate[] = {
    0x00010000, // 1 voz   (1.000)
    0x00009837, // 2 voces (0.594)
    0x0000704D, // 3 voces (0.438)
    0x00005A82, // 4 voces (0.353)
    0x00004C90, // 5 voces (0.299)
    0x000042C6, // 6 voces (0.260)
    0x00003B4D, // 7 voces (0.231)
    0x0000355A  // 8 voces (0.208)
};

void SYNTH_fill_audio_buffers(int16_t* buffer){
    current_frame++;

    if(need2recalc != 0){
        if(need2recalc & 0x01) waveform_fill(wave0, &wave0_cfg);
        if(need2recalc & 0x02) waveform_fill(wave1, &wave1_cfg);

        need2recalc = 0;
        //return;
    }

    // Crear y limpiar buffer local
    int32_t buff[SAMPLES];
    for(uint16_t i = 0; i < SAMPLES; i++)
        buff[i] = 0;

    // Para cada voz, calcular las 256 muestras
    for(uint8_t k = 0; k < MAX_VOICES; k++){
        voice_t* v = &voices[k];
        if(v->frame == 0) continue; // voz apagada

        uint16_t amp,       morph;
        uint32_t morph_raw;
        int32_t  wave0_sum, wave1_sum;
        int32_t  voice_out;
        
        // Desviación local de osciladores
        int32_t  osc_dev_local[oscXvoice];
        for(uint8_t i = 0; i < oscXvoice; i++)
            osc_dev_local[i] = ((((v->phase_inc >> 8) * osc_deviation[oscXvoice-1][i]) >> 12) * (voices_spread + 16)) >> 8;


        for(uint16_t i = 0; i < SAMPLES; i++){
            amp =   ADSR_advance(&v->vca_env,   &vca_envelope,   v->frame);
            if(v->vca_env.estado == off){ v->frame = 0; break; }
            morph_raw = ADSR_advance(&v->morph_env, &morph_envelope, v->frame)*env1_depth;
            morph_raw = (morph_raw >> 16) + env1_bias;
            morph = (uint16_t)__USAT(morph_raw, 16);

            wave0_sum = 0;
            wave1_sum = 0;
            for(uint8_t j = 0; j < oscXvoice; j++){
                v->phase_acc[j] += v->phase_inc + osc_dev_local[j];

                wave0_sum += wave0[v->phase_acc[j] >> 22]; // OJO CON EL WAVE_SIZE
                wave1_sum += wave1[v->phase_acc[j] >> 22];
            }

            wave0_sum = (int32_t)(((int64_t)wave0_sum * poly_compensate[oscXvoice-1]) >> 16);
            wave1_sum = (int32_t)(((int64_t)wave1_sum * poly_compensate[oscXvoice-1]) >> 16);

            // Morph
            voice_out = (((int64_t)wave1_sum * morph) >> 17) + (((int64_t)wave0_sum * (0xffff-morph)) >> 17);
            // VCA
            voice_out = (voice_out * amp) >> 16;

            // Voice Drive
            voice_out = soft_clip((voice_out * voices_drive) >> 4); 

            buff[i] += voice_out;
        }

    }

    // Llenar buffers de audio
    for(uint16_t i = 0; i < SAMPLES; i++){
        int16_t dry_sample = (int16_t)__SSAT(buff[i] >> 3, 16); // Saturación a int16

        uint16_t delay_read_ptr = (delay_write_ptr - delay_time_samples) & DELAY_MASK;
        int16_t wet_sample = delay_line[delay_read_ptr];

        int32_t feedback_in = dry_sample + ((wet_sample * delay_feedback) >> 15);
        delay_line[delay_write_ptr] = (int16_t)__SSAT(feedback_in, 16);

        int32_t out_sample = ((dry_sample * (255 - delay_mix)) + (wet_sample * delay_mix)) >> 8;
        int16_t final_out = (int16_t)__SSAT(out_sample, 16);

        buffer[2*i    ] = final_out;   // Canal L
        buffer[2*i + 1] = final_out;   // Canal R

        delay_write_ptr = (delay_write_ptr + 1) & DELAY_MASK;
    }
}

