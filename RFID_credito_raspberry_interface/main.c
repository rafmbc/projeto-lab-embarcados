/*
 * Fluxo:
 *   1. Inicia scan continuo (pthread separada) — opcional se sensor ausente
 *   2. Cartao aproximado -> guarda CardID (autenticacao)
 *   3. Cria cartoes.csv se nao houver
 *   4. Procura ou cria linha correspondente ao CardID
 *   5. Interface raylib: Leitura / Recarga / Retirada -> atualiza tabela CSV
 *   6. Retorna ao scan (sub-processo)
 *
 * Controles:
 *   Mouse   : clique nos botoes
 *   Seta ↑↓ : navega entre opcoes do menu
 *   Enter   : confirma opcao selecionada
 *   Esc     : cancela / volta
 *   0-9     : digita valor na tela de input
 *   Backspace: apaga ultimo digito
 *
 * Dependencias:
 *   sudo apt update && sudo apt install libraylib-dev
 *   Compilar: sudo gcc src/*\/*.c *.c -I include -lwiringPi -lraylib -lm -lpthread -o RFID_interface
 *   Sem sensor: gcc *.c -I include -lraylib -lm -lpthread -DRFID_DUMMY -o RFID_interface
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include "raylib.h"

/* Compilar com -DRFID_DUMMY para rodar sem hardware RFID */
#ifndef RFID_DUMMY
#include "mfrc522.h"
#else
/* Stubs minimos para compilar sem hardware */
typedef int MFRC522_Status_t;
#define MI_OK        0
#define MI_NOTAGERR -1
#define PICC_REQIDL  0x26
static inline int     MFRC522_Init(char t)                        { (void)t; return -1; }
static inline int     MFRC522_Request(uint8_t m, uint8_t *t)     { (void)m;(void)t; return MI_NOTAGERR; }
static inline int     MFRC522_Anticoll(uint8_t *s)               { (void)s; return MI_NOTAGERR; }
static inline int     MFRC522_SelectTag(uint8_t *s)              { (void)s; return 0; }
static inline void    MFRC522_Halt(void)                          {}
#endif

/* ── CSV ─────────────────────────────────────────────────────────────── */
#define CSV_FILE     "cartoes.csv"
#define CSV_HEADER   "CardID,Credito\n"
#define CSV_LINE_MAX 64

static void card_id_str(uint8_t *id, char *out)
{
    snprintf(out, 9, "%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
}

static int csv_init(void)
{
    FILE *f = fopen(CSV_FILE, "r");
    if (f) { fclose(f); return 0; }
    f = fopen(CSV_FILE, "w");
    if (!f) return -1;
    fputs(CSV_HEADER, f);
    fclose(f);
    return 0;
}

static int csv_read_credit(const char *id_str)
{
    FILE *f = fopen(CSV_FILE, "r");
    if (!f) return 0;
    char line[CSV_LINE_MAX];
    fgets(line, sizeof(line), f); /* pula cabecalho */
    while (fgets(line, sizeof(line), f)) {
        char fid[9]; int val;
        if (sscanf(line, "%8[^,],%d", fid, &val) == 2 && strcmp(fid, id_str) == 0) {
            fclose(f); return val;
        }
    }
    fclose(f);
    return 0;
}

/* ponytail: reescreve CSV inteiro; teto de 128 cartoes. */
static int csv_write_credit(const char *id_str, int novo)
{
    FILE *f = fopen(CSV_FILE, "r");
    if (!f) return -1;
    char lines[128][CSV_LINE_MAX];
    int n = 0, found = 0;
    fgets(lines[n++], CSV_LINE_MAX, f);
    while (n < 128 && fgets(lines[n], CSV_LINE_MAX, f)) {
        char fid[9]; int val;
        if (sscanf(lines[n], "%8[^,],%d", fid, &val) == 2 && strcmp(fid, id_str) == 0) {
            snprintf(lines[n], CSV_LINE_MAX, "%s,%d\n", id_str, novo);
            found = 1;
        }
        n++;
    }
    fclose(f);
    if (!found && n < 128)
        snprintf(lines[n++], CSV_LINE_MAX, "%s,%d\n", id_str, novo);
    f = fopen(CSV_FILE, "w");
    if (!f) return -1;
    for (int i = 0; i < n; i++) fputs(lines[i], f);
    fclose(f);
    return 0;
}

/* ── Estado compartilhado ────────────────────────────────────────────── */
typedef enum { ST_AGUARDANDO, ST_MENU, ST_INPUT, ST_RESULTADO } State;

typedef struct {
    pthread_mutex_t mtx;
    State  state;
    char   id_str[9];
    int    credito;
    int    opcao;      /* 1=Leitura 2=Recarga 3=Retirada */
    char   msg[128];
    int    rfid_ok;    /* 1 se hardware presente */
} App;

static App g_app;

/* ── Thread RFID ─────────────────────────────────────────────────────── */
static void *rfid_thread(void *arg)
{
    (void)arg;
    uint8_t CardID[5]   = {0};
    uint8_t tagType[16] = {0};

    while (1) {
        pthread_mutex_lock(&g_app.mtx);
        State s = g_app.state;
        pthread_mutex_unlock(&g_app.mtx);

        if (s != ST_AGUARDANDO) { usleep(100000); continue; }

        if (MFRC522_Request(PICC_REQIDL, tagType) != MI_OK) {
            MFRC522_Halt(); usleep(50000); continue;
        }
        if (MFRC522_Anticoll(CardID) != MI_OK) { MFRC522_Halt(); continue; }
        if (MFRC522_SelectTag(CardID) == 0)    { MFRC522_Halt(); continue; }

        char id[9];
        card_id_str(CardID, id);
        csv_init();
        int cred = csv_read_credit(id);

        pthread_mutex_lock(&g_app.mtx);
        memcpy(g_app.id_str, id, 9);
        g_app.credito = cred;
        g_app.state   = ST_MENU;
        pthread_mutex_unlock(&g_app.mtx);

        MFRC522_Halt();
    }
    return NULL;
}

/* ── Layout ──────────────────────────────────────────────────────────── */
#define W   800
#define H   480
#define CX  (W/2)

/* Itens do menu principal */
#define MENU_N 4
static const char *MENU_LABELS[MENU_N]  = { "Leitura", "Recarga", "Retirada", "Cancelar" };
static const Color MENU_COLORS[MENU_N]  = {
    {40, 120, 200, 255},
    {40, 160,  80, 255},
    {180, 60,  60, 255},
    {70,  70,  70, 255}
};

/* Itens do input */
#define INPUT_N 2
static const char *INPUT_LABELS[INPUT_N] = { "Confirmar", "Cancelar" };

/* Indice de item selecionado por teclado */
static int g_sel_menu  = 0;
static int g_sel_input = 0;

/* ── Input numerico ──────────────────────────────────────────────────── */
static char g_ibuf[8] = {0};
static int  g_ilen    = 0;

static void input_reset(void) { g_ilen = 0; memset(g_ibuf, 0, sizeof(g_ibuf)); g_sel_input = 0; }

static void input_handle_keys(void)
{
    int k = GetCharPressed();
    while (k > 0) {
        if (k >= '0' && k <= '9' && g_ilen < 6) {
            g_ibuf[g_ilen++] = (char)k;
            g_ibuf[g_ilen]   = '\0';
        }
        k = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE) && g_ilen > 0)
        g_ibuf[--g_ilen] = '\0';
}

/* ── Botao com suporte a foco de teclado ─────────────────────────────── */
/*
 * focused = 1 -> borda de selecao por teclado
 * Retorna 1 se clicado por mouse OU Enter pressionado com foco.
 */
static int btn_ex(const char *label, int bx, int by, int bw, int bh,
                  Color bg, int focused)
{
    Rectangle r = { bx, by, bw, bh };
    Vector2 mouse = GetMousePosition();
    int hover = CheckCollisionPointRec(mouse, r);

    Color fill = hover ? Fade(bg, 0.72f) : bg;
    DrawRectangleRounded(r, 0.28f, 8, fill);

    /* Borda de foco (teclado): anel amarelo */
    if (focused)
        DrawRectangleRoundedLines(r, 0.28f, 8, YELLOW);
    else
        DrawRectangleRoundedLines(r, 0.28f, 8, (Color){255,255,255,60});

    int fs = (int)(bh * 0.42f);
    DrawText(label,
             bx + bw/2 - MeasureText(label, fs)/2,
             by + (bh - fs)/2, fs, WHITE);

    int clicked = hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
    int enter   = focused && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER));
    return clicked || enter;
}

/* Atalho: botao centralizado */
static int btn_c(const char *label, int y, int bw, int bh, Color bg, int focused)
{
    return btn_ex(label, CX - bw/2, y, bw, bh, bg, focused);
}

/* ── Legenda de teclas (canto inferior) ──────────────────────────────── */
static void draw_key_legend(State state)
{
    int y = H - 28;
    Color c = (Color){140, 140, 180, 200};
    int fs = 17;

    if (state == ST_AGUARDANDO) {
        const char *t = "Mouse ou teclado";
        DrawText(t, CX - MeasureText(t, fs)/2, y, fs, c);
        return;
    }
    if (state == ST_MENU) {
        DrawText("[ ↑ ↓ ] Navegar    [ Enter ] Confirmar    [ Esc ] Cancelar",
                 CX - MeasureText("[ ↑ ↓ ] Navegar    [ Enter ] Confirmar    [ Esc ] Cancelar", fs)/2,
                 y, fs, c);
        return;
    }
    if (state == ST_INPUT) {
        DrawText("[ 0-9 ] Digitar    [ ← Backspace ] Apagar    [ Tab / ↑↓ ] Trocar botao    [ Enter ] Confirmar    [ Esc ] Cancelar",
                 10, y, 14, c);
        return;
    }
}

/* ── Confirm helper ──────────────────────────────────────────────────── */
static void confirma_operacao(const char *id, int credito, int opcao,
                               double *resultado_t)
{
    char result_msg[128] = {0};
    const char *op_label = (opcao == 2) ? "Recarga" : "Retirada";

    if (opcao == 1) {
        snprintf(result_msg, sizeof(result_msg), "Saldo atual: R$ %d", credito);
    } else {
        int valor = (g_ilen > 0) ? atoi(g_ibuf) : 0;
        if (valor <= 0) {
            snprintf(result_msg, sizeof(result_msg), "Valor invalido.");
        } else if (opcao == 3 && valor > credito) {
            snprintf(result_msg, sizeof(result_msg),
                     "Saldo insuficiente.\nSaldo atual: R$ %d", credito);
        } else {
            int novo = (opcao == 2) ? credito + valor : credito - valor;
            if (csv_write_credit(id, novo) < 0) {
                snprintf(result_msg, sizeof(result_msg), "Erro ao salvar CSV.");
            } else {
                snprintf(result_msg, sizeof(result_msg),
                         "%s de R$ %d efetuada.\nNovo saldo: R$ %d",
                         op_label, valor, novo);
                pthread_mutex_lock(&g_app.mtx);
                g_app.credito = novo;
                pthread_mutex_unlock(&g_app.mtx);
            }
        }
    }

    pthread_mutex_lock(&g_app.mtx);
    memcpy(g_app.msg, result_msg, 128);
    g_app.state = ST_RESULTADO;
    pthread_mutex_unlock(&g_app.mtx);
    *resultado_t = GetTime();
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void)
{
    /* RFID: opcional — interface funciona sem hardware */
    int rfid_ok = (MFRC522_Init('B') == 0);

    pthread_mutex_init(&g_app.mtx, NULL);
    g_app.state   = ST_AGUARDANDO;
    g_app.rfid_ok = rfid_ok;

    /* Se sensor presente, inicia thread de scan */
    pthread_t tid;
    if (rfid_ok)
        pthread_create(&tid, NULL, rfid_thread, NULL);

    InitWindow(W, H, "Sistema de Credito Fliperama");
    SetTargetFPS(30);

    double resultado_t = 0.0;

    /* ── Modo demo (sem RFID): permite digitar ID manualmente ── */
    char  demo_id[9]  = "DEMO0001";
    int   demo_mode   = !rfid_ok; /* ativa automaticamente sem hardware */

    while (!WindowShouldClose()) {

        /* ── Leitura do estado (copia local para nao travar render) ── */
        pthread_mutex_lock(&g_app.mtx);
        State state   = g_app.state;
        char  id[9];   memcpy(id,  g_app.id_str, 9);
        int   credito = g_app.credito;
        int   opcao   = g_app.opcao;
        char  msg[128]; memcpy(msg, g_app.msg, 128);
        pthread_mutex_unlock(&g_app.mtx);

        /* ── Transicao: resultado expira ── */
        if (state == ST_RESULTADO && GetTime() - resultado_t > 2.5) {
            pthread_mutex_lock(&g_app.mtx);
            g_app.state = rfid_ok ? ST_AGUARDANDO : ST_MENU;
            pthread_mutex_unlock(&g_app.mtx);
            state = g_app.state;
        }

        /* ── Navegacao por seta global ── */
        if (state == ST_MENU) {
            if (IsKeyPressed(KEY_DOWN))  g_sel_menu = (g_sel_menu + 1) % MENU_N;
            if (IsKeyPressed(KEY_UP))    g_sel_menu = (g_sel_menu - 1 + MENU_N) % MENU_N;
            if (IsKeyPressed(KEY_ESCAPE)) {
                g_sel_menu = MENU_N - 1; /* foca Cancelar */
            }
        }
        if (state == ST_INPUT) {
            if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_TAB))
                g_sel_input = (g_sel_input + 1) % INPUT_N;
            if (IsKeyPressed(KEY_UP))
                g_sel_input = (g_sel_input - 1 + INPUT_N) % INPUT_N;
            if (IsKeyPressed(KEY_ESCAPE)) {
                pthread_mutex_lock(&g_app.mtx);
                g_app.state = ST_MENU;
                pthread_mutex_unlock(&g_app.mtx);
            }
        }

        /* ── Modo demo: simula cartao sem RFID ── */
        if (demo_mode && state == ST_AGUARDANDO) {
            /* Pressionar Enter na tela de aguardo entra com ID demo */
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                csv_init();
                int cred = csv_read_credit(demo_id);
                pthread_mutex_lock(&g_app.mtx);
                memcpy(g_app.id_str, demo_id, 9);
                g_app.credito = cred;
                g_app.state   = ST_MENU;
                pthread_mutex_unlock(&g_app.mtx);
                g_sel_menu = 0;
            }
        }

        /* ── Render ── */
        BeginDrawing();
        ClearBackground((Color){14, 14, 26, 255});

        /* Titulo */
        DrawText("Sistema de Credito Fliperama",
                 CX - MeasureText("Sistema de Credito Fliperama", 22)/2,
                 16, 22, (Color){180, 180, 255, 255});
        DrawLine(0, 50, W, 50, (Color){50, 50, 110, 255});

        /* Badge de status do sensor */
        {
            const char *badge = rfid_ok ? "RFID: Online" : "RFID: Sem sensor (modo demo)";
            Color bc = rfid_ok ? (Color){40,180,80,220} : (Color){200,140,40,220};
            int bw = MeasureText(badge, 15) + 16;
            DrawRectangleRounded((Rectangle){W - bw - 10, 12, bw, 24}, 0.4f, 6, bc);
            DrawText(badge, W - bw - 2, 18, 15, WHITE);
        }

        /* ────────────────────────── ST_AGUARDANDO ── */
        if (state == ST_AGUARDANDO) {
            double t  = GetTime();
            float  r1 = 60 + 8*(float)sin(t*2.0);
            float  r2 = 90 + 8*(float)sin(t*2.0+1.0);
            DrawCircleLines(CX, 205, r1, (Color){100,100,255,170});
            DrawCircleLines(CX, 205, r2, (Color){80, 80, 200,110});
            DrawCircle(CX, 205, 42, (Color){50,50,170,255});
            DrawText("RFID", CX - MeasureText("RFID",22)/2, 195, 22, WHITE);

            if (rfid_ok) {
                const char *s = "Aproxime o cartao";
                DrawText(s, CX - MeasureText(s,28)/2, 290, 28, (Color){200,200,255,255});
            } else {
                /* Modo demo: instrucao explicita */
                const char *s1 = "Sensor nao detectado — Modo Demo";
                const char *s2 = "Pressione  [ Enter ]  para simular leitura de cartao";
                DrawText(s1, CX - MeasureText(s1,22)/2, 278, 22, (Color){255,200,80,255});
                DrawText(s2, CX - MeasureText(s2,19)/2, 312, 19, (Color){180,180,220,255});

                /* Caixa Enter animada */
                float pulse = 0.6f + 0.4f*(float)sin(t*3.0);
                DrawRectangleRounded((Rectangle){CX-60,350,120,38}, 0.3f, 6,
                                     Fade((Color){80,80,200,255}, pulse));
                DrawText("Enter", CX - MeasureText("Enter",22)/2, 359, 22, WHITE);
            }

        /* ────────────────────────── ST_MENU ── */
        } else if (state == ST_MENU) {

            /* Info do cartao */
            char l1[32]; snprintf(l1, sizeof(l1), "Cartao: %s", id);
            DrawText(l1, CX - MeasureText(l1,21)/2, 62, 21, (Color){140,255,140,255});

            char l2[32]; snprintf(l2, sizeof(l2), "Saldo:  R$ %d", credito);
            DrawText(l2, CX - MeasureText(l2,36)/2, 90, 36, WHITE);

            DrawLine(CX-170,140, CX+170,140, (Color){50,50,110,255});

            /* Instrucao de navegacao inline */
            const char *nav = "↑ ↓  para navegar   |   Enter  para confirmar";
            DrawText(nav, CX - MeasureText(nav,16)/2, 148, 16, (Color){140,140,180,200});

            /* Botoes do menu */
            int btn_y[MENU_N] = {175, 244, 313, 400};
            int btn_h[MENU_N] = { 58,  58,  58,  46};
            for (int i = 0; i < MENU_N; i++) {
                /* Seta de seleção ao lado do botão focado */
                if (g_sel_menu == i) {
                    DrawText("►", CX - 160, btn_y[i] + btn_h[i]/2 - 10, 20, YELLOW);
                }
                if (btn_c(MENU_LABELS[i], btn_y[i], 260, btn_h[i],
                          MENU_COLORS[i], g_sel_menu == i)) {
                    if (i == 0) { /* Leitura */
                        g_app.opcao = 1;
                        confirma_operacao(id, credito, 1, &resultado_t);
                    } else if (i == 1) { /* Recarga */
                        input_reset();
                        pthread_mutex_lock(&g_app.mtx);
                        g_app.opcao = 2; g_app.state = ST_INPUT;
                        pthread_mutex_unlock(&g_app.mtx);
                    } else if (i == 2) { /* Retirada */
                        input_reset();
                        pthread_mutex_lock(&g_app.mtx);
                        g_app.opcao = 3; g_app.state = ST_INPUT;
                        pthread_mutex_unlock(&g_app.mtx);
                    } else { /* Cancelar */
                        pthread_mutex_lock(&g_app.mtx);
                        g_app.state = ST_AGUARDANDO;
                        pthread_mutex_unlock(&g_app.mtx);
                        g_sel_menu = 0;
                    }
                }
            }

        /* ────────────────────────── ST_INPUT ── */
        } else if (state == ST_INPUT) {

            const char *op_label = (opcao == 2) ? "Recarga" : "Retirada";
            Color op_cor = (opcao == 2) ? (Color){40,160,80,255} : (Color){180,60,60,255};

            char hdr[40]; snprintf(hdr, sizeof(hdr), "%s — Cartao: %s", op_label, id);
            DrawText(hdr, CX - MeasureText(hdr,20)/2, 65, 20, Fade(op_cor,1.0f));

            char sld[32]; snprintf(sld, sizeof(sld), "Saldo atual: R$ %d", credito);
            DrawText(sld, CX - MeasureText(sld,24)/2, 95, 24, WHITE);

            /* Campo de entrada */
            const char *prompt = "Digite o valor (R$):";
            DrawText(prompt, CX - MeasureText(prompt,21)/2, 158, 21,
                     (Color){200,200,200,255});

            /* Instrucao de teclas inline */
            const char *hint = "[ 0-9 ] digitar   [ ← ] apagar   [ ↑↓ / Tab ] trocar botao";
            DrawText(hint, CX - MeasureText(hint,15)/2, 183, 15, (Color){130,130,170,200});

            Rectangle campo = {CX-130, 206, 260, 56};
            DrawRectangleRounded(campo, 0.2f, 8, (Color){22,22,50,255});
            DrawRectangleRoundedLines(campo, 0.2f, 8, op_cor);

            const char *vt = g_ilen > 0 ? g_ibuf : "0";
            DrawText(vt, CX - MeasureText(vt,34)/2, 218, 34, WHITE);
            if ((int)(GetTime()*2) % 2 == 0)
                DrawText("|", CX + MeasureText(vt,34)/2 + 3, 218, 34, op_cor);

            input_handle_keys();

            /* Botoes Confirmar / Cancelar */
            int iy[INPUT_N] = {285, 355};
            Color ic[INPUT_N] = {op_cor, (Color){70,70,70,255}};
            for (int i = 0; i < INPUT_N; i++) {
                if (g_sel_input == i)
                    DrawText("►", CX - 140, iy[i] + 16, 20, YELLOW);
                if (btn_c(INPUT_LABELS[i], iy[i], 220, 52, ic[i], g_sel_input == i)) {
                    if (i == 0) { /* Confirmar */
                        confirma_operacao(id, credito, opcao, &resultado_t);
                    } else { /* Cancelar */
                        pthread_mutex_lock(&g_app.mtx);
                        g_app.state = ST_MENU;
                        pthread_mutex_unlock(&g_app.mtx);
                    }
                }
            }

        /* ────────────────────────── ST_RESULTADO ── */
        } else if (state == ST_RESULTADO) {

            DrawRectangle(CX-290,120, 580,210, (Color){22,22,55,235});
            DrawRectangleLines(CX-290,120, 580,210, (Color){90,90,200,255});

            int y = 155;
            char tmp[128]; memcpy(tmp, msg, 128);
            char *ln = strtok(tmp, "\n");
            while (ln) {
                DrawText(ln, CX - MeasureText(ln,26)/2, y, 26, WHITE);
                y += 36; ln = strtok(NULL, "\n");
            }

            /* Barra de progresso */
            double el = GetTime() - resultado_t;
            float  pr = 1.0f - (float)(el/2.5);
            DrawRectangle(CX-140, 316, (int)(280*pr), 8, (Color){100,100,255,200});
        }

        /* Legenda de teclas (rodape) */
        draw_key_legend(state);

        EndDrawing();
    }

    CloseWindow();
    if (rfid_ok) { pthread_cancel(tid); pthread_join(tid, NULL); }
    pthread_mutex_destroy(&g_app.mtx);
    return 0;
}
