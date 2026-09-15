# Demo Freenove: direcional e buzzer (C)

Demo de componentes para o fliperama: os quatro botões direcionais da Projects Board são lidos individualmente e cada comando produz um padrão sonoro no buzzer. Ela serve para validar a montagem e os GPIOs antes da integração com RFID, créditos e jogos.

O código é em **C**, usando `wiringPi` e a numeração **BCM** dos GPIOs, seguindo os capítulos [3 — Buttons & LEDs](https://docs.freenove.com/projects/fnk0054/en/latest/fnk0054/codes/c%26py/3_Buttons_%26_LEDs.html) e [6 — Buzzer](https://docs.freenove.com/projects/fnk0054/en/latest/fnk0054/codes/c%26py/6_Buzzer.html) do kit FNK0054.

## Mapeamento da Projects Board

| Direção | Botão | GPIO BCM | Feedback sonoro |
| --- | --- | ---: | --- |
| Cima | S4 azul | 26 | 1 bipe |
| Esquerda | S5 amarelo | 20 | 2 bipes |
| Direita | S7 verde | 16 | 3 bipes |
| Baixo | S6 vermelho | 21 | 4 bipes |
| Áudio | Buzzer ativo | 12 | saída digital |

As entradas usam `pull-up`: um botão pressionado é lido como nível baixo. O programa aplica debounce de 50 ms, mostra o nome do botão, o GPIO e um contador por direção no terminal.

## Montagem

1. Ative os quatro botões no conjunto de chaves **No. 2**, conforme o capítulo 3.
2. Ative o circuito de botão na chave **No. 5**.
3. Ative o estágio do buzzer conforme o capítulo 6. Mantenha o transistor da Projects Board; não conecte o buzzer diretamente a um GPIO.

## Compilação e execução na Raspberry Pi

Com `wiringPi` já disponível na Raspberry Pi:

```bash
cd projeto-lab-embarcados/demo_freenove
gcc -Wall -Wextra -Werror directional_buzzer_demo.c -o directional_buzzer_demo -lwiringPi
./directional_buzzer_demo
```

Pressione cada botão uma vez. O buzzer deve tocar de um a quatro bipes conforme a direção. Encerre com `Ctrl+C`; será impresso um resumo dos comandos e o buzzer será desligado.

## Arquivos

- `directional_buzzer_demo.c`: demo principal com os quatro botões e buzzer.
- `arcade_io_demo.c`: teste inicial de um botão e buzzer, mantido como referência mínima.
