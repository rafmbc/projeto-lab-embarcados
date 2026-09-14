/*
 * Fluxo:
 *   1. Inicia scan continuo
 *   2. Cartao aproximado -> le bloco 1 (credito atual)
 *   3. Usuario escolhe: Leitura (mostra saldo) ou Recarga (insere valor, salva no bloco 1)
 *   4. Retorna ao scan
 * 
 * sudo apt-get update
git clone https://github.com/WiringPi/WiringPi
cd WiringPi
./build
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "mfrc522.h"

/* Bloco onde o credito e armazenado no cartao Mifare */
#define CREDIT_BLOCK 1

/* Tamanho de um bloco Mifare */
#define BLOCK_SIZE   16

/* Le o credito (reais inteiros) do bloco CREDIT_BLOCK. Retorna -1 em erro. */
static int read_credit(uint8_t *CardID)
{
    uint8_t buf[BLOCK_SIZE] = {0};

    if (MFRC522_Read(CREDIT_BLOCK, buf) != MI_OK)
        return -1;

    buf[BLOCK_SIZE - 1] = '\0';
    return atoi((char *)buf);
}

/* Salva o credito (reais inteiros) no bloco CREDIT_BLOCK. Retorna 0 ou -1. */
static int write_credit(uint8_t *CardID, int valor)
{
    uint8_t buf[BLOCK_SIZE] = {0};
    snprintf((char *)buf, BLOCK_SIZE, "%d", valor);

    if (MFRC522_Write(CREDIT_BLOCK, buf) != MI_OK)
        return -1;

    return 0;
}

/*
 * Seleciona o cartao detectado.
 * Retorna 0 em sucesso, -1 em erro.
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
 * Sub-processo: executado apos cartao detectado e selecionado.
 * 1. read <blockstart> 1  -> le credito do bloco 1
 * 2. Menu Leitura / Recarga
 * 3. Se recarga: write 1 <valor_recarga> -> salva novo credito
 */
static void processa_cartao(uint8_t *CardID)
{
    /* read <blockstart> 1 */
    int credito = read_credit(CardID);
    if (credito < 0) {
        printf("Erro ao ler credito do bloco %d.\r\n", CREDIT_BLOCK);
        return;
    }

    printf("\n--- Saldo atual: R$ %d ---\n", credito);
    printf("[1] Leitura   [2] Recarga   [3] Retirada\n> ");

    int opcao = 0;
    if (scanf("%d", &opcao) != 1)
        return;

    if (opcao == 1) {
        /* Leitura: exibe saldo */
        printf("Saldo: R$ %d\n", credito);

    } else if (opcao == 2) {
        /* Recarga: insere valor em reais */
        printf("Valor de recarga (R$): ");
        int recarga = 0;
        if (scanf("%d", &recarga) != 1 || recarga <= 0) {
            printf("Valor invalido.\r\n");
            return;
        }

        int novo_credito = credito + recarga;

        /* write 1 <valor_recarga> */
        if (write_credit(CardID, novo_credito) < 0) {
            printf("Erro ao salvar credito no cartao.\r\n");
            return;
        }

        /* Salva a informacao de credito no bloco 1 */
        printf("Recarga de R$ %d efetuada. Novo saldo: R$ %d\n", recarga, novo_credito);

    } else if (opcao == 3) {
        /* Retirada: insere valor em reais */
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

        int novo_credito = credito - retirada;

        /* write 1 <novo_credito> */
        if (write_credit(CardID, novo_credito) < 0) {
            printf("Erro ao salvar credito no cartao.\r\n");
            return;
        }

        /* Salva a informacao de credito no bloco 1 */
        printf("Retirada de R$ %d efetuada. Novo saldo: R$ %d\n", retirada, novo_credito);

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

    printf("=== Sistema de Credito RFID ===\r\n");
    printf("Aproxime o cartao...\r\n\n");

    /* Inicia scan (loop principal) */
    while (1) {

        /* Cartao aproximado? */
        if (MFRC522_Request(PICC_REQIDL, tagType) != MI_OK) {
            MFRC522_Halt();
            continue; /* Nao: volta ao scan */
        }

        /* Sim: obtem ID */
        if (MFRC522_Anticoll(CardID) != MI_OK) {
            printf("Falha ao obter ID do cartao.\r\n");
            MFRC522_Halt();
            continue;
        }

        if (seleciona_cartao(CardID) < 0) {
            MFRC522_Halt();
            continue;
        }

        /* Sub-processo: leitura/recarga */
        processa_cartao(CardID);

        MFRC522_Halt();

        /* Guardar informacoes de credito dentro do cartao ja feito acima.
         * Retorna ao inicio do scan. */
        printf("\nAproximie o cartao para nova operacao...\r\n\n");
    }

    return 0;
}
