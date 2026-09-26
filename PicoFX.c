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


#include "hardware/uart.h"

#define UART_ID uart1
#define BAUD_RATE 31250
#define UART_TX_PIN 4
#define UART_RX_PIN 5


#define CFG_BTN_PIN 3

// Buffers de audio estéreo
#define BUFFER_SAMPLES 256
int16_t buffer0[BUFFER_SAMPLES * 2]; 
int16_t buffer1[BUFFER_SAMPLES * 2];

int dma_channel;
volatile bool fill_buffer0 = false;
volatile bool fill_buffer1 = false;


volatile uint8_t midi_note_to_ctrl; // 0: limpio, [1 2 .. 12]: do, do# ... si

volatile uint8_t last_ctrl_change;

void process_midi(uint8_t packet[4]) {
    if(!gpio_get(CFG_BTN_PIN) && (packet[1] & 0xF0) == 0x90)
        midi_note_to_ctrl = (packet[2] % 12) + 1;
    else {
        SYNTH_MIDI_msg(packet);

        if((packet[1] & 0xF0) == 0xB0 && packet[2] == 1) // guardar último mensaje de control
            last_ctrl_change = packet[3];
    }
}


// Handlers MIDI hardware (USB & UART)

void process_USB_midi(){
    while (tud_midi_available()) {
        uint8_t packet[4];

        if (tud_midi_packet_read(packet)){
            process_midi(packet);
        }
    }
}

void process_uart_midi() {
    static uint8_t serial_midi_msg[4] = {0, 0, 0, 0};
    static uint8_t serial_midi_state = 0;

    while (uart_is_readable(UART_ID)) {
        uint8_t byte = uart_getc(UART_ID);

        if (byte >= 0x80) { 
            if (byte >= 0xF8) continue; 

            serial_midi_msg[1] = byte;
            serial_midi_state = 1;
        } 
        else if (serial_midi_state == 1) { 
            serial_midi_msg[2] = byte;
            serial_midi_state = 2;
        } 
        else if (serial_midi_state == 2) { 
            serial_midi_msg[3] = byte;
            
            process_midi(serial_midi_msg);
            
            // (MIDI running status)
            serial_midi_state = 1; 
        }
    }
}



void fill_audio_buffer(int16_t *buffer) {
    SYNTH_fill_audio_buffers(buffer);
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

#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9
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

    ssd1306_show(&display);
}


int8_t display_wave[128];


void plot_waveform(){
    for(uint8_t i = 0; i < 128; i++)
        ssd1306_draw_pixel(&display, i, 49 - display_wave[i]);
}

enum {
    main_menu, wave0_menu, empty1,  wave1_menu, empty2, empty3, env0_menu, empty4, env1_menu
} menu;

volatile uint8_t parameter_to_ctrl;

const char* titulos_menu[] = {"Main menu", "Wave 0", "Empty1", "Wave 1", "Empty2", "Empty3", "Envelope 0", "Empty4", "Envelope 1"};
const char* titulos_cfg[] = {
    "Wave0 waveform", "Wave0 X-warp", "Wave0 double freq", "Wave0 par3", "Wave0 par4", "Wave0 par5", 
    "Wave0 par6", "Wave0 par7", "Wave0 par8", "Wave0 par9", "Wave0 par10", "", "", "", "", "",

    "none", "none", "none", "none", "none", "none", "none", "none", "none", "none", "none",
    "", "", "", "", "",

    "Wave1 waveform", "Wave1 X-warp", "Wave1 double freq", "Wave1 par3", "Wave1 par4", "Wave1 par5", 
    "Wave1 par6", "Wave1 par7", "Wave1 par8", "Wave1 par9", "Wave1 par10", "", "", "", "", "",

    "none", "none", "none", "none", "none", "none", "none", "none", "none", "none", "none",
    "", "", "", "", "",

    "none", "none", "none", "none", "none", "none", "none", "none", "none", "none", "none",
    "", "", "", "", "",

    "Env0 attack", "env0", "Env0 decay", "env0", "Env0 sustain", "Env0 release", "env0",
    "env0", "env0", "env0", "env0", "", "", "", "", "",

    "none", "none", "none", "none", "none", "none", "none", "none", "none", "none", "none",
    "", "", "", "", "",
    
    "Env1 attack", "env1", "Env1 decay", "env1", "Env1 sustain", "Env1 release", "env1",
    "Env1 Depth", "env1", "Env1 Bias", "env1", "", "", "", "", "",
};

void core1_main() {
    // Inicializamos el display acá, manejado por el core 1
    setup_oled_i2c(); 

    while (1) {
        if(midi_note_to_ctrl != 0){
            uint8_t ctrl = midi_note_to_ctrl;
            midi_note_to_ctrl = 0;

            switch (menu) {
            case main_menu:
                switch (ctrl) {
                case 1: // Do
                    menu = wave0_menu;  break;
                case 3: // Re
                    menu = wave1_menu;  break;
                case 6: // Fa
                    menu = env0_menu;   break;
                case 8: // Sol
                    menu = env1_menu;   break;
                
                default:    break;
                }   

                break; // main_menu
            
            // Para el resto de los casos, simplemente paso al sinte el parámetro que va a ser modificado 
            default:
                if(ctrl == 12) menu = main_menu;
                else           parameter_to_ctrl = ((uint8_t)menu << 4) | ctrl;
                break;
        
            }

            ssd1306_clear_square(&display, 0, 0, 128, 32);
        
            ssd1306_draw_string(&display, 0, 0, 1, titulos_menu[(uint8_t) menu]);
            if(parameter_to_ctrl != 0 && menu != main_menu)
                ssd1306_draw_string(&display, 0, 9, 1, titulos_cfg[parameter_to_ctrl - 0x11]);

        }

        ssd1306_clear_square(&display, 0, 19, 128, 64-19);
        plot_waveform();

        char s[5];
        snprintf(s, 5, "%d", last_ctrl_change);
        ssd1306_draw_string(&display, 20, 19, 1, s);

        ssd1306_show(&display);
        
        // Dormimos el Core 1 un poco para no saturar el bus de memoria
        sleep_ms(50);
    }
}



void setup_midi_uart() {
    uart_init(UART_ID, BAUD_RATE);

    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);

    uart_set_fifo_enabled(UART_ID, true);
}


int main() {
    board_init(); 
    tusb_init();
    SYNTH_init();
    
    setup_midi_uart();

    //setup_oled_i2c(); <-- lo hace el core 1
    // Botón de configuración
    gpio_init(CFG_BTN_PIN);
    gpio_set_dir(CFG_BTN_PIN, GPIO_IN);
    gpio_pull_up(CFG_BTN_PIN);


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
        process_USB_midi();
        process_uart_midi();

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