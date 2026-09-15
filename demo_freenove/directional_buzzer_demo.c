/*
 * Diagnostico do direcional da Freenove Projects Board + buzzer.
 *
 * Mapeamento BCM confirmado na placa:
 *   S4 (azul)     = UP    = GPIO 26
 *   S5 (amarelo)  = LEFT  = GPIO 20
 *   S7 (verde)    = RIGHT = GPIO 16
 *   S6 (vermelho) = DOWN  = GPIO 21
 *   Buzzer ativo            GPIO 12
 *
 * Cada pressionamento gera uma mensagem, incrementa seu contador e toca
 * 1 a 4 bipes, conforme a direcao. Entradas usam pull-up e debounce.
 */

#include <signal.h>
#include <stdio.h>
#include <wiringPi.h>

#define BUZZER_PIN 12
#define DEBOUNCE_MS 50

typedef struct {
    const char *name;
    const char *switch_name;
    int pin;
    int beeps;
    int previous_level;
    unsigned int last_event_ms;
    unsigned int count;
} Direction;

static volatile sig_atomic_t running = 1;

static Direction directions[] = {
    { "CIMA",     "S4 azul",      26, 1, HIGH, 0, 0 },
    { "ESQUERDA", "S5 amarelo",   20, 2, HIGH, 0, 0 },
    { "DIREITA",  "S7 verde",     16, 3, HIGH, 0, 0 },
    { "BAIXO",    "S6 vermelho", 21, 4, HIGH, 0, 0 },
};

static const int direction_count = sizeof(directions) / sizeof(directions[0]);

static void stop_demo(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void beep(int times)
{
    for (int i = 0; i < times; i++) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(70);
        digitalWrite(BUZZER_PIN, LOW);
        if (i + 1 < times)
            delay(70);
    }
}

int main(void)
{
    if (wiringPiSetupGpio() == -1) {
        fprintf(stderr, "Erro: nao foi possivel inicializar wiringPi.\n");
        return 1;
    }

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    for (int i = 0; i < direction_count; i++) {
        pinMode(directions[i].pin, INPUT);
        pullUpDnControl(directions[i].pin, PUD_UP);
        directions[i].previous_level = digitalRead(directions[i].pin);
    }

    signal(SIGINT, stop_demo);
    signal(SIGTERM, stop_demo);

    puts("=== Diagnostico do direcional Freenove ===");
    puts("S4 azul=CIMA | S5 amarelo=ESQUERDA | S7 verde=DIREITA | S6 vermelho=BAIXO");
    puts("Cada direcao toca 1, 2, 3 ou 4 bipes. Ctrl+C encerra.\n");

    while (running) {
        const unsigned int now = millis();

        for (int i = 0; i < direction_count; i++) {
            Direction *direction = &directions[i];
            const int level = digitalRead(direction->pin);

            if (level == LOW && direction->previous_level == HIGH &&
                now - direction->last_event_ms >= DEBOUNCE_MS) {
                direction->last_event_ms = now;
                direction->count++;
                printf("%-8s (%s, GPIO %d): evento %u\n",
                       direction->name, direction->switch_name,
                       direction->pin, direction->count);
                fflush(stdout);
                beep(direction->beeps);
            }
            direction->previous_level = level;
        }
        delay(5);
    }

    digitalWrite(BUZZER_PIN, LOW);
    puts("\nResumo:");
    for (int i = 0; i < direction_count; i++)
        printf("  %-8s: %u pressionamento(s)\n",
               directions[i].name, directions[i].count);
    puts("Buzzer desligado. Demo encerrada.");
    return 0;
}
