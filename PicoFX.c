#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "bsp/board.h"
#include "tusb.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "audio_i2s.pio.h" // El header que genera CMake
// https://github.com/lichen-community-systems/pio-i2s/tree/main/src

#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "ssd1306.h"
#include "pico/time.h"
#include "pico/multicore.h"

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


    // Display OLED I2C //

#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
ssd1306_t display;

void setup_oled_i2c() {
    // puerto i2c0 a 400 kHz (Fast Mode, ideal para el OLED)
    i2c_init(i2c0, 400 * 1000);

    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);

    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    
    // inicialización SSD1306
    ssd1306_init(&display, 128, 64, 0x3c, i2c0);
    ssd1306_clear(&display);

    ssd1306_draw_square(&display, 10, 10, 108, 44);
    ssd1306_show(&display);
}


int8_t* w;
bool need2plot;
absolute_time_t last_plot_time;

void plot_curve(int8_t* wave){
    w = wave;
    need2plot = true;
}

void do_the_plot(){
    if (absolute_time_diff_us(last_plot_time, get_absolute_time()) > 50000) {
        ssd1306_clear(&display);

        for(uint8_t i = 0; i < 128; i++)
            ssd1306_draw_pixel(&display, i, 32-w[i]);

        ssd1306_show(&display);

        // Actualizamos el tiempo de la última vez que dibujamos
        last_plot_time = get_absolute_time(); 
        
        need2plot = false;
    }
}


void core1_main() {
    // Inicializamos el display acá, manejado por el núcleo secundario
    setup_oled_i2c(); 

    while (1) {
        if(need2plot)
            do_the_plot();
        
        
        // Dormimos el Core 1 un ratito para no saturar el bus de memoria de la Pico
        sleep_ms(15); 
    }
}





int main() {
    board_init(); 
    tusb_init();
    SYNTH_init();

    //setup_oled_i2c(); <-- lo hace el core 1

    // inicializar PIO I2S
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &audio_i2s_program);
    audio_i2s_program_init(pio, sm, offset, AUDIO_DATA_PIN, AUDIO_BCLK_PIN, SAMPLE_RATE);

    // configurar DMA
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

    // activar interrupciones
    dma_channel_set_irq0_enabled(dma_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    multicore_launch_core1(core1_main); // lanzar el core 1

    fill_audio_buffer(buffer0);
    fill_audio_buffer(buffer1);
    dma_channel_start(dma_channel);


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