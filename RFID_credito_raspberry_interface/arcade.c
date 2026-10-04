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
#define CSV_HEADER "CardID,Credito,Codigo,Pont_Cobrinha,Pont_Asteroides\n"
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

enum { UP = 1, LEFT = 2, RIGHT = 4, DOWN = 8 };
typedef enum { WAIT_CARD, CARD_CODE, MENU, RECHARGE, SCORES, SNAKE, ASTEROIDS, MESSAGE } State;
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
    int message_to_menu;
} App;

static App app;
static unsigned held, pressed, button_pressed, joystick_held;
static int adc_fd = -1;

static int buzz_edges;
static double buzz_at, message_at;
static int selected, recharge_selected;
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
typedef struct { char card[9]; char code[4]; int credits; int snake; int asteroid; } Row;

static int csv_load(Row *rows, int limit) {
    FILE *f = fopen(CSV_FILE, "r"); if (!f) return 0;
    char line[CSV_LINE_MAX]; int n = 0;
    fgets(line, sizeof line, f);
    while (n < limit && fgets(line, sizeof line, f)) {
        Row r = {0};
        sscanf(line, "%8[^,],%d,%3[^,],%d,%d", r.card, &r.credits, r.code, &r.snake, &r.asteroid);
        if (r.card[0]) rows[n++] = r;
    }
    fclose(f); return n;
}

static void csv_save(Row *rows, int n) {
    FILE *f = fopen(CSV_FILE, "w"); if (!f) return;
    fputs(CSV_HEADER, f);
    for (int i = 0; i < n; i++)
        fprintf(f, "%s,%d,%s,%d,%d\n", rows[i].card, rows[i].credits, rows[i].code, rows[i].snake, rows[i].asteroid);
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
static void score_record(const char *card, const char *game, int score) {
    Row rows[128]; int n = csv_load(rows, 128);
    Row *r = csv_find(rows, n, card);
    if (!r) { if (n >= 128) return; strncpy(rows[n].card, card, 8); rows[n].credits = csv_read(card); r = &rows[n++]; }
    if (!strcmp(game, "Cobrinha")  && score > r->snake)    r->snake    = score;
    if (!strcmp(game, "Asteroides") && score > r->asteroid) r->asteroid = score;
    csv_save(rows, n);
}

static int score_load(ScoreEntry *entries, int limit, const char *game) {
    Row rows[128]; int n = csv_load(rows, 128), count = 0;
    int is_snake = !strcmp(game, "Cobrinha");
    for (int i = 0; i < n && count < limit; i++) {
        int s = is_snake ? rows[i].snake : rows[i].asteroid;
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
#define BUZZ_DUTY 0.06
#define NOTE_GAP_MS 90
typedef struct { int f, ms; } Note;
static const Note SNAKE_START[]    = {{523,90},{659,90},{784,90},{1047,200},{0,0}};
static const Note SNAKE_OVER[]     = {{392,160},{349,160},{330,160},{262,160},{196,400},{0,0}};
static const Note ASTEROID_START[] = {{262,80},{392,80},{523,80},{392,80},{523,80},{784,220},{0,0}};
static const Note ASTEROID_OVER[]  = {{784,120},{659,120},{523,120},{415,120},{330,120},{220,120},{110,450},{0,0}};
static volatile int melody_on;
static void *melody_thread(void *arg) {
    for (const Note *n = arg; n->ms; n++) {
        int period = 1000000/n->f, high = (int)(period*BUZZ_DUTY), cycles = n->ms*1000/period;
        for (int c = 0; c < cycles; c++) {
            digitalWrite(PIN_BUZZER, HIGH); delayMicroseconds(high);
            digitalWrite(PIN_BUZZER, LOW);  delayMicroseconds(period-high);
        }
        usleep(NOTE_GAP_MS*1000);
    }
    melody_on = 0; return NULL;
}

static void melody_play(const Note *notes) {
    if (melody_on) return;
    melody_on = 1; pthread_t t; pthread_create(&t, NULL, melody_thread, (void *)notes); pthread_detach(t);
}

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
    if (pressed) buzzer_play(1);
    if (buzz_edges > 0 && GetTime() >= buzz_at) {
        digitalWrite(PIN_BUZZER, (buzz_edges & 1) == 0 ? HIGH : LOW);
        buzz_edges--; buzz_at = GetTime() + 0.07;
    } else if (!buzz_edges && !melody_on) digitalWrite(PIN_BUZZER, LOW);
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

static void start_game(State game, const char *card, int credits) {
    if (credits < COST) { show_message("Creditos insuficientes", 1); buzzer_play(3); return; }
    if (csv_write(card, credits-COST) < 0) { show_message("Erro ao salvar cartao", 1); buzzer_play(3); return; }
    pthread_mutex_lock(&app.lock); app.credits = credits-COST; app.state = game; pthread_mutex_unlock(&app.lock);
    strcpy(active_card, card);
    if (game == SNAKE) start_snake(); else start_asteroids();
    melody_play(game == SNAKE ? SNAKE_START : ASTEROID_START);
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
    if ((button_was_pressed(UP) || joystick_is_held(UP)) && dy != 1) { next_dx=0; next_dy=-1; }
    if ((button_was_pressed(DOWN) || joystick_is_held(DOWN)) && dy != -1) { next_dx=0; next_dy=1; }
    if ((button_was_pressed(LEFT) || joystick_is_held(LEFT)) && dx != 1) { next_dx=-1; next_dy=0; }
    if ((button_was_pressed(RIGHT) || joystick_is_held(RIGHT)) && dx != -1) { next_dx=1; next_dy=0; }
    if (GetTime() >= snake_at) {
        Cell next = {snake[0].x+next_dx, snake[0].y+next_dy};
        int hit = next.x<0 || next.x>=COLS || next.y<0 || next.y>=ROWS;
        for (int i=0;i<snake_len;i++) if (snake[i].x==next.x && snake[i].y==next.y) hit=1;
        if (hit) { score_record(active_card, "Cobrinha", snake_score); show_message(TextFormat("Cobrinha: %d pontos", snake_score), 1); melody_play(SNAKE_OVER); return; }
        int ate = next.x==food.x && next.y==food.y; if (ate) snake_len++;
        for (int i=snake_len-1; i>0; i--) snake[i]=snake[i-1];
        snake[0]=next; dx=next_dx; dy=next_dy;
        if (ate) { snake_score += 10; snake_speed*=0.93; snake_food(); } snake_at=GetTime()+snake_speed;
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
        if (x*x+y*y<r*r) { score_record(active_card, "Asteroides", asteroid_score); show_message(TextFormat("Asteroides: %d pontos", asteroid_score), 1); melody_play(ASTEROID_OVER); return; }
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
            shots[i].position.x += shots[i].speed.x;
            shots[i].position.y -= shots[i].speed.y;

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

int main(void) {
    if (MFRC522_Init('B') != 0) fputs("Erro ao iniciar RFID MFRC522.\n", stderr);
    pthread_mutex_init(&app.lock, NULL); app.state = WAIT_CARD;
    pthread_t thread; pthread_create(&thread, NULL, rfid_loop, NULL); pthread_detach(thread);
    InitWindow(W, H, "Arcade RFID"); SetTargetFPS(60); controls_init();
    initDisplay();
    while (!WindowShouldClose()) {
        controls_poll();
        pthread_mutex_lock(&app.lock); State state=app.state; char card[9]; strcpy(card,app.card); char public_code[4]; strcpy(public_code,app.code); int credits=app.credits; char note[96]; strcpy(note,app.message); int back=app.message_to_menu; pthread_mutex_unlock(&app.lock);
        switch(state){
            case WAIT_CARD:
                break;
            case MESSAGE:
                if (GetTime()-message_at>2.2) { pthread_mutex_lock(&app.lock); app.state=back?MENU:WAIT_CARD; pthread_mutex_unlock(&app.lock); } break;
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
            case MENU:
                if (was_pressed(UP) && selected > 0) selected--;
                if (was_pressed(DOWN) && selected < 5) selected++;
                if (was_pressed(LEFT)) { pthread_mutex_lock(&app.lock); app.state=WAIT_CARD; pthread_mutex_unlock(&app.lock); }
                if (was_pressed(RIGHT)) {
                    switch (selected){
                        case 0: start_game(SNAKE,card,credits); break;
                        case 1: start_game(ASTEROIDS,card,credits); break;
                        case 2: recharge_selected=0; pthread_mutex_lock(&app.lock); app.state=RECHARGE; pthread_mutex_unlock(&app.lock); break;
                        case 3: show_message(TextFormat("Saldo: %d credito(s)",credits),1); break;
                        case 4: pthread_mutex_lock(&app.lock); app.state=SCORES; pthread_mutex_unlock(&app.lock); break;
                        default: pthread_mutex_lock(&app.lock); app.state=WAIT_CARD; pthread_mutex_unlock(&app.lock); break;
                    }
                }
                break;
            case SCORES:
                if (was_pressed(LEFT) || was_pressed(RIGHT)) {
                    pthread_mutex_lock(&app.lock); app.state=MENU; pthread_mutex_unlock(&app.lock);
                }
                break;
            case RECHARGE:
                if (was_pressed(UP) && recharge_selected > 0) recharge_selected--;
                if (was_pressed(DOWN) && recharge_selected < 3) recharge_selected++;
                if (was_pressed(LEFT)) { pthread_mutex_lock(&app.lock); app.state=MENU; pthread_mutex_unlock(&app.lock); }
                if (was_pressed(RIGHT)) {
                    const int packs[] = {1, 5, 10};
                    if (recharge_selected < 3) recharge(card,credits,packs[recharge_selected]);
                    else { pthread_mutex_lock(&app.lock); app.state=MENU; pthread_mutex_unlock(&app.lock); }
                }
                break;
            default: break;
        }
        if (state==SNAKE) display_number(snake_score);
        else if (state==ASTEROIDS) display_number(asteroid_score);
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
            DrawText("ARCADE RFID",CX-105,18,28,RAYWHITE); DrawText(TextFormat("Codigo %s   |   Creditos: %d",public_code,credits),CX-150,58,20,GOLD);
            const char *items[] = {"COBRINHA  -  1 credito","ASTEROIDES  -  1 credito","RECARREGAR CREDITOS","CONSULTAR SALDO","PLACAR","ENCERRAR CARTAO"};
            for (int i=0;i<6;i++) { Color c=i==selected?(Color){70,110,220,255}:(Color){35,45,85,255}; DrawRectangleRounded((Rectangle){180,82+i*50,440,39},.2f,8,c); DrawText(items[i],230,91+i*50,18,WHITE); if(i==selected)DrawText(">",195,91+i*50,20,YELLOW); }
            DrawText("Joystick: cima/baixo seleciona | direita confirma | esquerda volta",65,425,16,LIGHTGRAY);
        } else if (state==RECHARGE) {
            const char *packs[] = {"+1 CREDITO","+5 CREDITOS","+10 CREDITOS","VOLTAR"};
            DrawText("RECARGA LIVRE", CX-120,75,30,GOLD);
            DrawText(TextFormat("Codigo %s   |   Saldo: %d",public_code,credits),CX-145,112,20,RAYWHITE);
            for (int i=0;i<4;i++) { Color c=i==recharge_selected?(Color){50,150,90,255}:(Color){35,65,55,255}; DrawRectangleRounded((Rectangle){205,155+i*55,390,42},.2f,8,c); DrawText(packs[i],270,165+i*55,19,WHITE); if(i==recharge_selected)DrawText(">",220,165+i*55,22,YELLOW); }
            DrawText("Selecione o pacote desejado",CX-120,395,17,LIGHTGRAY);
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
                char display_code[4] = "---"; card_code_read(snake_scores[i].card, display_code);
                DrawText(display_code, 135, 112+i*27, 18, RAYWHITE);
                DrawText(TextFormat("%d", snake_scores[i].score), 300, 112+i*27, 18, GREEN);
            }
            for (int i = 0; i < asteroid_count; i++) {
                DrawText(TextFormat("%d.", i+1), 430, 112+i*27, 18, YELLOW);
                char display_code[4] = "---"; card_code_read(asteroid_scores[i].card, display_code);
                DrawText(display_code, 470, 112+i*27, 18, RAYWHITE);
                DrawText(TextFormat("%d", asteroid_scores[i].score), 635, 112+i*27, 18, GREEN);
            }
            DrawText("Esquerda ou direita para voltar", CX-135, 430, 17, LIGHTGRAY);
        } else {
            if (state==SNAKE) draw_snake();
            else if (state==ASTEROIDS) draw_asteroids();
            else { DrawRectangleRounded((Rectangle){110,160,580,150},.15f,8,(Color){35,40,82,255}); DrawText(note,CX-MeasureText(note,27)/2,215,27,WHITE); }
        }
        EndDrawing();
    }
    digitalWrite(PIN_BUZZER, LOW);
    CloseWindow(); pthread_mutex_destroy(&app.lock); return 0;
}
