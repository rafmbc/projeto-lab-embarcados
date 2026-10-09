/* Defesa - incluido por arcade.c (#include, como o display.c); nao compilar sozinho.
 * Usa do arcade.c: W/H/CX, controles, ads7830_read, melody_play, alarm_beep, game_over, draw_header;
 * de defuse.c: POT_INVERT e df_point; de asteroids.c: SHOT_FIRE e ROCK_BOOM. */

static const Note DEFENSE_START[] = {{523,100},{523,100},{784,100},{1047,250},{0,0}};
static const Note DEFENSE_OVER[]  = {{659,150},{523,150},{392,150},{262,300},{0,0}};

/* DEFESA: RP1 gira o canhao, botao azul (cima) dispara. Asteroides entram por cima, cada um
 * com tamanho, direcao e velocidade proprios; nenhum pode tocar o chao. Grandes viram 2. */
#define DEF_POT_CH 2          /* RP1 (A2) no ADS7830 */
#define DEF_AIM_MIN 200       /* angulo do canhao, graus (270 = reto pra cima) */
#define DEF_AIM_MAX 340
#define DEF_GROUND (H - 14)
#define DEF_MISSILES 2        /* misseis ao mesmo tempo */
#define DEF_RELOAD 1.2        /* recarga de cada missil, s */
#define DEF_MISSILE_SPEED 520 /* px/s */
#define DEF_BIG 24            /* raio a partir do qual o asteroide se divide */
#define DEF_ROCKS 24
#define DEF_BOOMS 12
#define DEF_LIVES 3
typedef struct { Vector2 p, v; float r; int on; } DefRock;
typedef struct { Vector2 p, v; int on; double ready_at; } DefMissile;
typedef struct { Vector2 p; float r; double t; int on; } DefBoom;
static DefRock def_rocks[DEF_ROCKS];
static DefMissile def_missiles[DEF_MISSILES];
static DefBoom def_booms[DEF_BOOMS];
static float def_pot;
static int def_lives, defense_score;
static double def_start_at, def_spawn_at;

static float def_angle(void) { return DEF_AIM_MIN + def_pot*(DEF_AIM_MAX - DEF_AIM_MIN); }

static void def_read_pot(void) {
    int v = ads7830_read(DEF_POT_CH);
    if (v >= 0) def_pot = def_pot*0.6f + (POT_INVERT ? 1 - v/255.0f : v/255.0f)*0.4f; /* filtra o ruido */
}

static void def_add_rock(Vector2 p, Vector2 v, float r) {
    for (int i = 0; i < DEF_ROCKS; i++) if (!def_rocks[i].on) { def_rocks[i] = (DefRock){p, v, r, 1}; return; }
}

/* Nasce acima da tela e vai para um ponto aleatorio do chao. */
static void def_spawn(void) {
    float r = GetRandomValue(0, 99) < 35 ? GetRandomValue(26, 34) : GetRandomValue(10, 18);
    Vector2 p = {GetRandomValue(-60, W + 60), -r - GetRandomValue(5, 60)};
    float dx = GetRandomValue(40, W - 40) - p.x, dy = DEF_GROUND - p.y, len = sqrtf(dx*dx + dy*dy), sp = GetRandomValue(45, 110);
    def_add_rock(p, (Vector2){dx/len*sp, dy/len*sp}, r);
}

static void def_boom(Vector2 p, float r) {
    for (int i = 0; i < DEF_BOOMS; i++) if (!def_booms[i].on) { def_booms[i] = (DefBoom){p, r, GetTime(), 1}; return; }
}

static void start_defense(void) {
    memset(def_rocks, 0, sizeof def_rocks); memset(def_missiles, 0, sizeof def_missiles); memset(def_booms, 0, sizeof def_booms);
    def_lives = DEF_LIVES; defense_score = 0; def_start_at = GetTime(); def_spawn_at = GetTime() + 1;
    def_pot = .5f; for (int k = 0; k < 15; k++) def_read_pot(); /* assenta o filtro na posicao real */
}

static void def_draw_cannon(Vector2 c, float a) {
    DrawLineEx(c, df_point(c, 46, a), 24, BLUE);
    DrawCircleV(c, 22, (Color){35,45,85,255});
    DrawCircleLines(c.x, c.y, 22, BLUE);
}

/* Mira: tracejado vermelho do bico do canhao ate sair por cima. */
static void def_draw_aim(Vector2 c, float a, float top) {
    for (float d = 52; d < 1000; d += 22) {
        Vector2 p = df_point(c, d, a);
        if (p.y < top) break;
        DrawLineEx(p, df_point(c, d + 11, a), 2, RED);
    }
}

/* Explosao: estrela laranja que cresce e some; k vai de 0 a 1. */
static void def_draw_burst(Vector2 p, float r, float k) {
    float R = r*(1 + k), rot = k*40;
    Vector2 prev = df_point(p, R, rot);
    for (int i = 1; i <= 12; i++) {
        Vector2 q = df_point(p, i % 2 ? R*0.55f : R, rot + i*30);
        DrawLineEx(prev, q, 3, Fade(ORANGE, 1 - k)); prev = q;
    }
    DrawCircleV(p, R*0.4f*(1 - k), Fade(YELLOW, 1 - k));
}

static void def_draw_rock(Vector2 p, float r) {
    DrawCircleV(p, r, GRAY);
    DrawCircleV((Vector2){p.x - r*.3f, p.y - r*.2f}, r*.3f, DARKGRAY);
}

/* "Foto" estatica para o carrossel do menu. */
static void preview_defense(Rectangle r) {
    Vector2 c = {r.x + r.width/2, r.y + r.height - 4};
    DrawRectangleRec(r, (Color){12,14,29,255});
    BeginScissorMode(r.x, r.y, r.width, r.height);
    def_draw_aim(c, 290, r.y);
    def_draw_rock((Vector2){r.x + 110, r.y + 70}, 24);
    def_draw_rock((Vector2){r.x + 370, r.y + 125}, 12);
    def_draw_rock((Vector2){r.x + 60, r.y + 175}, 9);
    def_draw_burst((Vector2){r.x + 320, r.y + 45}, 18, .3f);
    DrawCircleV(df_point(c, 140, 290), 5, RED);
    def_draw_cannon(c, 290);
    EndScissorMode();
}

static void draw_defense(void) {
    float dt = GetFrameTime(); double now = GetTime();
    def_read_pot();
    float a = def_angle(); Vector2 c = {CX, DEF_GROUND};

    if (button_was_pressed(UP)) /* dispara o primeiro missil livre e recarregado */
        for (int i = 0; i < DEF_MISSILES; i++) {
            DefMissile *m = &def_missiles[i];
            if (m->on || now < m->ready_at) continue;
            *m = (DefMissile){df_point(c, 50, a), df_point((Vector2){0, 0}, DEF_MISSILE_SPEED, a), 1, now + DEF_RELOAD};
            melody_play(SHOT_FIRE); break;
        }

    /* Dificuldade: 2 asteroides no inicio, +1 a cada 15 s, ate 10. */
    int alive = 0, want = 2 + (int)((now - def_start_at)/15);
    if (want > 10) want = 10;
    for (int i = 0; i < DEF_ROCKS; i++) alive += def_rocks[i].on;
    if (alive < want && now >= def_spawn_at) { def_spawn(); def_spawn_at = now + 0.7; }

    for (int i = 0; i < DEF_MISSILES; i++) {
        DefMissile *m = &def_missiles[i];
        if (!m->on) continue;
        m->p.x += m->v.x*dt; m->p.y += m->v.y*dt;
        if (m->p.x < -10 || m->p.x > W + 10 || m->p.y < -10) { m->on = 0; continue; }
        for (int k = 0; k < DEF_ROCKS; k++) {
            if (!def_rocks[k].on || !CheckCollisionCircles(m->p, 5, def_rocks[k].p, def_rocks[k].r)) continue;
            DefRock o = def_rocks[k]; /* copia: os filhos podem reusar este slot */
            m->on = 0; def_rocks[k].on = 0; def_boom(o.p, o.r); melody_play(ROCK_BOOM);
            defense_score += o.r >= DEF_BIG ? 5 : 10;
            if (o.r >= DEF_BIG) /* grande: divide em 2, abrindo 25 graus para cada lado */
                for (int s = -1; s <= 1; s += 2) {
                    float t = s*25*DEG2RAD, cs = cosf(t), sn = sinf(t);
                    def_add_rock(o.p, (Vector2){(o.v.x*cs - o.v.y*sn)*1.15f, (o.v.x*sn + o.v.y*cs)*1.15f}, o.r*0.55f);
                }
            break;
        }
    }

    for (int i = 0; i < DEF_ROCKS; i++) {
        DefRock *o = &def_rocks[i];
        if (!o->on) continue;
        o->p.x += o->v.x*dt; o->p.y += o->v.y*dt;
        if (o->p.x < -120 || o->p.x > W + 120) { o->on = 0; continue; } /* saiu pelos lados: sem penalidade */
        if (o->p.y + o->r*0.5f >= DEF_GROUND) { o->on = 0; def_lives--; def_boom(o->p, o->r); alarm_beep(150); }
    }
    if (def_lives <= 0) { game_over("Defesa", defense_score, DEFENSE_OVER); return; }

    def_draw_aim(c, a, 56);
    for (int i = 0; i < DEF_ROCKS; i++) if (def_rocks[i].on) def_draw_rock(def_rocks[i].p, def_rocks[i].r);
    for (int i = 0; i < DEF_MISSILES; i++) {
        DefMissile *m = &def_missiles[i];
        if (!m->on) continue;
        DrawLineEx(m->p, (Vector2){m->p.x - m->v.x*.03f, m->p.y - m->v.y*.03f}, 3, ORANGE);
        DrawCircleV(m->p, 5, RED);
    }
    for (int i = 0; i < DEF_BOOMS; i++) {
        float k = (float)(now - def_booms[i].t)/0.45f;
        if (!def_booms[i].on) continue;
        if (k >= 1) def_booms[i].on = 0; else def_draw_burst(def_booms[i].p, def_booms[i].r, k);
    }
    DrawLine(0, DEF_GROUND, W, DEF_GROUND, (Color){90,90,155,255});
    def_draw_cannon(c, a);

    draw_header("DEFESA", defense_score);
    for (int i = 0; i < DEF_MISSILES; i++) { /* misseis no topo: vermelho = pronto, cinza enchendo = recarregando */
        DefMissile *m = &def_missiles[i];
        float k = 1 - (float)(m->ready_at - now)/DEF_RELOAD;
        if (k > 1) k = 1;
        DrawRectangle(170 + i*22, 16, 12, 30, (Color){35,45,85,255});
        DrawRectangle(170 + i*22, 16 + 30*(1 - k), 12, 30*k, k >= 1 && !m->on ? RED : GRAY);
    }
    DrawText(TextFormat("Vidas: %d", def_lives), 400, 24, 20, def_lives == 1 ? RED : LIGHTGRAY);
}
