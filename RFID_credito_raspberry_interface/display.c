/* Display 4 digitos 7 segmentos (anodo comum) via 74HC595 - Freenove.
 * O display e multiplexado: so um digito acende por vez, entao precisa ser
 * redesenhado continuamente (~200Hz). Uma thread faz isso; o resto do programa
 * so chama display_text() / display_number() quando o valor muda. */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <wiringPi.h>

#define DISP_DATA   22   /* DS    (pino 14 do 74HC595) */
#define DISP_LATCH  27   /* ST_CP (pino 12) */
#define DISP_CLOCK  17   /* SH_CP (pino 11) */

static const unsigned char disp_digits[16] = {
    0xc0,0xf9,0xa4,0xb0,0x99,0x92,0x82,0xf8,0x80,0x90,0x88,0x83,0xc6,0xa1,0x86,0x8e
}; /* 0-F, anodo comum: bit 0 = segmento aceso */

static pthread_mutex_t disp_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char disp_buf[4] = { 0xff, 0xff, 0xff, 0xff };

static unsigned char disp_glyph(char c) {
    if (c >= '0' && c <= '9') return disp_digits[c - '0'];
    if (c >= 'A' && c <= 'F') return disp_digits[c - 'A' + 10];
    if (c >= 'a' && c <= 'f') return disp_digits[c - 'a' + 10];
    if (c == '_') return 0xf7;
    if (c == '-') return 0xbf;
    return 0xff; /* apagado */
}

static void disp_out(int data) {
    digitalWrite(DISP_LATCH, LOW);
    for (int i = 15; i >= 0; i--) {  /* MSB primeiro: byte alto = digito, baixo = segmentos */
        digitalWrite(DISP_CLOCK, LOW);
        digitalWrite(DISP_DATA, (data >> i) & 1);
        delayMicroseconds(10);
        digitalWrite(DISP_CLOCK, HIGH);
        delayMicroseconds(10);
    }
    digitalWrite(DISP_LATCH, HIGH);
}

static void *disp_loop(void *arg) {
    (void)arg;
    for (;;) {
        for (int d = 0; d < 4; d++) {
            pthread_mutex_lock(&disp_lock); unsigned char seg = disp_buf[d]; pthread_mutex_unlock(&disp_lock);
            disp_out(seg | ((1 << d) << 8));
            usleep(1500);
        }
    }
    return NULL;
}

/* Mostra ate 4 caracteres alinhados a esquerda (ex.: codigo do jogador "ABC"). */
static void display_text(const char *s) {
    pthread_mutex_lock(&disp_lock);
    for (int i = 0; i < 4; i++) disp_buf[i] = disp_glyph(s && s[i] ? s[i] : ' ');
    pthread_mutex_unlock(&disp_lock);
}

/* Mostra numero alinhado a direita, sem zeros a esquerda (satura em 9999). */
static void display_number(int n) {
    char s[5];
    if (n < 0) n = 0;
    if (n > 9999) n = 9999;
    snprintf(s, sizeof s, "%4d", n);
    display_text(s);
}

static void initDisplay(void) {
    pinMode(DISP_DATA, OUTPUT); pinMode(DISP_LATCH, OUTPUT); pinMode(DISP_CLOCK, OUTPUT);
    pthread_t t; pthread_create(&t, NULL, disp_loop, NULL); pthread_detach(t);
}
