/* Arcade RFID para Raspberry Pi + Projects Board Freenove.
 * Hardware: direcional BCM 26/20/16/21 e buzzer BCM 4. */
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
#define CSV_HEADER "CardID,Credito,Codigo,Pont_Cobrinha,Pont_Asteroides,Pont_Ritmo\n"
#define CSV_LINE_MAX 64
#define PIN_UP 20
#define PIN_LEFT 26
#define PIN_RIGHT 16
#define PIN_DOWN 21
#define PIN_BUZZER 4
#define PIN_JOYSTICK_Z 7
#define ADC_I2C_ADDRESS 0x48
#define JOYSTICK_X_CHANNEL 5
#define JOYSTICK_Y_CHANNEL 6
#define JOYSTICK_LOW 80
#define JOYSTICK_HIGH 175
#define COST 1

#define GAME_COUNT 3
/* Cores dos botoes da placa, usadas no Ritmo: esquerda amarelo, cima azul, baixo vermelho, direita verde. */
static const Color LANE_COLORS[4] = {{253,249,0,255}, {0,121,241,255}, {230,41,55,255}, {0,228,48,255}};

enum { UP = 1, LEFT = 2, RIGHT = 4, DOWN = 8 };
typedef enum { WAIT_CARD, CARD_CODE, MENU, OPTIONS, RECHARGE, SCORES, SNAKE, ASTEROIDS, RHYTHM, MESSAGE } State;
typedef struct { int x, y; } Cell;
typedef struct { float x, y, speed, radius; } Rock;
typedef struct Shoot {
    Vector2 position;
    Vector2 speed;
    float radius;
    float rotation;
    int lifeSpawn;
    bool active;
    Color color;
} Shoot;
typedef struct Player {
    Vector2 position;
    Vector2 speed;
    float acceleration;
    float rotation;
    Vector3 collider;
    Color color;
} Player;
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
static const char *GAME_NAMES[GAME_COUNT] = {"COBRINHA", "ASTEROIDES", "RITMO"};
static char active_card[9];
static char code_letters[4] = "AAA";
static int code_index;
static char code_error[64];

/* Cobrinha */
#define COLS 18
#define ROWS 11
#define CELL 28
#define BOARD_X ((W - COLS*CELL)/2)
#define BOARD_Y 120
static Cell snake[COLS*ROWS], food;
static int snake_len, dx, dy, next_dx, next_dy, snake_score;
static double snake_at;

/* Asteroides */
#define ROCKS 9
static Player player = { 0 };
static Rock rocks[ROCKS];
static int asteroid_score;
static int destroyedMeteorsCount;
static double asteroid_at;
#define PLAYER_MAX_SHOTS   5
static Shoot shots[PLAYER_MAX_SHOTS];
static const int screenWidth = 800;
static const int screenHeight = 450;
static float shipHeight = 0.0f;
#define PLAYER_SPEED        6.0f

static void card_string(const uint8_t *id, char *out) {
    snprintf(out, 9, "%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
}

/* ponytail: uma linha por cartao. Teto: 128 cartoes. Upgrade path: SQLite. */
typedef struct { char card[9]; char code[4]; int credits; int snake; int asteroid; int rhythm; } Row;

static int csv_load(Row *rows, int limit) {
    memset(rows, 0, limit * sizeof *rows); /* linhas novas (rows[n]) saem zeradas e com '\0' */
    FILE *f = fopen(CSV_FILE, "r"); if (!f) return 0;
    char line[CSV_LINE_MAX]; int n = 0;
    fgets(line, sizeof line, f);
    while (n < limit && fgets(line, sizeof line, f)) {
        Row r = {0};
        sscanf(line, "%8[^,],%d,%3[^,],%d,%d,%d", r.card, &r.credits, r.code, &r.snake, &r.asteroid, &r.rhythm);
        if (r.card[0]) rows[n++] = r;
    }
    fclose(f); return n;
}

static void csv_save(Row *rows, int n) {
    FILE *f = fopen(CSV_FILE, "w"); if (!f) return;
    fputs(CSV_HEADER, f);
    for (int i = 0; i < n; i++)
        fprintf(f, "%s,%d,%s,%d,%d,%d\n", rows[i].card, rows[i].credits, rows[i].code, rows[i].snake, rows[i].asteroid, rows[i].rhythm);
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
    csv_save(rows, n); return record;
}

static int score_load(ScoreEntry *entries, int limit, const char *game) {
    Row rows[128]; int n = csv_load(rows, 128), count = 0;
    memset(entries, 0, limit * sizeof *entries); /* strncpy de 8 chars nao poe '\0' no card */
    for (int i = 0; i < n && count < limit; i++) {
        int s = !strcmp(game, "Cobrinha") ? rows[i].snake : !strcmp(game, "Asteroides") ? rows[i].asteroid : rows[i].rhythm;
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
static const Note SNAKE_START[]    = {{523,90},{659,90},{784,90},{1047,200},{0,0}};
static const Note SNAKE_OVER[]     = {{392,160},{349,160},{330,160},{262,160},{196,200},{0,0}};
static const Note ASTEROID_START[] = {{262,80},{392,80},{523,80},{392,80},{523,80},{784,220},{0,0}};
static const Note ASTEROID_OVER[]  = {{784,120},{659,120},{523,120},{415,120},{330,120},{220,120},{110,200},{0,0}};
static const Note RHYTHM_START[]   = {{659,90},{622,90},{659,90},{622,90},{659,200},{0,0}};
/* Efeitos de um tom so, mesmo volume (BUZZ_DUTY). */
static const Note SNAKE_EAT[]      = {{1047,60},{0,0}};
static const Note SHOT_FIRE[]      = {{1568,25},{0,0}};
static const Note ROCK_BOOM[]      = {{110,90},{0,0}};
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

static void controls_init(void) {
    if (wiringPiSetupGpio() == -1) { fputs("Erro ao iniciar GPIO.\n", stderr); return; }
    const int pins[] = { PIN_UP, PIN_LEFT, PIN_RIGHT, PIN_DOWN, PIN_JOYSTICK_Z };
    for (int i = 0; i < 5; i++) { pinMode(pins[i], INPUT); pullUpDnControl(pins[i], PUD_UP); }
    pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
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

static void snake_food(void) {
    int on_snake;
    do {
        on_snake = 0; food.x = GetRandomValue(0, COLS-1); food.y = GetRandomValue(0, ROWS-1);
        for (int i = 0; i < snake_len; i++) if (snake[i].x == food.x && snake[i].y == food.y) on_snake = 1;
    } while (on_snake);
}

static float snake_speed;
static void start_snake(void) {
    snake_speed=0.3;
    snake_len = 4; for (int i = 0; i < snake_len; i++) snake[i] = (Cell){8-i, 5};
    dx = next_dx = 1; dy = next_dy = 0; snake_score = 0; snake_at = GetTime()+0.18; snake_food();
}


static void init_shot(Shoot *shot) {
    shot->position = (Vector2){0, 0}; shot->speed = (Vector2){0, 0};
    shot->radius = 2; shot->active = false; shot->lifeSpawn = 0; shot->color = WHITE;
}

static void start_asteroids(void) {
    destroyedMeteorsCount=0;
    asteroid_score=0;
    player.position.x = CX; asteroid_score = 0; asteroid_at = GetTime();
    
    player.position = (Vector2){screenWidth/2, screenHeight/2 - shipHeight/2};
    player.speed = (Vector2){0, 0};
    player.acceleration = 0;
    player.rotation = 0;
    player.collider = (Vector3){player.position.x, player.position.y - (shipHeight/2.5f), 12};
    player.color = LIGHTGRAY;
    
    for (int i = 0; i < ROCKS; i++) { rocks[i] = (Rock){GetRandomValue(25,W-25), GetRandomValue(-H,0), GetRandomValue(120,220), GetRandomValue(16,30)}; }
    for (int i = 0; i < PLAYER_MAX_SHOTS; i++) init_shot(&shots[i]);

}

/* Ritmo (estilo Piano Tiles): os blocos descem nas 4 faixas; aperte o botao da cor
 * da faixa quando o bloco cruza a linha. Cada acerto toca a proxima nota no buzzer. */
#define S16 400 /* semicolcheia, em ms de musica (espaco entre notas) */
static const Note FUR_ELISE[] = {
    {659,S16},{622,S16},{659,S16},{622,S16},{659,S16},{494,S16},{587,S16},{523,S16},{440,3*S16},
    {262,S16},{330,S16},{440,S16},{494,3*S16},
    {330,S16},{415,S16},{494,S16},{523,3*S16},
    {330,S16},{659,S16},{622,S16},{659,S16},{622,S16},{659,S16},{494,S16},{587,S16},{523,S16},{440,3*S16},
    {262,S16},{330,S16},{440,S16},{494,3*S16},
    {330,S16},{523,S16},{494,S16},{440,5*S16},
};
#define SONG_N (int)(sizeof FUR_ELISE / sizeof FUR_ELISE[0])
/* Faixas da esquerda p/ direita = botoes da placa: esquerda amarelo, cima azul, baixo vermelho, direita verde. */
static const unsigned LANE_KEYS[4] = {LEFT, UP, DOWN, RIGHT};
#define RH_LANE_W 110
#define RH_X ((W - 4*RH_LANE_W)/2)
#define RH_TOP 95
#define RH_HIT_Y 400
#define RH_TILE_H 56
#define RH_PX 0.25f   /* pixels por ms de musica */
#define RH_WINDOW 300 /* tolerancia do acerto, ms de musica */
#define RH_LIVES 5
static int song_at[SONG_N], song_len, rhythm_next, rhythm_lives, rhythm_score;
static double rhythm_clock;
static Note rhythm_note[2]; /* nota atual + terminador {0,0} */

/* Grave na esquerda, agudo na direita. */
static int rhythm_lane(int f) { return f < 400 ? 0 : f < 500 ? 1 : f < 630 ? 2 : 3; }
/* A musica repete sem fim; a cada volta fica 15% mais rapida. */
static float rhythm_speed(void) { return 1 + 0.15f * (rhythm_next / SONG_N); }
static int rhythm_time(int k) { return k / SONG_N * song_len + song_at[k % SONG_N]; }

static void start_rhythm(void) {
    song_len = 0;
    for (int i = 0; i < SONG_N; i++) { song_at[i] = song_len; song_len += FUR_ELISE[i].ms; }
    rhythm_clock = -2000; rhythm_next = 0; rhythm_lives = RH_LIVES; rhythm_score = 0;
}

static void start_game(State game, const char *card, int credits) {
    if (credits < COST) { show_message("Creditos insuficientes", MENU); buzzer_play(3); return; }
    if (csv_write(card, credits-COST) < 0) { show_message("Erro ao salvar cartao", MENU); buzzer_play(3); return; }
    pthread_mutex_lock(&app.lock); app.credits = credits-COST; app.state = game; pthread_mutex_unlock(&app.lock);
    strcpy(active_card, card);
    if (game == SNAKE) start_snake(); else if (game == ASTEROIDS) start_asteroids(); else start_rhythm();
    melody_play(game == SNAKE ? SNAKE_START : game == ASTEROIDS ? ASTEROID_START : RHYTHM_START);
}

static void recharge(const char *card, int credits, int value) {
    if (csv_write(card, credits + value) < 0) { show_message("Erro ao salvar recarga", OPTIONS); buzzer_play(3); return; }
    pthread_mutex_lock(&app.lock); app.credits = credits + value; pthread_mutex_unlock(&app.lock);
    show_message(TextFormat("Recarga de +%d credito(s)", value), OPTIONS); buzzer_play(2);
}

static void game_over(const char *game, int score, const Note *over) {
    int record = score_record(active_card, game, score);
    show_message(TextFormat("%s: %d pontos", game, score), MENU);
    if (record) { message_record = 1; message_len = 5.0; }
    melody_play2(over, record ? NEW_RECORD : NULL);
}

static void text_center(const char *t, int y, int size, Color c) { DrawText(t, CX - MeasureText(t, size)/2, y, size, c); }

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

/* "Foto" estatica de cada jogo com as cores originais (primitivas, sem arquivos de imagem). */
static void draw_preview(int game, Rectangle r) {
    DrawRectangle(r.x-3, r.y-3, r.width+6, r.height+6, (Color){65,80,125,255});
    if (game == 0) {
        const int c = 18, ox = r.x + 3, oy = r.y + 2;
        static const Cell body[] = {{14,5},{15,5},{15,6},{15,7},{15,8},{15,9},{16,9},{17,9},{18,9},{19,9},{19,8},{19,7}};
        DrawRectangleRec(r, (Color){10,31,24,255});
        for (int i = 0; i < 12; i++) DrawRectangle(ox + body[i].x*c + 2, oy + body[i].y*c + 2, c-4, c-4, i ? (Color){50,180,100,255} : LIME);
        DrawCircle(ox + 12*c + c/2, oy + 4*c + c/2, 6, RED);
    } else if (game == 1) {
        static const int rk[][3] = {{126,98,23},{36,139,11},{205,139,11},{259,161,23},{179,197,11},{36,219,6},{222,207,6},{277,207,6},{345,208,11},{384,219,6}};
        DrawRectangleRec(r, (Color){12,14,29,255});
        for (int y = 10; y < r.height; y += 29) DrawCircle(r.x + (y*17)%(int)r.width, r.y + y, 1.5f, (Color){185,185,255,170});
        for (int i = 0; i < 10; i++) DrawCircle(r.x + rk[i][0], r.y + rk[i][1], rk[i][2], GRAY);
        DrawText("Pontos: 600", r.x + 10, r.y + 10, 14, YELLOW);
        int sx = r.x + 256;
        DrawTriangle((Vector2){sx, r.y + 232}, (Vector2){sx - 9, r.y + 254}, (Vector2){sx + 9, r.y + 254}, SKYBLUE);
    } else {
        static const int tl[][2] = {{3,10},{2,60},{3,110},{1,150},{0,190}}; /* faixa, y */
        int lw = r.width/4;
        for (int l = 0; l < 4; l++) DrawRectangle(r.x + l*lw, r.y, lw, r.height, l % 2 ? (Color){20,24,48,255} : (Color){16,19,40,255});
        for (int i = 0; i < 5; i++) DrawRectangleRounded((Rectangle){r.x + tl[i][0]*lw + 6, r.y + tl[i][1], lw - 12, 34}, .25f, 8, LANE_COLORS[tl[i][0]]);
        DrawLine(r.x, r.y + 214, r.x + r.width, r.y + 214, RAYWHITE);
        for (int l = 0; l < 4; l++) DrawRectangleRounded((Rectangle){r.x + l*lw + 10, r.y + 222, lw - 20, 26}, .4f, 8, LANE_COLORS[l]);
    }
}

static void draw_header(const char *title, int score) {
    DrawText(title, 24, 18, 28, RAYWHITE);
    DrawText(TextFormat("Pontos: %d", score), W-170, 24, 20, YELLOW);
    DrawLine(0, 55, W, 55, (Color){90,90,155,255});
}

static void draw_snake(void) {
    if ((button_was_pressed(UP) || joystick_is_held(UP)) && dy != 1) { next_dx=0; next_dy=-1; }
    if ((button_was_pressed(DOWN) || joystick_is_held(DOWN)) && dy != -1) { next_dx=0; next_dy=1; }
    if ((button_was_pressed(LEFT) || joystick_is_held(LEFT)) && dx != 1) { next_dx=-1; next_dy=0; }
    if ((button_was_pressed(RIGHT) || joystick_is_held(RIGHT)) && dx != -1) { next_dx=1; next_dy=0; }
    if (GetTime() >= snake_at) {
        Cell next = {snake[0].x+next_dx, snake[0].y+next_dy};
        int hit = next.x<0 || next.x>=COLS || next.y<0 || next.y>=ROWS;
        for (int i=0;i<snake_len;i++) if (snake[i].x==next.x && snake[i].y==next.y) hit=1;
        if (hit) { game_over("Cobrinha", snake_score, SNAKE_OVER); return; }
        int ate = next.x==food.x && next.y==food.y; if (ate) snake_len++;
        for (int i=snake_len-1; i>0; i--) snake[i]=snake[i-1];
        snake[0]=next; dx=next_dx; dy=next_dy;
        if (ate) { snake_score += 10; snake_speed*=0.93; snake_food(); melody_play(SNAKE_EAT); } snake_at=GetTime()+snake_speed;
    }
    draw_header("COBRINHA", snake_score);
    DrawText("Use os botoes direcionais", CX-130, 72, 18, LIGHTGRAY);
    DrawRectangle(BOARD_X-3, BOARD_Y-3, COLS*CELL+6, ROWS*CELL+6, (Color){65,80,125,255});
    DrawRectangle(BOARD_X, BOARD_Y, COLS*CELL, ROWS*CELL, (Color){10,31,24,255});
    for (int i=0;i<snake_len;i++) DrawRectangle(BOARD_X+snake[i].x*CELL+2, BOARD_Y+snake[i].y*CELL+2, CELL-4, CELL-4, i ? (Color){50,180,100,255} : LIME);
    DrawCircle(BOARD_X+food.x*CELL+CELL/2, BOARD_Y+food.y*CELL+CELL/2, 9, RED);
}


static Vector2 player_rot(){
    return (Vector2) {sin(player.rotation*DEG2RAD),cos(player.rotation*DEG2RAD)};
}

static void init_rock(int i){
    rocks[i]=(Rock){GetRandomValue(25,W-25),GetRandomValue(-150,-20),rocks[i].speed+5,GetRandomValue(16,30)};
}

static void draw_asteroids(void) {
    float dt=GetFrameTime();
    if (joystick_is_held(LEFT)) player.position.x-=340*dt;
    if (joystick_is_held(RIGHT)) player.position.x+=340*dt;
    if (player.position.x<24) player.position.x=24;
    if (player.position.x>W-24) player.position.x=W-24;
    asteroid_score=(int)((GetTime()-asteroid_at)*10)+destroyedMeteorsCount*20;
    for (int i=0;i<ROCKS;i++) {
        rocks[i].y+=rocks[i].speed*dt;
        if (rocks[i].y>H+rocks[i].radius) init_rock(i);
        float x=rocks[i].x-player.position.x, y=rocks[i].y-(H-55), r=rocks[i].radius+18;
        if (x*x+y*y<r*r) { game_over("Asteroides", asteroid_score, ASTEROID_OVER); return; }
    }
    
    // Player shoot logic
    if (button_was_pressed(UP) || button_was_pressed(DOWN))
    {
        for (int i = 0; i < PLAYER_MAX_SHOTS; i++)
        {
            if (!shots[i].active)
            {
                Vector2 rot = player_rot();
                shots[i].position = (Vector2){ player.position.x + rot.x*(shipHeight), 440 };
                shots[i].active = true;
                shots[i].speed.x = 1.5*rot.x*PLAYER_SPEED;
                shots[i].speed.y = 1.5*rot.y*PLAYER_SPEED;
                shots[i].rotation = player.rotation;
                melody_play(SHOT_FIRE); /* substitui o clique da tecla */
                break;
            }
        }
    }
    
     // Shoot logic
    for (int i = 0; i < PLAYER_MAX_SHOTS; i++)
    {
        if (shots[i].active)
        {
            shots[i].lifeSpawn++;
            // Movement
            shots[i].position.x += shots[i].speed.x*dt*60;  /* speed era px/frame a 60 FPS */
            shots[i].position.y -= shots[i].speed.y*dt*60;

            // Collision logic: shoot vs walls
            if  ((shots[i].position.x > screenWidth + shots[i].radius) || (shots[i].position.x < 0 - shots[i].radius) || (shots[i].position.y > screenHeight + shots[i].radius) || (shots[i].position.y < 0 - shots[i].radius))
            {
                shots[i].active = false;
                shots[i].lifeSpawn = 0;
            }

            // Life of shoot
            if (shots[i].lifeSpawn >= 60) init_shot(&shots[i]);
            DrawCircleV(shots[i].position, shots[i].radius, shots[i].color);
        }
    }
        
    // Collision logic: player-shoots vs meteors
    for (int i = 0; i < PLAYER_MAX_SHOTS; i++)
    {
        if (shots[i].active)
        {
            for (int a = 0; a < ROCKS; a++)
            {
                if (CheckCollisionCircles(shots[i].position, shots[i].radius, (Vector2){rocks[a].x,rocks[a].y}, rocks[a].radius))
                {
                    shots[i].active = false;
                    shots[i].lifeSpawn = 0;
                    // bigMeteor[a].active = false;
                    destroyedMeteorsCount++;
                    melody_play(ROCK_BOOM);
                    
                    printf("%f", rocks[a].radius);
                    if (rocks[a].radius > 18) { rocks[a].radius=(int)rocks[a].radius*0.7; rocks[a].y+=20; }
                    else init_rock(a);

                    /* for (int j = 0; j < 2; j ++)
                    {
                        mediumMeteor[midMeteorsCount].position = (Vector2){bigMeteor[a].position.x, bigMeteor[a].position.y};
                       // mediumMeteor[midMeteorsCount].speed = (Vector2){cos(shots[i].rotation*DEG2RAD)*METEORS_SPEED*-1, sin(shots[i].rotation*DEG2RAD)*METEORS_SPEED*-1};

                        mediumMeteor[midMeteorsCount].active = true;
                        midMeteorsCount ++;
                    } */
                    //bigMeteor[a].position = (Vector2){-100, -100};
                    //rocks[a].color = RED;
                }
            }
        }
    }
    
    draw_header("ASTEROIDES", asteroid_score);
    DrawText("Joystick esquerda/direita", CX-115, 72, 18, LIGHTGRAY);
    for (int y=100;y<H;y+=43) DrawCircle((y*17)%W,y,1.5f,(Color){185,185,255,170});
    for (int i=0;i<ROCKS;i++) DrawCircleV((Vector2){rocks[i].x,rocks[i].y},rocks[i].radius,GRAY);
    DrawTriangle((Vector2){player.position.x,H-90},(Vector2){player.position.x-20,H-35},(Vector2){player.position.x+20,H-35},SKYBLUE);
}

static void draw_rhythm(void) {
    rhythm_clock += GetFrameTime() * 1000 * rhythm_speed();
    const Note *n = &FUR_ELISE[rhythm_next % SONG_N];
    double off = rhythm_clock - rhythm_time(rhythm_next);
    for (int l = 0; l < 4; l++) {
        if (!button_was_pressed(LANE_KEYS[l])) continue;
        if (l != rhythm_lane(n->f) || fabs(off) > RH_WINDOW) { rhythm_lives--; continue; }
        if (!melody_on) { /* som = metade do espaco ate a proxima nota (teto 300 ms): sobra folga p/ acerto adiantado */
            int len = (int)(n->ms / rhythm_speed()) / 2;
            rhythm_note[0] = (Note){n->f, len < 40 ? 40 : len > 300 ? 300 : len};
            melody_play(rhythm_note);
        }
        rhythm_score += 10; rhythm_next++; off = 0; break;
    }
    if (off > RH_WINDOW) { rhythm_lives--; rhythm_next++; } /* bloco passou da linha */
    if (rhythm_lives <= 0) { game_over("Ritmo", rhythm_score, SNAKE_OVER); return; }

    for (int l = 0; l < 4; l++) DrawRectangle(RH_X + l*RH_LANE_W, RH_TOP, RH_LANE_W, H - RH_TOP, l % 2 ? (Color){20,24,48,255} : (Color){16,19,40,255});
    DrawRectangle(RH_X, RH_HIT_Y - RH_TILE_H/2, 4*RH_LANE_W, RH_TILE_H, Fade(WHITE, .06f));
    BeginScissorMode(0, RH_TOP, W, H - RH_TOP);
    for (int k = rhythm_next; ; k++) {
        float y = RH_HIT_Y - (rhythm_time(k) - rhythm_clock) * RH_PX;
        if (y < RH_TOP - RH_TILE_H) break;
        int l = rhythm_lane(FUR_ELISE[k % SONG_N].f);
        DrawRectangleRounded((Rectangle){RH_X + l*RH_LANE_W + 6, y - RH_TILE_H/2, RH_LANE_W - 12, RH_TILE_H}, .25f, 8, LANE_COLORS[l]);
    }
    EndScissorMode();
    DrawLine(RH_X, RH_HIT_Y, RH_X + 4*RH_LANE_W, RH_HIT_Y, RAYWHITE);
    for (int l = 0; l < 4; l++)
        DrawRectangleRounded((Rectangle){RH_X + l*RH_LANE_W + 10, 434, RH_LANE_W - 20, 34}, .4f, 8, is_held(LANE_KEYS[l]) ? LANE_COLORS[l] : Fade(LANE_COLORS[l], .35f));
    draw_header("RITMO", rhythm_score);
    text_center(TextFormat("Fur Elise - Beethoven   |   Vidas: %d", rhythm_lives), 68, 18, LIGHTGRAY);
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
                if (was_pressed(UP)) { const State games[GAME_COUNT] = {SNAKE, ASTEROIDS, RHYTHM}; start_game(games[selected], card, credits); }
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
            static const char *score_games[GAME_COUNT] = {"Cobrinha", "Asteroides", "Ritmo"};
            const Color score_colors[GAME_COUNT] = {LIME, SKYBLUE, VIOLET};
            DrawText("PLACAR", CX-58, 24, 32, GOLD);
            for (int g = 0; g < GAME_COUNT; g++) {
                ScoreEntry e[10]; int n = score_load(e, 10, score_games[g]); int x = 40 + g*260;
                DrawText(GAME_NAMES[g], x + 20, 75, 22, score_colors[g]);
                if (g) DrawLine(x - 20, 68, x - 20, 390, (Color){80,90,145,255});
                if (n == 0) DrawText("Sem scores", x + 20, 115, 18, LIGHTGRAY);
                for (int i = 0; i < n; i++) {
                    char display_code[4] = "---"; card_code_read(e[i].card, display_code);
                    DrawText(TextFormat("%d.", i+1), x, 112+i*27, 18, YELLOW);
                    DrawText(display_code, x + 40, 112+i*27, 18, RAYWHITE);
                    DrawText(TextFormat("%d", e[i].score), x + 150, 112+i*27, 18, GREEN);
                }
            }
            DrawText("Esquerda ou direita para voltar", CX-135, 430, 17, LIGHTGRAY);
        } else {
            if (state==SNAKE) draw_snake();
            else if (state==ASTEROIDS) draw_asteroids();
            else if (state==RHYTHM) draw_rhythm();
            else if (message_record) {
                DrawRectangleRounded((Rectangle){110,140,580,200},.15f,8,(Color){60,45,10,255}); DrawRectangleRoundedLines((Rectangle){110,140,580,200},.15f,8,GOLD);
                DrawText("NOVO RECORDE!",CX-MeasureText("NOVO RECORDE!",42)/2,170,42,GOLD);
                DrawText(note,CX-MeasureText(note,27)/2,250,27,WHITE);
            }
            else { DrawRectangleRounded((Rectangle){110,160,580,150},.15f,8,(Color){35,40,82,255}); DrawText(note,CX-MeasureText(note,27)/2,215,27,WHITE); }
        }
        EndDrawing();
    }
    digitalWrite(PIN_BUZZER, LOW);
    CloseWindow(); pthread_mutex_destroy(&app.lock); return 0;
}
