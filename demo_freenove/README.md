# Demo Freenove: botão e buzzer (C)

Pequena prova de conceito para o fliperama: enquanto o botão está pressionado, o buzzer fica ligado. O contador exibido no terminal representa um evento de entrada que, no sistema final, poderá disparar uma ação de menu ou jogo.

Ela combina os exemplos oficiais **3.1 Buttons & LEDs** e **6.1 Doorbell** do [tutorial FNK0054](https://docs.freenove.com/projects/fnk0054/en/latest/fnk0054/c%26py.html). A numeração dos pinos é **BCM**, não a numeração física do conector. A implementação é em **C**, com `wiringPi`, como nos exemplos C do tutorial.

## Montagem

Ative o botão conforme o circuito **3.1 Buttons & LEDs** e o estágio do buzzer conforme o circuito **6.1 Doorbell** do tutorial:

| Sinal | GPIO BCM | Função |
| --- | ---: | --- |
| Botão | 26 | entrada com pull-up interno; pressionado = nível baixo |
| Buzzer ativo | 12 | saída digital |
| Terra | GND | referência comum da placa |

Na Projects Board, faça as chaves/jumpers indicados nos diagramas dos capítulos 3 e 6. O estágio com transistor da placa deve ser mantido: não conecte o buzzer diretamente a um GPIO fora do circuito recomendado, pois o GPIO não deve fornecer sua corrente de acionamento.

## Compilação e execução na Raspberry Pi

No Raspberry Pi OS, com a placa conectada e o `wiringPi` instalado conforme a preparação do tutorial:

```bash
cd projeto-lab-embarcados/demo_freenove
gcc -Wall -Wextra -Werror arcade_io_demo.c -o arcade_io_demo -lwiringPi
./arcade_io_demo
```

Pressione o botão para ligar o buzzer e solte para desligá-lo. Encerre com `Ctrl+C`; o programa desliga o buzzer antes de terminar.

## Verificação sem a placa

A compilação no computador de desenvolvimento exige a biblioteca `wiringPi`. Na Raspberry Pi, o próprio comando de compilação acima verifica a sintaxe e a vinculação da biblioteca. A execução requer GPIOs reais e a Projects Board montada.

## Referência de pinagem

O capítulo 3 usa GPIO 26 para o botão e o capítulo 6 usa GPIO 12 para o buzzer. O exemplo 6.1 usa GPIO 21 para o seu botão próprio; ele não é utilizado nesta combinação. Em ambos os exemplos, a leitura do botão usa `pull-up`, onde o evento pressionado corresponde a nível lógico baixo.
