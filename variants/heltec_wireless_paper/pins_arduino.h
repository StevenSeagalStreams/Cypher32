// Pin map for the Heltec Wireless Paper (ESP32-S3 + SX1262 + 250x122 e-ink).
//
// The Arduino core only gained a variant for this board in 3.x, and this
// project builds against the 2.x core that platform-espressif32 ships — so
// without this file the board has no pin definitions at all. The values are
// the published ones for the board; WIRELESS_PAPER and Vext are the two that
// anything actually depends on, the rest are here so ordinary Arduino sketch
// idioms (SS, SDA, analog pin names) behave the way they do everywhere else.
//
// The SPI four — 8, 9, 10, 11 — are the same pins cypher32_packets.h and
// cypher32_lora.h name for the SX1262, which is a useful independent check on
// that mapping.
#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

#define WIRELESS_PAPER true

static const uint8_t LED_BUILTIN = 18;
#define BUILTIN_LED LED_BUILTIN
static const uint8_t KEY_BUILTIN = 0;     // the PRG button the page cycle uses

static const uint8_t Vext = 45;           // display and peripheral power rail

static const uint8_t TX = 43;
static const uint8_t RX = 44;

static const uint8_t SDA = 21;
static const uint8_t SCL = 22;

static const uint8_t SS   = 8;            // = LORA_NSS
static const uint8_t SCK  = 9;            // = LORA_SCK
static const uint8_t MOSI = 10;           // = LORA_MOSI
static const uint8_t MISO = 11;           // = LORA_MISO

static const uint8_t A0 = 1;   static const uint8_t A1 = 2;
static const uint8_t A2 = 3;   static const uint8_t A3 = 4;
static const uint8_t A4 = 5;   static const uint8_t A5 = 6;
static const uint8_t A6 = 7;   static const uint8_t A7 = 8;

#endif /* Pins_Arduino_h */
