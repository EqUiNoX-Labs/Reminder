#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t midi_note;   /* 0 = rest */
    uint16_t duration_ms;
    uint8_t velocity;    /* 1–127 */
} tune_note_t;

extern const tune_note_t classical_tune[];
extern const size_t classical_tune_count;
