/*
 * Demo de E/S para o fliperama RFID no kit Freenove FNK0054.
 *
 * Combina o botão do capítulo 3 (GPIO BCM 26) com o buzzer do
 * capítulo 6 (GPIO BCM 12): pressionar liga o buzzer e soltar desliga.
 * Baseado nos exemplos ButtonLED.c e Doorbell.c da Freenove.
 */

#include <signal.h>
#include <stdio.h>
#include <wiringPi.h>

#define BUTTON_PIN 26
#define BUZZER_PIN 12

static volatile sig_atomic_t running = 1;

static void stop_demo(int signal_number) {
    (void)signal_number;
    running = 0;
}

int main(void) {
    int previous_pressed = 0;
    unsigned int press_count = 0;

    if (wiringPiSetupGpio() == -1) {
        fprintf(stderr, "Não foi possível inicializar wiringPi.\n");
        return 1;
    }

    pinMode(BUTTON_PIN, INPUT);
    pullUpDnControl(BUTTON_PIN, PUD_UP);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    signal(SIGINT, stop_demo);
    signal(SIGTERM, stop_demo);

    printf("Demo iniciada. Pressione o botão para tocar o buzzer; Ctrl+C encerra.\n");

    while (running) {
        const int pressed = digitalRead(BUTTON_PIN) == LOW;

        if (pressed != previous_pressed) {
            if (pressed) {
                press_count++;
                digitalWrite(BUZZER_PIN, HIGH);
                printf("Botão pressionado (%u): buzzer ligado.\n", press_count);
            } else {
                digitalWrite(BUZZER_PIN, LOW);
                printf("Botão liberado: buzzer desligado.\n");
            }
            previous_pressed = pressed;
        }
        delay(20); /* polling de 50 Hz; reduz efeito de bounce */
    }

    digitalWrite(BUZZER_PIN, LOW);
    printf("GPIOs retornaram ao estado seguro. Demo encerrada.\n");
    return 0;
}
