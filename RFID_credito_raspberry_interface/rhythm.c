/* Ritmo - incluido por arcade.c (#include, como o display.c); nao compilar sozinho.
 * Usa do arcade.c: W/H/CX, controles (button_was_pressed...), melody_play, game_over, draw_header. */

/* Cores dos botoes da placa, usadas no Ritmo: esquerda amarelo, cima azul, baixo vermelho, direita verde. */
static const Color LANE_COLORS[4] = {{253,249,0,255}, {0,121,241,255}, {230,41,55,255}, {0,228,48,255}};
static const Note RHYTHM_START[]   = {{659,90},{622,90},{659,90},{622,90},{659,200},{0,0}};

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

/* "Foto" estatica para o carrossel do menu. */
static void preview_rhythm(Rectangle r) {
    static const int tl[][2] = {{3,10},{2,60},{3,110},{1,150},{0,190}}; /* faixa, y */
    int lw = r.width/4;
    for (int l = 0; l < 4; l++) DrawRectangle(r.x + l*lw, r.y, lw, r.height, l % 2 ? (Color){20,24,48,255} : (Color){16,19,40,255});
    for (int i = 0; i < 5; i++) DrawRectangleRounded((Rectangle){r.x + tl[i][0]*lw + 6, r.y + tl[i][1], lw - 12, 34}, .25f, 8, LANE_COLORS[tl[i][0]]);
    DrawLine(r.x, r.y + 214, r.x + r.width, r.y + 214, RAYWHITE);
    for (int l = 0; l < 4; l++) DrawRectangleRounded((Rectangle){r.x + l*lw + 10, r.y + 222, lw - 20, 26}, .4f, 8, LANE_COLORS[l]);
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
