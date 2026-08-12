#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#define CFG_TUSB_RHPORT0_MODE    OPT_MODE_DEVICE
#define CFG_TUD_MIDI             1

// Buffers para transmisión y recepción (32-64 bytes son suficientes)
#define CFG_TUD_MIDI_RX_BUFSIZE  64
#define CFG_TUD_MIDI_TX_BUFSIZE  64

#endif