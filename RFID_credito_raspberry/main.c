/*
 * Fluxo:
 *   1. Inicia scan continuo
 *   2. Cartao aproximado -> guarda CardID (autenticacao)
 *   3. Cria cartoes.csv se nao houver
 *   4. Procura ou cria linha correspondente ao CardID
 *   5. Menu: Leitura / Recarga / Retirada -> atualiza tabela CSV
 *   6. Retorna ao scan (sub-processo)
 *
 * Guardar as informacoes de credito dentro do raspberryPI via tabela CSV.
 * Salva informacoes de credito via uma tabela csv.
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "mfrc522.h"

/* Arquivo CSV onde os creditos ficam armazenados no Raspberry Pi */
#define CSV_FILE "cartoes.csv"

/* Cabecalho do CSV */
#define CSV_HEADER "CardID,Credito\n"

/* Tamanho maximo de uma linha do CSV */
#define CSV_LINE_MAX 64

/*
 * Formata o CardID como string hexadecimal (8 chars + '\0').
 * Utiliza o CardID como autenticacao/chave na tabela.
 */
static void card_id_str(uint8_t *CardID, char *out)
{
    snprintf(out, 9, "%02X%02X%02X%02X",
             CardID[0], CardID[1], CardID[2], CardID[3]);
}

/*
 * Cria cartoes.csv se nao houver.
 * Retorna 0 em sucesso, -1 em erro.
 */
static int csv_init(void)
{
    FILE *f = fopen(CSV_FILE, "r");
    if (f) { fclose(f); return 0; } /* ja existe */

    f = fopen(CSV_FILE, "w");
    if (!f) { perror("fopen cartoes.csv"); return -1; }
    fputs(CSV_HEADER, f);
    fclose(f);
    printf("Arquivo %s criado.\n", CSV_FILE);
    return 0;
}

/*
 * Procura o credito do CardID no CSV.
 * Retorna o valor encontrado, ou 0 se o cartao for novo (linha sera criada no write).
 * ponytail: CSV simples de 2 colunas; para multiplos campos, usar struct + realloc.
 */
static int csv_read_credit(const char *id_str)
{
    FILE *f = fopen(CSV_FILE, "r");
    if (!f) return 0;

    char line[CSV_LINE_MAX];
    /* pula cabecalho */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }

    while (fgets(line, sizeof(line), f)) {
        char fid[9];
        int  val;
        if (sscanf(line, "%8[^,],%d", fid, &val) == 2) {
            if (strcmp(fid, id_str) == 0) {
                fclose(f);
                return val;
            }
        }
    }
    fclose(f);
    return 0; /* cartao novo, credito inicial = 0 */
}

/*
 * Atualiza (ou insere) o credito do CardID na tabela CSV.
 * Reescreve o arquivo inteiro — adequado para poucos cartoes.
 * Retorna 0 em sucesso, -1 em erro.
 */
static int csv_write_credit(const char *id_str, int novo_credito)
{
    /* Le todas as linhas existentes */
    FILE *f = fopen(CSV_FILE, "r");
    if (!f) return -1;

    /* Aloca buffer temporario para o conteudo atual */
    char  lines[128][CSV_LINE_MAX];
    int   n = 0;
    int   found = 0;

    /* pula cabecalho */
    fgets(lines[n], CSV_LINE_MAX, f);
    n++; /* lines[0] = cabecalho */

    while (n < 128 && fgets(lines[n], CSV_LINE_MAX, f)) {
        char fid[9]; int val;
        if (sscanf(lines[n], "%8[^,],%d", fid, &val) == 2
                && strcmp(fid, id_str) == 0) {
            /* Atualiza informacoes na tabela */
            snprintf(lines[n], CSV_LINE_MAX, "%s,%d\n", id_str, novo_credito);
            found = 1;
        }
        n++;
    }
    fclose(f);

    if (!found) {
        /* Procura ou cria coluna correspondente ao CardID */
        if (n < 128)
            snprintf(lines[n++], CSV_LINE_MAX, "%s,%d\n", id_str, novo_credito);
    }

    /* Reescreve o arquivo */
    f = fopen(CSV_FILE, "w");
    if (!f) { perror("fopen cartoes.csv"); return -1; }
    for (int i = 0; i < n; i++)
        fputs(lines[i], f);
    fclose(f);
    return 0;
}

/*
 * Seleciona o cartao detectado. Retorna 0 ou -1.
 */
static int seleciona_cartao(uint8_t *CardID)
{
    int tipo = MFRC522_SelectTag(CardID);
    if (tipo == 0) {
        printf("Falha ao selecionar cartao.\r\n");
        return -1;
    }
    printf("Cartao UID: %02X %02X %02X %02X | Tipo: %s\r\n",
           CardID[0], CardID[1], CardID[2], CardID[3],
           MFRC522_TypeToString(MFRC522_ParseType(tipo)));
    return 0;
}

/*
 * Sub-processo: executado apos cartao detectado.
 * Utiliza o CardID como autenticacao.
 * Cria cartoes.csv se nao houver, procura/cria linha do CardID,
 * exibe menu e atualiza a tabela.
 */
static void processa_cartao(uint8_t *CardID)
{
    char id_str[9];
    card_id_str(CardID, id_str);

    /* Cria arquivo cartoes.csv se nao houver */
    if (csv_init() < 0) return;

    /* Procura ou cria coluna correspondente ao CardID (credito inicial = 0) */
    int credito = csv_read_credit(id_str);

    printf("\n--- Cartao [%s] | Saldo: R$ %d ---\n", id_str, credito);
    printf("[1] Leitura   [2] Recarga   [3] Retirada\n> ");

    int opcao = 0;
    if (scanf("%d", &opcao) != 1) return;

    if (opcao == 1) {
        /* Leitura: mostra o saldo atual */
        printf("Saldo atual: R$ %d\n", credito);

    } else if (opcao == 2) {
        /* Recarga */
        printf("Valor de recarga (R$): ");
        int recarga = 0;
        if (scanf("%d", &recarga) != 1 || recarga <= 0) {
            printf("Valor invalido.\r\n");
            return;
        }
        int novo = credito + recarga;
        /* Atualiza informacoes na tabela */
        if (csv_write_credit(id_str, novo) < 0) {
            printf("Erro ao salvar no CSV.\r\n");
            return;
        }
        printf("Recarga de R$ %d efetuada. Novo saldo: R$ %d\n", recarga, novo);

    } else if (opcao == 3) {
        /* Retirada */
        printf("Valor de retirada (R$): ");
        int retirada = 0;
        if (scanf("%d", &retirada) != 1 || retirada <= 0) {
            printf("Valor invalido.\r\n");
            return;
        }
        if (retirada > credito) {
            printf("Saldo insuficiente. Saldo atual: R$ %d\n", credito);
            return;
        }
        int novo = credito - retirada;
        /* Atualiza informacoes na tabela */
        if (csv_write_credit(id_str, novo) < 0) {
            printf("Erro ao salvar no CSV.\r\n");
            return;
        }
        printf("Retirada de R$ %d efetuada. Novo saldo: R$ %d\n", retirada, novo);

    } else {
        printf("Opcao invalida.\r\n");
    }
}

int main(void)
{
    uint8_t CardID[5]   = {0};
    uint8_t tagType[16] = {0};

    if (MFRC522_Init('B') < 0) {
        printf("Falha ao inicializar MFRC522. Encerrando.\r\n");
        return -1;
    }

    printf("=== Sistema de Credito RFID (Raspberry Pi) ===\r\n");
    printf("Aproxime o cartao...\r\n\n");

    /* Inicia scan (loop principal / sub-processo) */
    while (1) {

        /* Cartao aproximado? */
        if (MFRC522_Request(PICC_REQIDL, tagType) != MI_OK) {
            MFRC522_Halt();
            continue; /* Nao: volta ao scan */
        }

        /* Sim: guarda a informacao do CardID atual */
        if (MFRC522_Anticoll(CardID) != MI_OK) {
            printf("Falha ao obter ID do cartao.\r\n");
            MFRC522_Halt();
            continue;
        }

        if (seleciona_cartao(CardID) < 0) {
            MFRC522_Halt();
            continue;
        }

        /* Sub-processo: CSV + menu credito */
        processa_cartao(CardID);

        MFRC522_Halt();

        /* Guardar as informacoes de credito dentro do raspberryPI: feito via CSV acima.
         * Retorna ao inicio do scan. */
        printf("\nAproximie o cartao para nova operacao...\r\n\n");
    }

    return 0;
}
