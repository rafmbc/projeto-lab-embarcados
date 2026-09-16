/* Arcade RFID para Raspberry Pi + Projects Board Freenove.
 * Hardware: direcional BCM 26/20/16/21 e buzzer ativo BCM 12.
 * O modo RFID_DUMMY permite testar no PC com as setas/WASD. */
#define _DEFAULT_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "raylib.h"

#ifndef RFID_DUMMY
#include <wiringPi.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include "mfrc522.h"
#else
typedef int MFRC522_Status_t;
#define MI_OK 0
#define MI_NOTAGERR -1
#define PICC_REQIDL 0x26
static inline int MFRC522_Init(char t) { (void)t; return -1; }
static inline int MFRC522_Request(uint8_t m, uint8_t *t) { (void)m; (void)t; return MI_NOTAGERR; }
static inline int MFRC522_Anticoll(uint8_t *s) { (void)s; return MI_NOTAGERR; }
static inline int MFRC522_SelectTag(uint8_t *s) { (void)s; return 0; }
static inline void MFRC522_Halt(void) {}
#endif

#define W 800
#define H 480
#define CX (W/2)
#define CSV_FILE "cartoes.csv"
#define SCORE_FILE "placar.csv"
#define PIN_UP 20
#define PIN_LEFT 26
#define PIN_RIGHT 16
#define PIN_DOWN 21
#define PIN_BUZZER 12
#define PIN_JOYSTICK_Z 7
#define ADC_I2C_ADDRESS 0x48
#define JOYSTICK_X_CHANNEL 5
#define JOYSTICK_Y_CHANNEL 6
#define JOYSTICK_LOW 80
#define JOYSTICK_HIGH 175
#define COST 1

enum { UP = 1, LEFT = 2, RIGHT = 4, DOWN = 8 };
typedef enum { WAIT_CARD, MENU, RECHARGE, SCORES, SNAKE, ASTEROIDS, MESSAGE } State;
typedef struct { int x, y; } Cell;
typedef struct { float x, y, speed, radius; } Rock;
typedef struct { char card[9]; char game[16]; int score; } ScoreEntry;
typedef struct {
    pthread_mutex_t lock;
    State state;
    char card[9];
    int credits;
    int rfid_online;
    char message[96];
    int message_to_menu;
} App;

static App app;
static unsigned held, pressed, button_pressed, joystick_held;
#ifndef RFID_DUMMY
static int adc_fd = -1;
#endif
static int buzz_edges;
static double buzz_at, message_at;
static int selected, recharge_selected;
static char active_card[9];

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
static Rock rocks[ROCKS];
static float ship_x;
static int asteroid_score;
static double asteroid_at;

static void card_string(const uint8_t *id, char *out) {
    snprintf(out, 9, "%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
}

static void csv_init(void) {
    FILE *f = fopen(CSV_FILE, "r");
    if (f) { fclose(f); return; }
    f = fopen(CSV_FILE, "w");
    if (f) { fputs("CardID,Credito\n", f); fclose(f); }
}

static int csv_read(const char *card) {
    FILE *f = fopen(CSV_FILE, "r");
    char line[64], id[9]; int value;
    if (!f) return 0;
    fgets(line, sizeof line, f);
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%8[^,],%d", id, &value) == 2 && !strcmp(id, card)) {
            fclose(f); return value;
        }
    fclose(f); return 0;
}

static int csv_write(const char *card, int credits) {
    FILE *in = fopen(CSV_FILE, "r"), *out;
    char lines[128][64], id[9]; int value, count = 0, found = 0;
    if (!in) return -1;
    while (count < 128 && fgets(lines[count], sizeof lines[count], in)) {
        if (count && sscanf(lines[count], "%8[^,],%d", id, &value) == 2 && !strcmp(id, card)) {
            snprintf(lines[count], sizeof lines[count], "%s,%d\n", card, credits);
            found = 1;
        }
        count++;
    }
    fclose(in);
    if (!found && count < 128) snprintf(lines[count++], sizeof lines[0], "%s,%d\n", card, credits);
    out = fopen(CSV_FILE, "w");
    if (!out) return -1;
    for (int i = 0; i < count; i++) fputs(lines[i], out);
    fclose(out); return 0;
}

static void score_init(void) {
    FILE *f = fopen(SCORE_FILE, "r");
    if (f) { fclose(f); return; }
    f = fopen(SCORE_FILE, "w");
    if (f) { fputs("CardID,Jogo,Pontuacao\n", f); fclose(f); }
}

static void score_record(const char *card, const char *game, int score) {
    FILE *in, *out; char lines[128][80], id[9], name[16]; int value, count = 0, found = 0;
    score_init(); in = fopen(SCORE_FILE, "r"); if (!in) return;
    while (count < 128 && fgets(lines[count], sizeof lines[count], in)) {
        if (count && sscanf(lines[count], "%8[^,],%15[^,],%d", id, name, &value) == 3 && !strcmp(id, card) && !strcmp(name, game)) {
            if (score > value) snprintf(lines[count], sizeof lines[count], "%s,%s,%d\n", card, game, score);
            found = 1;
        }
        count++;
    }
    fclose(in);
    if (!found && count < 128) snprintf(lines[count++], sizeof lines[0], "%s,%s,%d\n", card, game, score);
    out = fopen(SCORE_FILE, "w"); if (!out) return;
    for (int i = 0; i < count; i++) fputs(lines[i], out);
    fclose(out);
}

static int score_load(ScoreEntry *entries, int limit, const char *game) {
    FILE *f; char line[80]; int count = 0; ScoreEntry candidate;
    score_init(); f = fopen(SCORE_FILE, "r"); if (!f) return 0;
    fgets(line, sizeof line, f);
    while (count < limit && fgets(line, sizeof line, f))
        if (sscanf(line, "%8[^,],%15[^,],%d", candidate.card, candidate.game, &candidate.score) == 3 && !strcmp(candidate.game, game))
            entries[count++] = candidate;
    fclose(f);
    for (int i = 0; i < count; i++) for (int j = i + 1; j < count; j++)
        if (entries[j].score > entries[i].score) { ScoreEntry tmp = entries[i]; entries[i] = entries[j]; entries[j] = tmp; }
    return count;
}

static void buzzer_play(int pulses) {
#ifndef RFID_DUMMY
    buzz_edges = pulses * 2;
    buzz_at = GetTime();
#else
    (void)pulses;
#endif
}

static void controls_init(void) {
#ifndef RFID_DUMMY
    if (wiringPiSetupGpio() == -1) { fputs("Erro ao iniciar GPIO.\n", stderr); return; }
    const int pins[] = { PIN_UP, PIN_LEFT, PIN_RIGHT, PIN_DOWN, PIN_JOYSTICK_Z };
    for (int i = 0; i < 5; i++) { pinMode(pins[i], INPUT); pullUpDnControl(pins[i], PUD_UP); }
    pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
    adc_fd = open("/dev/i2c-1", O_RDWR);
    if (adc_fd >= 0 && ioctl(adc_fd, I2C_SLAVE, ADC_I2C_ADDRESS) < 0) { close(adc_fd); adc_fd = -1; }
    if (adc_fd < 0) fputs("Joystick analogico indisponivel: habilite I2C e verifique ADS7830 (0x48).\n", stderr);
#endif
}

static int ads7830_read(int channel) {
#ifndef RFID_DUMMY
    unsigned char command = (unsigned char)(0x84 | ((((channel << 2) | (channel >> 1)) & 0x07) << 4));
    unsigned char value;
    if (adc_fd < 0 || write(adc_fd, &command, 1) != 1 || read(adc_fd, &value, 1) != 1) return -1;
    return value;
#else
    (void)channel;
    return -1;
#endif
}

static void controls_poll(void) {
    unsigned old = held;
#ifndef RFID_DUMMY
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
    if (button_sampled != buttons_held && GetTime() - button_sampled_at >= 0.035) buttons_held = button_sampled;
    if (raw != sampled) { sampled = raw; sampled_at = GetTime(); }
    if (sampled != held && GetTime() - sampled_at >= 0.035) held = sampled;
    button_pressed = buttons_held & ~old_buttons;
#else
    held = (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W) ? UP : 0) |
           (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A) ? LEFT : 0) |
           (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D) ? RIGHT : 0) |
           (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S) ? DOWN : 0);
    joystick_held = held;
    button_pressed = held & ~old;
#endif
    pressed = held & ~old;
    if (pressed) buzzer_play(1);
#ifndef RFID_DUMMY
    if (buzz_edges > 0 && GetTime() >= buzz_at) {
        digitalWrite(PIN_BUZZER, (buzz_edges & 1) == 0 ? HIGH : LOW);
        buzz_edges--; buzz_at = GetTime() + 0.07;
    } else if (!buzz_edges) digitalWrite(PIN_BUZZER, LOW);
#endif
}

static int was_pressed(unsigned key) { return (pressed & key) != 0; }
static int button_was_pressed(unsigned key) { return (button_pressed & key) != 0; }
static int is_held(unsigned key) { return (held & key) != 0; }
static int joystick_is_held(unsigned key) { return (joystick_held & key) != 0; }

static void show_message(const char *text, int return_menu) {
    pthread_mutex_lock(&app.lock);
    snprintf(app.message, sizeof app.message, "%s", text);
    app.message_to_menu = return_menu;
    app.state = MESSAGE;
    pthread_mutex_unlock(&app.lock);
    message_at = GetTime();
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
        char card[9]; card_string(serial, card); csv_init();
        pthread_mutex_lock(&app.lock);
        strcpy(app.card, card); app.credits = csv_read(card); app.state = MENU;
        pthread_mutex_unlock(&app.lock);
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

static void start_snake(void) {
    snake_len = 4; for (int i = 0; i < snake_len; i++) snake[i] = (Cell){8-i, 5};
    dx = next_dx = 1; dy = next_dy = 0; snake_score = 0; snake_at = GetTime()+0.18; snake_food();
}

static void start_asteroids(void) {
    ship_x = CX; asteroid_score = 0; asteroid_at = GetTime();
    for (int i = 0; i < ROCKS; i++) rocks[i] = (Rock){GetRandomValue(25,W-25), GetRandomValue(-H,0), GetRandomValue(120,220), GetRandomValue(16,30)};
}

static void start_game(State game, const char *card, int credits) {
    if (credits < COST) { show_message("Creditos insuficientes", 1); buzzer_play(3); return; }
    if (csv_write(card, credits-COST) < 0) { show_message("Erro ao salvar cartao", 1); buzzer_play(3); return; }
    pthread_mutex_lock(&app.lock); app.credits = credits-COST; app.state = game; pthread_mutex_unlock(&app.lock);
    strcpy(active_card, card);
    if (game == SNAKE) start_snake(); else start_asteroids();
    buzzer_play(2);
}

static void recharge(const char *card, int credits, int value) {
    if (csv_write(card, credits + value) < 0) { show_message("Erro ao salvar recarga", 1); buzzer_play(3); return; }
    pthread_mutex_lock(&app.lock); app.credits = credits + value; pthread_mutex_unlock(&app.lock);
    show_message(TextFormat("Recarga de +%d credito(s)", value), 1); buzzer_play(2);
}

static void draw_header(const char *title, int score) {
    DrawText(title, 24, 18, 28, RAYWHITE);
    DrawText(TextFormat("Pontos: %d", score), W-170, 24, 20, YELLOW);
    DrawLine(0, 55, W, 55, (Color){90,90,155,255});
}

static void draw_snake(void) {
    if (button_was_pressed(UP) && dy != 1) { next_dx=0; next_dy=-1; }
    if (button_was_pressed(DOWN) && dy != -1) { next_dx=0; next_dy=1; }
    if (button_was_pressed(LEFT) && dx != 1) { next_dx=-1; next_dy=0; }
    if (button_was_pressed(RIGHT) && dx != -1) { next_dx=1; next_dy=0; }
    if (GetTime() >= snake_at) {
        Cell next = {snake[0].x+next_dx, snake[0].y+next_dy};
        int hit = next.x<0 || next.x>=COLS || next.y<0 || next.y>=ROWS;
        for (int i=0;i<snake_len;i++) if (snake[i].x==next.x && snake[i].y==next.y) hit=1;
        if (hit) { score_record(active_card, "Cobrinha", snake_score); show_message(TextFormat("Cobrinha: %d pontos", snake_score), 1); buzzer_play(3); return; }
        int ate = next.x==food.x && next.y==food.y; if (ate) snake_len++;
        for (int i=snake_len-1; i>0; i--) snake[i]=snake[i-1];
        snake[0]=next; dx=next_dx; dy=next_dy;
        if (ate) { snake_score += 10; snake_food(); } snake_at=GetTime()+0.13;
    }
    draw_header("COBRINHA", snake_score);
    DrawText("Use os botoes direcionais", CX-130, 72, 18, LIGHTGRAY);
    DrawRectangle(BOARD_X-3, BOARD_Y-3, COLS*CELL+6, ROWS*CELL+6, (Color){65,80,125,255});
    DrawRectangle(BOARD_X, BOARD_Y, COLS*CELL, ROWS*CELL, (Color){10,31,24,255});
    for (int i=0;i<snake_len;i++) DrawRectangle(BOARD_X+snake[i].x*CELL+2, BOARD_Y+snake[i].y*CELL+2, CELL-4, CELL-4, i ? (Color){50,180,100,255} : LIME);
    DrawCircle(BOARD_X+food.x*CELL+CELL/2, BOARD_Y+food.y*CELL+CELL/2, 9, RED);
}

static void draw_asteroids(void) {
    float dt=GetFrameTime();
    if (joystick_is_held(LEFT)) ship_x-=340*dt;
    if (joystick_is_held(RIGHT)) ship_x+=340*dt;
    if (ship_x<24) ship_x=24;
    if (ship_x>W-24) ship_x=W-24;
    asteroid_score=(int)((GetTime()-asteroid_at)*10);
    for (int i=0;i<ROCKS;i++) {
        rocks[i].y+=rocks[i].speed*dt;
        if (rocks[i].y>H+rocks[i].radius) rocks[i]=(Rock){GetRandomValue(25,W-25),GetRandomValue(-150,-20),rocks[i].speed+5,GetRandomValue(16,30)};
        float x=rocks[i].x-ship_x, y=rocks[i].y-(H-55), r=rocks[i].radius+18;
        if (x*x+y*y<r*r) { score_record(active_card, "Asteroides", asteroid_score); show_message(TextFormat("Asteroides: %d pontos", asteroid_score), 1); buzzer_play(3); return; }
    }
    draw_header("ASTEROIDES", asteroid_score);
    DrawText("Joystick esquerda/direita", CX-115, 72, 18, LIGHTGRAY);
    for (int y=100;y<H;y+=43) DrawCircle((y*17)%W,y,1.5f,(Color){185,185,255,170});
    for (int i=0;i<ROCKS;i++) DrawCircleV((Vector2){rocks[i].x,rocks[i].y},rocks[i].radius,GRAY);
    DrawTriangle((Vector2){ship_x,H-90},(Vector2){ship_x-20,H-35},(Vector2){ship_x+20,H-35},SKYBLUE);
}

int main(void) {
    int rfid_ok = MFRC522_Init('B') == 0;
    pthread_mutex_init(&app.lock, NULL); app.state=WAIT_CARD; app.rfid_online=rfid_ok;
    if (rfid_ok) { pthread_t thread; pthread_create(&thread,NULL,rfid_loop,NULL); pthread_detach(thread); }
    InitWindow(W,H,"Arcade RFID"); SetTargetFPS(60); controls_init();
    while (!WindowShouldClose()) {
        controls_poll();
        pthread_mutex_lock(&app.lock); State state=app.state; char card[9]; strcpy(card,app.card); int credits=app.credits; char note[96]; strcpy(note,app.message); int back=app.message_to_menu; pthread_mutex_unlock(&app.lock);
#ifdef RFID_DUMMY
        if (state==WAIT_CARD && (was_pressed(RIGHT) || IsKeyPressed(KEY_ENTER))) { csv_init(); strcpy(card,"DEMO0001"); pthread_mutex_lock(&app.lock); strcpy(app.card,card); app.credits=csv_read(card); app.state=MENU; pthread_mutex_unlock(&app.lock); buzzer_play(2); }
#endif
        if (state==MESSAGE && GetTime()-message_at>2.2) { pthread_mutex_lock(&app.lock); app.state=back?MENU:(rfid_ok?WAIT_CARD:MENU); pthread_mutex_unlock(&app.lock); }
        if (state==MENU) {
            if (was_pressed(UP) && selected > 0) selected--;
            if (was_pressed(DOWN) && selected < 5) selected++;
            if (was_pressed(LEFT)) { pthread_mutex_lock(&app.lock); app.state=WAIT_CARD; pthread_mutex_unlock(&app.lock); }
            if (was_pressed(RIGHT)) {
                if (selected==0) start_game(SNAKE,card,credits);
                else if (selected==1) start_game(ASTEROIDS,card,credits);
                else if (selected==2) { recharge_selected=0; pthread_mutex_lock(&app.lock); app.state=RECHARGE; pthread_mutex_unlock(&app.lock); }
                else if (selected==3) show_message(TextFormat("Saldo: %d credito(s)",credits),1);
                else if (selected==4) { pthread_mutex_lock(&app.lock); app.state=SCORES; pthread_mutex_unlock(&app.lock); }
                else { pthread_mutex_lock(&app.lock); app.state=WAIT_CARD; pthread_mutex_unlock(&app.lock); }
            }
        }
        if (state==SCORES && (was_pressed(LEFT) || was_pressed(RIGHT))) {
            pthread_mutex_lock(&app.lock); app.state=MENU; pthread_mutex_unlock(&app.lock);
        }
        if (state==RECHARGE) {
            if (was_pressed(UP) && recharge_selected > 0) recharge_selected--;
            if (was_pressed(DOWN) && recharge_selected < 3) recharge_selected++;
            if (was_pressed(LEFT)) { pthread_mutex_lock(&app.lock); app.state=MENU; pthread_mutex_unlock(&app.lock); }
            if (was_pressed(RIGHT)) {
                const int packs[] = {1, 5, 10};
                if (recharge_selected < 3) recharge(card,credits,packs[recharge_selected]);
                else { pthread_mutex_lock(&app.lock); app.state=MENU; pthread_mutex_unlock(&app.lock); }
            }
        }
        BeginDrawing(); ClearBackground((Color){12,14,29,255});
        if (state==WAIT_CARD) {
            DrawText("ARCADE RFID", CX-105,75,32,RAYWHITE); DrawCircleLines(CX,205,78,(Color){100,120,255,220}); DrawCircle(CX,205,44,(Color){55,70,190,255});
            DrawText("RFID",CX-28,196,21,WHITE);
            const char *prompt = rfid_ok ? "Aproxime o cartao" : "Modo demo: pressione DIREITA";
            DrawText(prompt, CX - MeasureText(prompt, 22)/2, 315, 22, rfid_ok ? RAYWHITE : GOLD);
        } else if (state==MENU) {
            DrawText("ARCADE RFID",CX-105,18,28,RAYWHITE); DrawText(TextFormat("Cartao %s   |   Creditos: %d",card,credits),CX-170,58,20,GOLD);
            const char *items[] = {"COBRINHA  -  1 credito","ASTEROIDES  -  1 credito","RECARREGAR CREDITOS","CONSULTAR SALDO","PLACAR","ENCERRAR CARTAO"};
            for (int i=0;i<6;i++) { Color c=i==selected?(Color){70,110,220,255}:(Color){35,45,85,255}; DrawRectangleRounded((Rectangle){180,82+i*50,440,39},.2f,8,c); DrawText(items[i],230,91+i*50,18,WHITE); if(i==selected)DrawText(">",195,91+i*50,20,YELLOW); }
            DrawText("Joystick: cima/baixo seleciona | direita confirma | esquerda volta",65,425,16,LIGHTGRAY);
        } else if (state==RECHARGE) {
            const char *packs[] = {"+1 CREDITO","+5 CREDITOS","+10 CREDITOS","VOLTAR"};
            DrawText("RECARGA LIVRE", CX-120,75,30,GOLD);
            DrawText(TextFormat("Cartao %s   |   Saldo: %d",card,credits),CX-160,112,20,RAYWHITE);
            for (int i=0;i<4;i++) { Color c=i==recharge_selected?(Color){50,150,90,255}:(Color){35,65,55,255}; DrawRectangleRounded((Rectangle){205,155+i*55,390,42},.2f,8,c); DrawText(packs[i],270,165+i*55,19,WHITE); if(i==recharge_selected)DrawText(">",220,165+i*55,22,YELLOW); }
            DrawText("Qualquer usuario pode recarregar neste modo demo",150,395,17,LIGHTGRAY);
            DrawText("Cima/baixo seleciona | direita confirma | esquerda volta",140,425,16,LIGHTGRAY);
        } else if (state==SCORES) {
            ScoreEntry snake_scores[10], asteroid_scores[10];
            int snake_count = score_load(snake_scores, 10, "Cobrinha");
            int asteroid_count = score_load(asteroid_scores, 10, "Asteroides");
            DrawText("PLACAR", CX-58, 24, 32, GOLD);
            DrawText("COBRINHA", 145, 75, 24, LIME);
            DrawText("ASTEROIDES", 475, 75, 24, SKYBLUE);
            DrawLine(CX, 68, CX, 390, (Color){80,90,145,255});
            if (snake_count == 0) DrawText("Sem scores", 145, 115, 18, LIGHTGRAY);
            if (asteroid_count == 0) DrawText("Sem scores", 475, 115, 18, LIGHTGRAY);
            for (int i = 0; i < snake_count; i++) {
                DrawText(TextFormat("%d.", i+1), 95, 112+i*27, 18, YELLOW);
                DrawText(snake_scores[i].card, 135, 112+i*27, 18, RAYWHITE);
                DrawText(TextFormat("%d", snake_scores[i].score), 300, 112+i*27, 18, GREEN);
            }
            for (int i = 0; i < asteroid_count; i++) {
                DrawText(TextFormat("%d.", i+1), 430, 112+i*27, 18, YELLOW);
                DrawText(asteroid_scores[i].card, 470, 112+i*27, 18, RAYWHITE);
                DrawText(TextFormat("%d", asteroid_scores[i].score), 635, 112+i*27, 18, GREEN);
            }
            DrawText("Esquerda ou direita para voltar", CX-135, 430, 17, LIGHTGRAY);
        } else if (state==SNAKE) draw_snake();
        else if (state==ASTEROIDS) draw_asteroids();
        else { DrawRectangleRounded((Rectangle){110,160,580,150},.15f,8,(Color){35,40,82,255}); DrawText(note,CX-MeasureText(note,27)/2,215,27,WHITE); }
        EndDrawing();
    }
#ifndef RFID_DUMMY
    digitalWrite(PIN_BUZZER,LOW);
#endif
    CloseWindow(); pthread_mutex_destroy(&app.lock); return 0;
}
