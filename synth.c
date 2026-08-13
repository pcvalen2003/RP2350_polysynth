#include <synth.h>
#include <analog_modeling.c>

#include <LUT/LFO.h>
#include <LUT/ENV.h>

#ifndef SAMPLE_RATE
    #define SAMPLE_RATE 44100
#endif



// ADSR Envelope

typedef struct{
    volatile uint32_t attack;
    volatile uint32_t decay;
    volatile uint16_t sustain;
    volatile uint32_t release;

    volatile uint16_t depth;
    volatile uint16_t bias;
} ADSR_config_t;

ADSR_config_t vca_envelope = {
    16, 8, 0xffff, 1, 
    0xffff, 0
};


ADSR_config_t vcf_envelope = {
    64, 16, 0xffff/2, 1, 
    0xffff/2, 0xffff/2
};

int32_t resonance   = 28000;    // [0, 1] Q15



// Motor del ADSR

typedef enum{
    att, dec, sus, rel, off
} ADSR_estate_t;

typedef struct{
    volatile ADSR_estate_t estado;
    volatile uint32_t count;
    volatile uint16_t amp_to_release;
    volatile uint16_t output;
} ADSR_engine_t;

uint16_t ADSR_advance(ADSR_engine_t* adsr, ADSR_config_t* cfg, uint32_t frame) {
    switch (adsr->estado) {
    case att:
        if(adsr->count < 0x1ffff - cfg->attack) {
            adsr->count += cfg->attack;
            adsr->output = ENV_lut_exp[(uint8_t)(adsr->count >> 9)];
        } else {
            adsr->estado = (cfg->sustain == 0xffff)? sus : dec;
            adsr->count = 0;
            adsr->output = 0xffff;
        }
        break;

    case dec:
        if(adsr->count < 0x1ffff - cfg->decay) {
            adsr->count += cfg->decay;
            adsr->output = 0xffff - (((uint32_t)(0xffff - cfg->sustain) * ENV_lut_exp[(uint8_t)(adsr->count >> 9)]) >> 16);
        } else {
            adsr->estado = sus;
            adsr->count = 0;
            adsr->output = cfg->sustain;
        }
        break;

    case sus:
        if(frame == 1){
            adsr->estado = rel;
            adsr->amp_to_release = adsr->output;
        }
        break;

    case rel:
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

    // Depth & bias
    uint32_t out = (((uint32_t)adsr->output * cfg->depth) >> 16) + cfg->bias;

    if(out >= 0xffff)
        return 0xffff;
    else
        return (uint16_t) out;

}


// Voces

typedef enum{saw, tri, square, pulse} osc_shape_t;
volatile osc_shape_t shape;     // sierra-triangular / cuadrada
volatile uint8_t shaping;       // hipersaw - metalizer - PW - XOR

typedef struct{
    volatile uint32_t frame; // <-- acá guardo cuándo se encendió el oscilador! 
    volatile int32_t phase_acc;
    volatile uint32_t phase_inc;
    volatile uint8_t current_note;

    ADSR_engine_t vca_env;
    volatile uint16_t current_amp;

    uint16_t metalic;

    // --- Filtro Sallen-Key ---
    ADSR_engine_t vcf_env;
    volatile int32_t cutoff;    // rango seguro 100 a 16.000 ?
    volatile int32_t s1; // Capacitor del polo 1
    volatile int32_t s2; // Capacitor del polo 2
} voice_t;


#define MAX_VOICES 8
voice_t voices[MAX_VOICES];

volatile uint32_t current_frame = 0;

float midi_to_freq(uint8_t note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
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



void plot_curve(int8_t*);
int8_t wave[128];

// Funciones externas

void SYNTH_init(){
    for(uint8_t i = 0; i < MAX_VOICES; i++){
        voices[i].vcf_env.estado = off;
        voices[i].vca_env.estado = off;
        voices[i].current_amp = 0;
        voices[i].cutoff = 100;
        voices[i].frame = 0;
    }
}


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

        v->current_note = note;
        v->frame = current_frame;

        float freq = midi_to_freq(note);
        v->phase_inc = (uint32_t) (freq / SAMPLE_RATE * 0xffffffff);

        v->vca_env.estado = att;
        v->vca_env.count = 0;
        v->vcf_env.estado = att;
        v->vcf_env.count = 0;

        v->metalic = (uint16_t)shaping << 8;

    } else if (command == 0x80 || (command ==0x90 && vel == 0)) {
        // Note OFF
        for(uint8_t i = 0; i < MAX_VOICES; i++){
            if(voices[i].current_note == note) {
                voices[i].frame = 1;
            }
        }
    }

    if(command == 0xB0){
        if(note == 1){          // 1-Modulation 
            shaping = vel * 2;

            for(uint8_t i = 0; i < 128; i++)
                wave[i] = ((i/2 - 32) * shaping) >> 8;

            plot_curve(wave);

        } //else if (note == 2){  // 2-Breath 
        //     resonance = vel * 300;
        // }


    }
}



const int16_t wavefolder[] = {
    0, 324, 649, 973, 1298, 1622, 1947, 2271, 
    2595, 2920, 3244, 3569, 3893, 4218, 4542, 4866, 
    5191, 5515, 5840, 6164, 6489, 6813, 7137, 7462, 
    7786, 8111, 8435, 8760, 9084, 9408, 9733, 10057, 
    10382, 10706, 11030, 11355, 11679, 12004, 12328, 12653, 
    12977, 13301, 13626, 13950, 14275, 14599, 14924, 15248, 
    15572, 15897, 16221, 16546, 16870, 17195, 17519, 17843, 
    18168, 18492, 18817, 19141, 19466, 19790, 20114, 20439, 
    20763, 21088, 21412, 21737, 22061, 22385, 22710, 23034, 
    23359, 23683, 24008, 24332, 24656, 24981, 25305, 25630, 
    25954, 26278, 26603, 26927, 27252, 27576, 27901, 28225, 
    28549, 28874, 29198, 29523, 29847, 30172, 30496, 30820, 
    31145, 31469, 31794, 32118, 32443, 32767, 32439, 32112, 
    31784, 31456, 31129, 30801, 30473, 30146, 29818, 29490, 
    29163, 28835, 28507, 28180, 27852, 27524, 27197, 26869, 
    26541, 26214, 25886, 25558, 25231, 24903, 24576, 24248, 
    23920, 23593, 23265, 22937, 22610, 22282, 21954, 21627, 
    21299, 20971, 20644, 20316, 19988, 19661, 19333, 19005, 
    18678, 18350, 18022, 17695, 17367, 17039, 16712, 16384, 
    16712, 17039, 17367, 17695, 18022, 18350, 18678, 19005, 
    19333, 19661, 19988, 20316, 20644, 20971, 21299, 21627, 
    21954, 22282, 22610, 22937, 23265, 23593, 23920, 24248, 
    24576, 24903, 25231, 25558, 25886, 26214, 26541, 26869, 
    27197, 27524, 27852, 28180, 28507, 28835, 29163, 29490, 
    29818, 30146, 30473, 30801, 31129, 31456, 31784, 32112, 
    32439, 32767, 32439, 32112, 31784, 31456, 31129, 30801, 
    30473, 30146, 29818, 29490, 29163, 28835, 28507, 28180, 
    27852, 27524, 27197, 26869, 26541, 26214, 25886, 25558, 
    25231, 24903, 24576, 24248, 23920, 23593, 23265, 22937, 
    22610, 22282, 21954, 21627, 21299, 20971, 20644, 20316, 
    19988, 19661, 19333, 19005, 18678, 18350, 18022, 17695, 
    17367, 17039, 16712, 16384, 16056, 15729, 15401, 15073
};


int16_t SYNTH_get_audio_sample(){
    int16_t buff = 0;

    current_frame++;
    
    // LFO
    lfo_detune.phase_acc += lfo_detune.phase_inc;
    lfo_detune.value = LFO_LUT(&lfo_detune);

    // Voces
    for(uint8_t i = 0; i < MAX_VOICES; i++){
        voice_t* v = &voices[i];
        if(v->frame == 0) continue; // voz apagada


        // Forma de onda
        v->phase_acc += v->phase_inc;// + lfo_detune.value;
            
        int32_t p = v->phase_acc;
        int16_t raw_saw = (int16_t)(p >> 17); // 16 + 1


        int16_t raw_tri = (int16_t)(((p ^ (p >> 31)) >> 15) - 32768);

        uint16_t gain = 256 + ((v->metalic >> 8) * 6);
        if(v->metalic > 0)
            v->metalic--;
        
        // triángulo amplificado (excedido del rango de int16_t)
        int32_t driven_tri = (raw_tri * gain) >> 8; 
        
        int32_t sign = driven_tri >> 31;
        // valor absoluto branchless
        int32_t abs_driven = (driven_tri ^ sign) - sign;

        int16_t raw_wave = (wavefolder[(uint8_t)(abs_driven >> 9)] ^ sign) - sign; 


        // VCF
        
        // VCF Envelope (Filter)
        uint16_t vcf_adsr = ADSR_advance(&v->vcf_env, &vcf_envelope, v->frame);
        v->cutoff = 100 + ((uint32_t)vcf_adsr * 8000) / 65535; // Cutoff máximo ~ 8100

        // Feedback global (Resonancia) extraído del último capacitor (s2)
        int32_t feedback = (voices[i].s2 * resonance) >> 15; // Q15
        // Mezcla de entrada y saturación en el Op-Amp virtual
        int32_t mixed_in = soft_clip(raw_wave - feedback);

        // Polo 1: s1[n] = s1[n-1] + f * (input - s1[n-1])
        voices[i].s1 += (v->cutoff * (mixed_in - voices[i].s1)) >> 15;
        // Polo 2: s2[n] = s2[n-1] + f * (s1[n] - s2[n-1])
        voices[i].s2 += (v->cutoff * (voices[i].s1 - voices[i].s2)) >> 15;


        // VCA
        // VCA Envelope
        v->current_amp = ADSR_advance(&v->vca_env, &vca_envelope, v->frame);

        int32_t vca_out = ((int32_t)v->s2 * voices[i].current_amp) >> 15; // 16

        if (v->vca_env.estado == off) // condición de apagado
            v->frame = 0;



        buff += vca_out >> 3; // headroom
    }

    return buff;
}
