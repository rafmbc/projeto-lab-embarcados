/* Arcade RFID para Raspberry Pi + Projects Board Freenove.
 * Hardware: direcional BCM 26/20/16/21 e buzzer BCM 4.
 * Este arquivo: sistema (GPIO, ADC, buzzers, RFID, cartoes.csv, menus) e a troca entre jogos.
 * Cada jogo vive no seu arquivo: snake.c, asteroids.c, rhythm.c, defuse.c. */
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wiringPi.h>
#include "display.c"
#include "mfrc522.h"
#include "raylib.h"


#define W 800
#define H 480
#define CX (W/2)
#define CSV_FILE "cartoes.csv"
#define CSV_HEADER "CardID,Credito,Codigo,Pont_Cobrinha,Pont_Asteroides,Pont_Ritmo,Pont_Defuse\n"
#define CSV_LINE_MAX 64
#define PIN_UP 20
#define PIN_LEFT 26
#define PIN_RIGHT 16
#define PIN_DOWN 21
#define PIN_BUZZER 4
#define PIN_ALARM 12 /* buzzer ATIVO: HIGH liga (chave S3 "Active Buzzer" em ON, "Relay" em OFF) */
#define PIN_JOYSTICK_Z 7
#define ADC_I2C_ADDRESS 0x48
#define JOYSTICK_X_CHANNEL 5
#define JOYSTICK_Y_CHANNEL 6
#define JOYSTICK_LOW 80
#define JOYSTICK_HIGH 175
#define COST 1

#define GAME_COUNT 4

enum { UP = 1, LEFT = 2, RIGHT = 4, DOWN = 8 };
typedef enum { WAIT_CARD, CARD_CODE, MENU, OPTIONS, RECHARGE, SCORES, SNAKE, ASTEROIDS, RHYTHM, DEFUSE, MESSAGE } State;
typedef struct { int x, y; } Cell;
typedef struct { char card[9]; char game[16]; int score; } ScoreEntry;
typedef struct {
    pthread_mutex_t lock;
    State state;
    char card[9];
    char code[4];
    int credits;
    char message[96];
    State message_back;
} App;

static App app;
static unsigned held, pressed, button_pressed, joystick_held;
static int adc_fd = -1;

static int buzz_edges;
static double buzz_at, message_at;
static int selected, option_selected, recharge_selected;
static const char *GAME_NAMES[GAME_COUNT] = {"COBRINHA", "ASTEROIDES", "RITMO", "DEFUSE"};
static char active_card[9];
static char code_letters[4] = "AAA";
static int code_index;
static char code_error[64];

static void card_string(const uint8_t *id, char *out) {
    snprintf(out, 9, "%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
}

/* ponytail: uma linha por cartao. Teto: 128 cartoes. Upgrade path: SQLite. */
typedef struct { char card[9]; char code[4]; int credits; int snake; int asteroid; int rhythm; int defuse; } Row;

static int csv_load(Row *rows, int limit) {
    memset(rows, 0, limit * sizeof *rows); /* linhas novas (rows[n]) saem zeradas e com '\0' */
    FILE *f = fopen(CSV_FILE, "r"); if (!f) return 0;
    char line[CSV_LINE_MAX]; int n = 0;
    fgets(line, sizeof line, f);
    while (n < limit && fgets(line, sizeof line, f)) {
        Row r = {0};
        sscanf(line, "%8[^,],%d,%3[^,],%d,%d,%d,%d", r.card, &r.credits, r.code, &r.snake, &r.asteroid, &r.rhythm, &r.defuse);
        if (r.card[0]) rows[n++] = r;
    }
    fclose(f); return n;
}

static void csv_save(Row *rows, int n) {
    FILE *f = fopen(CSV_FILE, "w"); if (!f) return;
    fputs(CSV_HEADER, f);
    for (int i = 0; i < n; i++)
        fprintf(f, "%s,%d,%s,%d,%d,%d,%d\n", rows[i].card, rows[i].credits, rows[i].code, rows[i].snake, rows[i].asteroid, rows[i].rhythm, rows[i].defuse);
    fclose(f);
}

static void csv_init(void) {
    FILE *f = fopen(CSV_FILE, "r");
    if (f) { fclose(f); return; }
    f = fopen(CSV_FILE, "w");
    if (f) { fputs(CSV_HEADER, f); fclose(f); }
}

static Row *csv_find(Row *rows, int n, const char *card) {
    for (int i = 0; i < n; i++) if (!strcmp(rows[i].card, card)) return &rows[i];
    return NULL;
}

static int csv_read(const char *card) {
    Row rows[128]; int n = csv_load(rows, 128);
    Row *r = csv_find(rows, n, card); return r ? r->credits : 0;
}

static int csv_write(const char *card, int credits) {
    Row rows[128]; int n = csv_load(rows, 128);
    Row *r = csv_find(rows, n, card);
    if (r) { r->credits = credits; }
    else { if (n >= 128) return -1; strncpy(rows[n].card, card, 8); rows[n].credits = credits; n++; }
    csv_save(rows, n); return 0;
}

static int card_code_read(const char *card, char *code) {
    Row rows[128]; int n = csv_load(rows, 128);
    Row *r = csv_find(rows, n, card);
    if (r && r->code[0]) { strcpy(code, r->code); return 1; } return 0;
}

static int card_code_write(const char *card, const char *code) {
    Row rows[128]; int n = csv_load(rows, 128);
    for (int i = 0; i < n; i++)
        if (!strcmp(rows[i].code, code) && strcmp(rows[i].card, card)) return -2;
    Row *r = csv_find(rows, n, card);
    if (r) { strncpy(r->code, code, 3); }
    else { if (n >= 128) return -1; strncpy(rows[n].card, card, 8); strncpy(rows[n].code, code, 3); n++; }
    csv_save(rows, n); return 0;
}

static void card_activate(const char *card) {
    char code[4] = {0}; int registered;
    csv_init(); registered = card_code_read(card, code);
    pthread_mutex_lock(&app.lock);
    strcpy(app.card, card); app.credits = csv_read(card);
    if (registered) { strcpy(app.code, code); app.state = MENU; }
    else { app.code[0] = 0; app.state = CARD_CODE; code_letters[0] = code_letters[1] = code_letters[2] = 'A'; code_index = 0; code_error[0] = 0; }
    pthread_mutex_unlock(&app.lock);
}

/* Registra score; mantem apenas o melhor por jogo. */
static int score_record(const char *card, const char *game, int score) {
    Row rows[128]; int n = csv_load(rows, 128); int record = 0;
    Row *r = csv_find(rows, n, card);
    if (!r) { if (n >= 128) return 0; strncpy(rows[n].card, card, 8); rows[n].credits = csv_read(card); r = &rows[n++]; }
    if (!strcmp(game, "Cobrinha")  && score > r->snake)    { r->snake    = score; record = 1; }
    if (!strcmp(game, "Asteroides") && score > r->asteroid) { r->asteroid = score; record = 1; }
    if (!strcmp(game, "Ritmo")      && score > r->rhythm)   { r->rhythm   = score; record = 1; }
    if (!strcmp(game, "Defuse")     && score > r->defuse)   { r->defuse   = score; record = 1; }
    csv_save(rows, n); return record;
}

static int score_load(ScoreEntry *entries, int limit, const char *game) {
    Row rows[128]; int n = csv_load(rows, 128), count = 0;
    memset(entries, 0, limit * sizeof *entries); /* strncpy de 8 chars nao poe '\0' no card */
    for (int i = 0; i < n && count < limit; i++) {
        int s = !strcmp(game, "Cobrinha") ? rows[i].snake : !strcmp(game, "Asteroides") ? rows[i].asteroid : !strcmp(game, "Ritmo") ? rows[i].rhythm : rows[i].defuse;
        if (!s) continue;
        strncpy(entries[count].card, rows[i].card, 8);
        strncpy(entries[count].game, game, 15);
        entries[count].score = s; count++;
    }
    for (int i = 0; i < count; i++) for (int j = i+1; j < count; j++)
        if (entries[j].score > entries[i].score) { ScoreEntry t = entries[i]; entries[i] = entries[j]; entries[j] = t; }
    return count;
}

/* Melodias 8-bit: {frequencia Hz, duracao ms}, terminadas em {0,0}. Exige buzzer PASSIVO.
 * VOLUME: duty da onda quadrada; 50% = volume maximo, 15% = 30% disso. */
#define BUZZ_DUTY 0.02
#define NOTE_GAP_MS 90
typedef struct { int f, ms; } Note;
static const Note NEW_RECORD[]     = {{523,100},{659,100},{784,100},{1047,100},{784,100},{1047,100},{1319,300},{0,0}};
static volatile int melody_on;
static const Note *melody_queue[2];

static void play_notes(const Note *notes) {
    if (!notes) return;
    for (const Note *n = notes; n->ms; n++) {
        int period = 1000000/n->f, high = (int)(period*BUZZ_DUTY), cycles = n->ms*1000/period;
        for (int c = 0; c < cycles; c++) {
            digitalWrite(PIN_BUZZER, HIGH); delayMicroseconds(high);
            digitalWrite(PIN_BUZZER, LOW);  delayMicroseconds(period-high);
        }
        usleep(NOTE_GAP_MS*1000);
    }
}

static void *melody_thread(void *arg) {
    (void)arg;
    play_notes(melody_queue[0]);
    if (melody_queue[1]) { usleep(600000); play_notes(melody_queue[1]); }
    melody_on = 0; return NULL;
}

static void melody_play2(const Note *n1, const Note *n2) {
    if (melody_on) return;
    buzz_edges = 0; /* melodia tem prioridade sobre o clique de tecla */
    melody_on = 1; melody_queue[0] = n1; melody_queue[1] = n2;
    pthread_t t; pthread_create(&t, NULL, melody_thread, NULL); pthread_detach(t);
}

static void melody_play(const Note *notes) { melody_play2(notes, NULL); }

static void buzzer_play(int pulses) {
    if (melody_on) return;
    buzz_edges = pulses * 2;
    buzz_at = GetTime();
}

/* Buzzer ativo: som fixo. VOLUME: liga/desliga rapido (PWM por software);
 * ALARM_DUTY menor = mais baixo. Calibrar na placa: se sumir o som, suba o duty. */
#define ALARM_DUTY 0.25
#define ALARM_CHOP_US 1000
static volatile int alarm_on;

static void *alarm_thread(void *arg) {
    int ms = (int)(intptr_t)arg, high = (int)(ALARM_CHOP_US*ALARM_DUTY);
    for (int c = 0; c < ms*1000/ALARM_CHOP_US; c++) {
        digitalWrite(PIN_ALARM, HIGH); delayMicroseconds(high);
        digitalWrite(PIN_ALARM, LOW);  delayMicroseconds(ALARM_CHOP_US-high);
    }
    alarm_on = 0; return NULL;
}

static void alarm_beep(int ms) {
    if (alarm_on) return;
    alarm_on = 1;
    pthread_t t; pthread_create(&t, NULL, alarm_thread, (void *)(intptr_t)ms); pthread_detach(t);
}

static void controls_init(void) {
    if (wiringPiSetupGpio() == -1) { fputs("Erro ao iniciar GPIO.\n", stderr); return; }
    const int pins[] = { PIN_UP, PIN_LEFT, PIN_RIGHT, PIN_DOWN, PIN_JOYSTICK_Z };
    for (int i = 0; i < 5; i++) { pinMode(pins[i], INPUT); pullUpDnControl(pins[i], PUD_UP); }
    pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
    pinMode(PIN_ALARM, OUTPUT); digitalWrite(PIN_ALARM, LOW);
    delayMicroseconds(5);
    adc_fd = open("/dev/i2c-1", O_RDWR);
    if (adc_fd >= 0 && ioctl(adc_fd, I2C_SLAVE, ADC_I2C_ADDRESS) < 0) { close(adc_fd); adc_fd = -1; }
    if (adc_fd < 0) fputs("Joystick analogico indisponivel: habilite I2C e verifique ADS7830 (0x48).\n", stderr);
}

static int ads7830_read(int channel) {
    unsigned char command = (unsigned char)(0x84 | ((((channel << 2) | (channel >> 1)) & 0x07) << 4));
    unsigned char value;
    if (adc_fd < 0 || write(adc_fd, &command, 1) != 1 || read(adc_fd, &value, 1) != 1) return -1;
    return value;
}

#define DELAY 0.035
static void controls_poll(void) {
    unsigned old = held;
    /* Estabiliza o nivel por 35 ms. Evita que o bounce do S4 azul
       desapareca antes de virar um evento de navegacao. */
    static unsigned sampled;
    static double sampled_at;
    unsigned buttons = (digitalRead(PIN_UP) == LOW ? UP : 0) |
                       (digitalRead(PIN_LEFT) == LOW ? LEFT : 0) |
                       (digitalRead(PIN_RIGHT) == LOW ? RIGHT : 0) |
                       (digitalRead(PIN_DOWN) == LOW ? DOWN : 0) |
                       (digitalRead(PIN_JOYSTICK_Z) == LOW ? RIGHT : 0);
    static unsigned button_sampled, buttons_held;
    static double button_sampled_at;
    unsigned old_buttons = buttons_held;
    unsigned raw = buttons;

    /* Nesta Projects Board os eixos analogicos estao invertidos.
       Botao fisico sempre vence uma leitura analogica concorrente. */
    joystick_held = 0;
    if (buttons == 0) {
        int joy_x = ads7830_read(JOYSTICK_X_CHANNEL);
        int joy_y = ads7830_read(JOYSTICK_Y_CHANNEL);
        if (joy_x >= 0 && joy_y >= 0) {
            if (joy_x <= JOYSTICK_LOW) joystick_held = RIGHT;
            else if (joy_x >= JOYSTICK_HIGH) joystick_held = LEFT;
            else if (joy_y <= JOYSTICK_LOW) joystick_held = DOWN;
            else if (joy_y >= JOYSTICK_HIGH) joystick_held = UP;
            raw = joystick_held;
        }
    }
    if (buttons != button_sampled) { button_sampled = buttons; button_sampled_at = GetTime(); }
    if (button_sampled != buttons_held && GetTime() - button_sampled_at >= DELAY) buttons_held = button_sampled;
    if (raw != sampled) { sampled = raw; sampled_at = GetTime(); }
    if (sampled != held && GetTime() - sampled_at >= DELAY) held = sampled;
    button_pressed = buttons_held & ~old_buttons;
    pressed = held & ~old;
    if (pressed && !buttons_held) buzzer_play(1); /* clique so no joystick analogico; botoes ficam mudos */
    if (buzz_edges > 0 && GetTime() >= buzz_at) {
        digitalWrite(PIN_BUZZER, (buzz_edges & 1) == 0 ? HIGH : LOW);
        buzz_edges--; buzz_at = GetTime() + 0.07;
    } else if (!buzz_edges && !melody_on) digitalWrite(PIN_BUZZER, LOW);
}

static int was_pressed(unsigned key) { return (pressed & key) != 0; }
static int button_was_pressed(unsigned key) { return (button_pressed & key) != 0; }
static int is_held(unsigned key) { return (held & key) != 0; }
static int joystick_is_held(unsigned key) { return (joystick_held & key) != 0; }

static double message_len = 2.2;
static int message_record;

static void set_state(State s) {
    pthread_mutex_lock(&app.lock); app.state = s; pthread_mutex_unlock(&app.lock);
}

static void show_message(const char *text, State back) {
    pthread_mutex_lock(&app.lock);
    snprintf(app.message, sizeof app.message, "%s", text);
    app.message_back = back;
    app.state = MESSAGE;
    pthread_mutex_unlock(&app.lock);
    message_at = GetTime(); message_len = 2.2; message_record = 0;
}

static void *rfid_loop(void *unused) {
    uint8_t serial[5] = {0}, tag[16] = {0};
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&app.lock); State state = app.state; pthread_mutex_unlock(&app.lock);
        if (state != WAIT_CARD) { usleep(80000); continue; }
        if (MFRC522_Request(PICC_REQIDL, tag) != MI_OK || MFRC522_Anticoll(serial) != MI_OK || MFRC522_SelectTag(serial) == 0) {
            MFRC522_Halt(); usleep(50000); continue;
        }
        char card[9]; card_string(serial, card);
        card_activate(card);
        buzzer_play(2); MFRC522_Halt();
    }
    return NULL;
}

static void game_over(const char *game, int score, const Note *over) {
    int record = score_record(active_card, game, score);
    show_message(TextFormat("%s: %d pontos", game, score), MENU);
    if (record) { message_record = 1; message_len = 5.0; }
    melody_play2(over, record ? NEW_RECORD : NULL);
}

static void text_center(const char *t, int y, int size, Color c) { DrawText(t, CX - MeasureText(t, size)/2, y, size, c); }

static void draw_header(const char *title, int score) {
    DrawText(title, 24, 18, 28, RAYWHITE);
    DrawText(TextFormat("Pontos: %d", score), W-170, 24, 20, YELLOW);
    DrawLine(0, 55, W, 55, (Color){90,90,155,255});
}

/* Jogos: um arquivo cada, compilados junto com este (mesmo esquema do display.c). */
#include "snake.c"
#include "asteroids.c"
#include "rhythm.c"
#include "defuse.c"

static void start_game(State game, const char *card, int credits) {
    if (credits < COST) { show_message("Creditos insuficientes", MENU); buzzer_play(3); return; }
    if (csv_write(card, credits-COST) < 0) { show_message("Erro ao salvar cartao", MENU); buzzer_play(3); return; }
    pthread_mutex_lock(&app.lock); app.credits = credits-COST; app.state = game; pthread_mutex_unlock(&app.lock);
    strcpy(active_card, card);
    if (game == SNAKE) start_snake(); else if (game == ASTEROIDS) start_asteroids(); else if (game == RHYTHM) start_rhythm(); else start_defuse();
    melody_play(game == SNAKE ? SNAKE_START : game == ASTEROIDS ? ASTEROID_START : game == RHYTHM ? RHYTHM_START : DEFUSE_START);
}

static void recharge(const char *card, int credits, int value) {
    if (csv_write(card, credits + value) < 0) { show_message("Erro ao salvar recarga", OPTIONS); buzzer_play(3); return; }
    pthread_mutex_lock(&app.lock); app.credits = credits + value; pthread_mutex_unlock(&app.lock);
    show_message(TextFormat("Recarga de +%d credito(s)", value), OPTIONS); buzzer_play(2);
}

static void draw_wallet(const char *code, int credits, int y) {
    text_center(TextFormat("Codigo %s  |  Creditos: %d  |  1 credito por partida", code, credits), y, 18, GOLD);
}

static void draw_button(const char *label, int y, int on) {
    int w = MeasureText(label, 20) + 44;
    DrawRectangleRounded((Rectangle){CX - w/2, y, w, 42}, .3f, 8, on ? (Color){70,110,220,255} : (Color){35,45,85,255});
    text_center(label, y + 11, 20, WHITE);
}

static void draw_arrow(int cx, int cy, int dir) {
    DrawRing((Vector2){cx, cy}, 23, 28, 0, 360, 48, RAYWHITE);
    Vector2 tip = {cx + 11*dir, cy};
    DrawLineEx((Vector2){cx - 11*dir, cy}, tip, 4, RAYWHITE);
    DrawLineEx(tip, (Vector2){cx + 2*dir, cy - 9}, 4, RAYWHITE);
    DrawLineEx(tip, (Vector2){cx + 2*dir, cy + 9}, 4, RAYWHITE);
}

/* Carrossel do menu: moldura + "foto" de cada jogo. */
static void draw_preview(int game, Rectangle r) {
    DrawRectangle(r.x-3, r.y-3, r.width+6, r.height+6, (Color){65,80,125,255});
    if (game == 0) preview_snake(r); else if (game == 1) preview_asteroids(r); else if (game == 2) preview_rhythm(r); else preview_defuse(r);
}

int main(void) {
    if (MFRC522_Init('B') != 0) fputs("Erro ao iniciar RFID MFRC522.\n", stderr);
    pthread_mutex_init(&app.lock, NULL); app.state = WAIT_CARD;
    pthread_t thread; pthread_create(&thread, NULL, rfid_loop, NULL); pthread_detach(thread);
    InitWindow(W, H, "Arcade RFID"); SetTargetFPS(30); controls_init();
    initDisplay();
    while (!WindowShouldClose()) {
        controls_poll();
        pthread_mutex_lock(&app.lock); State state=app.state; char card[9]; strcpy(card,app.card); char public_code[4]; strcpy(public_code,app.code); int credits=app.credits; char note[96]; strcpy(note,app.message); State back=app.message_back; pthread_mutex_unlock(&app.lock);
        switch(state){
            case WAIT_CARD:
                break;
            case MESSAGE:
                if (GetTime()-message_at>message_len) { set_state(back); } break;
            case CARD_CODE:
                if (button_was_pressed(UP))
                    code_letters[code_index] = code_letters[code_index] == 70 ? 65 : code_letters[code_index] + 1;
                if (button_was_pressed(DOWN))
                    code_letters[code_index] = code_letters[code_index] == 65 ? 70 : code_letters[code_index] - 1;
                if (button_was_pressed(RIGHT)) {
                    code_index++;
                    if (code_index > 2) {
                        int result = card_code_write(card, code_letters);
                        if (result == 0) {
                            pthread_mutex_lock(&app.lock);
                            strcpy(app.code, code_letters); app.state = MENU;
                            pthread_mutex_unlock(&app.lock);
                            selected = 0; buzzer_play(2);
                        } else {
                            snprintf(code_error, sizeof code_error, result == -2 ? "Esse codigo ja esta em uso." : "Erro ao salvar codigo.");
                            code_letters[0] = code_letters[1] = code_letters[2] = 65;
                            code_index = 0; buzzer_play(3);
                        }
                    }
                }
                break;
            case MENU: /* carrossel: esq/dir troca jogo, cima joga, baixo abre opcoes */
                if (was_pressed(LEFT)) selected = (selected + GAME_COUNT - 1) % GAME_COUNT;
                if (was_pressed(RIGHT)) selected = (selected + 1) % GAME_COUNT;
                if (was_pressed(UP)) { const State games[GAME_COUNT] = {SNAKE, ASTEROIDS, RHYTHM, DEFUSE}; start_game(games[selected], card, credits); }
                else if (was_pressed(DOWN)) { option_selected = 0; set_state(OPTIONS); }
                break;
            case OPTIONS: /* cima/baixo escolhe, direita confirma, esquerda volta aos jogos */
                if (was_pressed(UP) && option_selected > 0) option_selected--;
                if (was_pressed(DOWN) && option_selected < 3) option_selected++;
                if (was_pressed(LEFT)) set_state(MENU);
                if (was_pressed(RIGHT)) {
                    switch (option_selected) {
                        case 0: recharge_selected = 0; set_state(RECHARGE); break;
                        case 1: show_message(TextFormat("Saldo: %d credito(s)", credits), OPTIONS); break;
                        case 2: set_state(SCORES); break;
                        default: set_state(WAIT_CARD); break;
                    }
                }
                break;
            case SCORES:
                if (was_pressed(LEFT) || was_pressed(RIGHT)) set_state(OPTIONS);
                break;
            case RECHARGE:
                if (was_pressed(UP) && recharge_selected > 0) recharge_selected--;
                if (was_pressed(DOWN) && recharge_selected < 3) recharge_selected++;
                if (was_pressed(LEFT)) set_state(OPTIONS);
                if (was_pressed(RIGHT)) {
                    const int packs[] = {1, 5, 10};
                    if (recharge_selected < 3) recharge(card,credits,packs[recharge_selected]);
                    else set_state(OPTIONS);
                }
                break;
            default: break;
        }
        if (state==SNAKE) display_number(snake_score);
        else if (state==ASTEROIDS) display_number(asteroid_score);
        else if (state==RHYTHM) display_number(rhythm_score);
        else if (state==DEFUSE) display_number(defuse_score);
        else if (state==CARD_CODE) { char t[4]; for (int i=0;i<3;i++) t[i]=i<=code_index?code_letters[i]:'_'; t[3]=0; display_text(t); }
        else if (state==WAIT_CARD) display_text("");
        else display_text(public_code);
        BeginDrawing(); ClearBackground((Color){12,14,29,255});
        if (state==WAIT_CARD) {
            DrawText("ARCADE RFID", CX-105,75,32,RAYWHITE); DrawCircleLines(CX,205,78,(Color){100,120,255,220}); DrawCircle(CX,205,44,(Color){55,70,190,255});
            DrawText("RFID",CX-28,196,21,WHITE);
            DrawText("Aproxime o cartao", CX - MeasureText("Aproxime o cartao", 22)/2, 315, 22, RAYWHITE);
        } else if (state==CARD_CODE) {
            DrawText("NOVO CARTAO", CX-108, 65, 31, GOLD);
            DrawText("CRIE SEU CODIGO", CX-125, 112, 25, RAYWHITE);
            DrawText("Escolha tres letras de A a F", CX-154, 145, 18, LIGHTGRAY);
            for (int i = 0; i < 3; i++) {
                int x = CX - 70 + i*70;
                char shown[2] = { i <= code_index ? code_letters[i] : 95, 0 };
                Color color = i == code_index ? YELLOW : RAYWHITE;
                DrawText(shown, x, 195, 42, color);
                DrawRectangle(x - 2, 245, 35, i == code_index ? 4 : 2, color);
            }
            DrawText("Azul sobe  |  Vermelho desce  |  Verde confirma", CX-250, 285, 18, LIGHTGRAY);
            if (code_error[0]) DrawText(code_error, CX-MeasureText(code_error,18)/2, 330, 18, RED);
        } else if (state==MENU) {
            text_center(GAME_NAMES[selected], 16, 44, RAYWHITE);
            draw_wallet(public_code, credits, 402);
            draw_preview(selected, (Rectangle){172, 78, 456, 256});
            draw_arrow(92, 206, -1); draw_arrow(708, 206, 1);
            draw_button("OPCOES", 350, 0);
            text_center("Esquerda/direita: trocar jogo | Cima: jogar | Baixo: opcoes", 440, 16, LIGHTGRAY);
        } else if (state==OPTIONS) {
            const char *items[] = {"RECARREGAR SALDO", "CONSULTAR SALDO", "PLACAR", "ENCERRAR CARTAO"};
            text_center("OPCOES", 16, 44, RAYWHITE);
            draw_wallet(public_code, credits, 370);
            for (int i = 0; i < 4; i++) draw_button(items[i], 100 + i*62, i == option_selected);
            text_center("Cima/baixo: escolher | Direita: confirmar | Esquerda: voltar aos jogos", 440, 16, LIGHTGRAY);
        } else if (state==RECHARGE) {
            const char *packs[] = {"+1 CREDITO","+5 CREDITOS","+10 CREDITOS","VOLTAR"};
            DrawText("RECARGA LIVRE", CX-120,75,30,GOLD);
            DrawText(TextFormat("Codigo %s   |   Saldo: %d",public_code,credits),CX-145,112,20,RAYWHITE);
            for (int i=0;i<4;i++) { Color c=i==recharge_selected?(Color){50,150,90,255}:(Color){35,65,55,255}; DrawRectangleRounded((Rectangle){205,155+i*55,390,42},.2f,8,c); DrawText(packs[i],270,165+i*55,19,WHITE); if(i==recharge_selected)DrawText(">",220,165+i*55,22,YELLOW); }
            DrawText("Selecione o pacote desejado",CX-120,395,17,LIGHTGRAY);
            DrawText("Cima/baixo seleciona | direita confirma | esquerda volta",140,425,16,LIGHTGRAY);
        } else if (state==SCORES) {
            static const char *score_games[GAME_COUNT] = {"Cobrinha", "Asteroides", "Ritmo", "Defuse"};
            const Color score_colors[GAME_COUNT] = {LIME, SKYBLUE, VIOLET, ORANGE};
            DrawText("PLACAR", CX-58, 24, 32, GOLD);
            for (int g = 0; g < GAME_COUNT; g++) {
                ScoreEntry e[10]; int n = score_load(e, 10, score_games[g]); int x = 18 + g*197;
                DrawText(GAME_NAMES[g], x + 10, 75, 20, score_colors[g]);
                if (g) DrawLine(x - 10, 68, x - 10, 390, (Color){80,90,145,255});
                if (n == 0) DrawText("Sem scores", x + 10, 115, 18, LIGHTGRAY);
                for (int i = 0; i < n; i++) {
                    char display_code[4] = "---"; card_code_read(e[i].card, display_code);
                    DrawText(TextFormat("%d.", i+1), x, 112+i*27, 18, YELLOW);
                    DrawText(display_code, x + 32, 112+i*27, 18, RAYWHITE);
                    DrawText(TextFormat("%d", e[i].score), x + 100, 112+i*27, 18, GREEN);
                }
            }
            DrawText("Esquerda ou direita para voltar", CX-135, 430, 17, LIGHTGRAY);
        } else {
            if (state==SNAKE) draw_snake();
            else if (state==ASTEROIDS) draw_asteroids();
            else if (state==RHYTHM) draw_rhythm();
            else if (state==DEFUSE) draw_defuse();
            else if (message_record) {
                DrawRectangleRounded((Rectangle){110,140,580,200},.15f,8,(Color){60,45,10,255}); DrawRectangleRoundedLines((Rectangle){110,140,580,200},.15f,8,GOLD);
                DrawText("NOVO RECORDE!",CX-MeasureText("NOVO RECORDE!",42)/2,170,42,GOLD);
                DrawText(note,CX-MeasureText(note,27)/2,250,27,WHITE);
            }
            else { DrawRectangleRounded((Rectangle){110,160,580,150},.15f,8,(Color){35,40,82,255}); DrawText(note,CX-MeasureText(note,27)/2,215,27,WHITE); }
        }
        EndDrawing();
    }
    digitalWrite(PIN_BUZZER, LOW); digitalWrite(PIN_ALARM, LOW);
    CloseWindow(); pthread_mutex_destroy(&app.lock); return 0;
}
