/* Defuse - incluido por arcade.c (#include, como o display.c); nao compilar sozinho.
 * Usa do arcade.c: W/H/CX, controles (button_was_pressed...), melody_play, game_over, draw_header. */

static const Note DEFUSE_START[]   = {{880,60},{880,60},{1319,150},{0,0}};
static const Note DF_IN[]          = {{880,50},{0,0}};   /* ponteiro entrou na faixa */
static const Note DF_LOCK[]        = {{1319,120},{0,0}}; /* pot travado (desarmado) */

/* DEFUSE: cada potenciometro move o ponteiro azul de uma barra em arco. Deixe cada ponteiro
 * PARADO na faixa vermelha por DF_HOLD s (trava em verde) antes do tempo acabar.
 * A cada rodada a faixa muda de lugar e encolhe; o tempo tambem encolhe, mais devagar. */
static const int DF_CH[3] = {4, 3, 2}; /* ADS7830: barra de cima = RP3 (A4), meio = RP2 (A3), baixo = RP1 (A2) */
static const char *DF_LABEL[3] = {"RP3", "RP2", "RP1"};
#define POT_INVERT 0      /* 1 se girar o pot p/ direita levar o ponteiro p/ esquerda */
#define DF_R 800          /* raio externo do arco, px */
#define DF_THICK 50
#define DF_SPAN 26        /* meia abertura do arco, graus */
#define DF_TOP 84         /* topo da 1a barra; as outras a cada 125 px */
#define DF_W0 0.22f       /* largura inicial da faixa (fracao do curso do pot) */
#define DF_W_MIN 0.035f
#define DF_T0 15.0f       /* tempo inicial da rodada, s */
#define DF_T_MIN 4.0f
#define DF_HOLD 0.8f      /* tempo parado na faixa p/ travar */
#define DF_ALARM 3.0f     /* ultimos segundos: buzzer ativo apita cada vez mais rapido */
static float df_pot[3], df_target[3], df_hold[3], df_width, df_time;
static int df_locked[3], df_inside[3], df_round, defuse_score;
static double df_deadline, df_beep_at;

static float df_angle(float span, float p) { return 270 - span + p*2*span; }
static Vector2 df_point(Vector2 c, float r, float a) { return (Vector2){c.x + cosf(a*DEG2RAD)*r, c.y + sinf(a*DEG2RAD)*r}; }

static void draw_defuse_bar(Vector2 c, float r, float th, float span, float p, float t, float w, Color zone, float hold) {
    float a0 = df_angle(span, t - w/2), a1 = df_angle(span, t + w/2), ap = df_angle(span, p);
    DrawRing(c, r - th, r, 270 - span, 270 + span, 64, (Color){35,45,85,255});
    DrawRingLines(c, r - th, r, 270 - span, 270 + span, 64, (Color){65,80,125,255});
    DrawRing(c, r - th, r, a0, a1, 8, Fade(zone, .45f));
    DrawLineEx(df_point(c, r - th, a0), df_point(c, r, a0), 3, zone);
    DrawLineEx(df_point(c, r - th, a1), df_point(c, r, a1), 3, zone);
    if (hold > 0) DrawRing(c, r + 3, r + 8, a0, a0 + (a1 - a0)*hold, 8, GREEN);
    DrawLineEx(df_point(c, r - th - 14, ap), df_point(c, r + 10, ap), 5, BLUE);
    DrawCircleV(df_point(c, r - th - 14, ap), 7, BLUE);
}

static void defuse_read_pots(void) {
    for (int i = 0; i < 3; i++) {
        int v = ads7830_read(DF_CH[i]);
        if (v < 0) continue;
        float p = POT_INVERT ? 1 - v/255.0f : v/255.0f;
        df_pot[i] = df_pot[i]*0.6f + p*0.4f; /* filtra o ruido do ADC */
    }
}

static void defuse_round(void) {
    df_width = fmaxf(DF_W_MIN, DF_W0 * powf(0.85f, df_round));
    df_time = fmaxf(DF_T_MIN, DF_T0 * powf(0.93f, df_round)); /* tempo encolhe mais devagar que a faixa */
    for (int i = 0; i < 3; i++) {
        int tries = 0;
        do df_target[i] = df_width/2 + GetRandomValue(0, 1000)/1000.0f * (1 - df_width);
        while (fabsf(df_target[i] - df_pot[i]) < df_width && ++tries < 20); /* nao nasce embaixo do ponteiro */
        df_locked[i] = df_inside[i] = 0; df_hold[i] = 0;
    }
    df_deadline = GetTime() + df_time; df_beep_at = 0;
}

static void start_defuse(void) {
    df_round = 0; defuse_score = 0;
    for (int i = 0; i < 3; i++) df_pot[i] = .5f;
    for (int k = 0; k < 15; k++) defuse_read_pots(); /* assenta o filtro na posicao real */
    defuse_round();
}

/* "Foto" estatica para o carrossel do menu. */
static void preview_defuse(Rectangle r) {
    static const float pv[3][2] = {{.30f,.75f},{.50f,.20f},{.62f,.62f}}; /* ponteiro, faixa */
    DrawRectangleRec(r, (Color){12,14,29,255});
    for (int i = 0; i < 3; i++)
        draw_defuse_bar((Vector2){r.x + r.width/2, r.y + 24 + i*76 + 500}, 500, 26, 22, pv[i][0], pv[i][1], .14f, i == 2 ? GREEN : RED, i == 2);
}

static void draw_defuse(void) {
    float dt = GetFrameTime(), left = (float)(df_deadline - GetTime());
    defuse_read_pots();
    int done = 0;
    for (int i = 0; i < 3; i++) {
        if (df_locked[i]) { done++; continue; }
        int in = fabsf(df_pot[i] - df_target[i]) <= df_width/2;
        if (in && !df_inside[i]) melody_play(DF_IN);
        df_inside[i] = in;
        df_hold[i] = in ? df_hold[i] + dt : 0; /* saiu da faixa: zera, precisa ficar parado */
        if (df_hold[i] >= DF_HOLD) { df_locked[i] = 1; done++; melody_play(DF_LOCK); }
    }
    if (done == 3) { /* bomba desarmada: pontua e comeca a proxima rodada */
        defuse_score += 100*(df_round+1) + (int)(left*10);
        df_round++; defuse_round(); left = df_time;
    }
    if (left <= 0) { alarm_beep(400); game_over("Defuse", defuse_score, ASTEROID_OVER); return; }
    if (left < DF_ALARM && GetTime() >= df_beep_at) { alarm_beep(40); df_beep_at = GetTime() + 0.1 + 0.5*left/DF_ALARM; }

    for (int i = 0; i < 3; i++) {
        draw_defuse_bar((Vector2){CX, DF_TOP + i*125 + DF_R}, DF_R, DF_THICK, DF_SPAN, df_pot[i], df_target[i], df_width,
                        df_locked[i] ? GREEN : RED, df_locked[i] ? 1 : df_hold[i]/DF_HOLD);
        DrawText(DF_LABEL[i], 8, DF_TOP + i*125 + 92, 18, LIGHTGRAY);
    }
    draw_header("DEFUSE", defuse_score);
    for (int k = 0; k < 3; k++) DrawRectangle(150 + k*15, 16, 12, 30, (Color){170,25,40,255}); /* dinamite */
    DrawRectangle(146, 27, 52, 7, (Color){60,45,10,255});
    float fuse = 198 + 180*left/df_time; /* pavio queima junto com o tempo */
    DrawLineEx((Vector2){198, 22}, (Vector2){fuse, 22}, 3, (Color){200,170,120,255});
    DrawCircle(fuse, 22, (int)(GetTime()*10) % 2 ? 5 : 3, left < DF_ALARM ? RED : ORANGE);
    DrawText(TextFormat("Rodada %d   %.1f s", df_round+1, left), 400, 24, 20, left < DF_ALARM ? RED : LIGHTGRAY);
}
