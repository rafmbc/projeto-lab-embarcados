/* Cobrinha - incluido por arcade.c (#include, como o display.c); nao compilar sozinho.
 * Usa do arcade.c: W/H/CX, controles (button_was_pressed...), melody_play, game_over, draw_header. */

/* Cobrinha */
#define COLS 18
#define ROWS 11
#define CELL 28
#define BOARD_X ((W - COLS*CELL)/2)
#define BOARD_Y 120
static Cell snake[COLS*ROWS], food;
static int snake_len, dx, dy, next_dx, next_dy, snake_score;
static double snake_at;

static const Note SNAKE_START[]    = {{523,90},{659,90},{784,90},{1047,200},{0,0}};
static const Note SNAKE_OVER[]     = {{392,160},{349,160},{330,160},{262,160},{196,200},{0,0}};
static const Note SNAKE_EAT[]      = {{1047,60},{0,0}};

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


/* "Foto" estatica para o carrossel do menu. */
static void preview_snake(Rectangle r) {
    const int c = 18, ox = r.x + 3, oy = r.y + 2;
    static const Cell body[] = {{14,5},{15,5},{15,6},{15,7},{15,8},{15,9},{16,9},{17,9},{18,9},{19,9},{19,8},{19,7}};
    DrawRectangleRec(r, (Color){10,31,24,255});
    for (int i = 0; i < 12; i++) DrawRectangle(ox + body[i].x*c + 2, oy + body[i].y*c + 2, c-4, c-4, i ? (Color){50,180,100,255} : LIME);
    DrawCircle(ox + 12*c + c/2, oy + 4*c + c/2, 6, RED);
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
