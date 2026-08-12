#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "bsp/board.h"
#include "tusb.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "audio_i2s.pio.h" // El header que genera CMake
// https://github.com/lichen-community-systems/pio-i2s/tree/main/src

#include "synth.h"

#define AUDIO_DATA_PIN 28
#define AUDIO_BCLK_PIN 26 // LRCK será el 27
#define SAMPLE_RATE 44100

// Buffers de audio estéreo
#define BUFFER_SAMPLES 256
int16_t buffer0[BUFFER_SAMPLES * 2]; 
int16_t buffer1[BUFFER_SAMPLES * 2];

int dma_channel;
volatile bool fill_buffer0 = false;
volatile bool fill_buffer1 = false;

// Variables de Síntesis
volatile float current_phase = 0.0f;
volatile float current_freq  = 440.0f;
volatile float current_amp   = 0.0f;
volatile uint8_t active_note = 0;


void process_midi() {
    while (tud_midi_available()) {
        uint8_t packet[4];
        if (tud_midi_packet_read(packet))
            SYNTH_MIDI_msg(packet);
    }
}

// Rellena el buffer con nuestro oscilador
void fill_audio_buffer(int16_t *buffer) {
    float phase_inc = 2.0f * (float)M_PI * current_freq / SAMPLE_RATE;

    for (int i = 0; i < BUFFER_SAMPLES; i++) {
        int16_t val = SYNTH_get_audio_sample();
        buffer[i * 2 + 0] = val; // Canal L
        buffer[i * 2 + 1] = val; // Canal R
    }
}

// ISR (Interrupt Service Routine) del DMA
void dma_handler() {
    dma_hw->ints0 = 1u << dma_channel; // Limpiamos la bandera de interrupción
    static bool playing_buffer0 = true;
    
    // El DMA terminó de enviar un buffer. Inmediatamente lo re-apuntamos al otro.
    if (playing_buffer0) {
        dma_channel_set_read_addr(dma_channel, buffer1, true);
        fill_buffer0 = true; // Le avisamos al main que puede sobreescribir el 0
    } else {
        dma_channel_set_read_addr(dma_channel, buffer0, true);
        fill_buffer1 = true; // Le avisamos al main que puede sobreescribir el 1
    }
    playing_buffer0 = !playing_buffer0;
}

int main() {
    board_init(); 
    tusb_init();
    SYNTH_init();

    // 1. Inicializar PIO I2S
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &audio_i2s_program);
    audio_i2s_program_init(pio, sm, offset, AUDIO_DATA_PIN, AUDIO_BCLK_PIN, SAMPLE_RATE);

    // 2. Configurar DMA
    dma_channel = dma_claim_unused_channel(true);
    dma_channel_config dma_config = dma_channel_get_default_config(dma_channel);
    
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false); // El FIFO del PIO es fijo
    channel_config_set_dreq(&dma_config, pio_get_dreq(pio, sm, true));

    dma_channel_configure(
        dma_channel,
        &dma_config,
        &pio->txf[sm], 
        buffer0,       
        BUFFER_SAMPLES, 
        false          
    );

    // 3. Activar Interrupciones
    dma_channel_set_irq0_enabled(dma_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Rellenar la "bomba" de agua inicial y encender
    fill_audio_buffer(buffer0);
    fill_audio_buffer(buffer1);
    dma_channel_start(dma_channel);

    // Loop infinito Bare-Metal
    while (1) {
        tud_task();
        process_midi();

        // Rellenamos buffers únicamente cuando el DMA nos avisa
        if (fill_buffer0) {
            fill_audio_buffer(buffer0);
            fill_buffer0 = false;
        }
        if (fill_buffer1) {
            fill_audio_buffer(buffer1);
            fill_buffer1 = false;
        }
    }

    return 0;
}