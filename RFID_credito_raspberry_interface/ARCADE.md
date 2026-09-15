# Arcade RFID

O arcade reaproveita a leitura RC522 e o arquivo `cartoes.csv`: cada partida custa **1 crédito**. Há dois jogos Raylib: Cobrinha e Asteroides. A interface anterior (`main.c`/`build.sh`) foi preservada; o arcade está isolado em `arcade.c`.

No menu **RECARREGAR CREDITOS**, qualquer cartão pode receber +1, +5 ou +10 créditos. É uma recarga livre para a demonstração; não há cobrança/pagamento integrado.

## Controles físicos

Os quatro botões do direcional/joystick usam a numeração BCM já validada na Projects Board Freenove:

| Controle | BCM | No arcade |
|---|---:|---|
| Cima (S4 azul) | 26 | Seleciona / sobe na Cobrinha |
| Esquerda (S5 amarelo) | 20 | Volta no menu / move |
| Direita (S7 verde) | 16 | Confirma no menu / move |
| Baixo (S6 vermelho) | 21 | Seleciona / desce na Cobrinha |
| Buzzer ativo | 12 | Feedback: toque, início e fim de partida |

As entradas são `pull-up`: botão pressionado equivale a nível baixo. O buzzer é controlado sem `delay`, então não interrompe a renderização.

## Raspberry Pi

Instale Raylib, `wiringPi` e os requisitos do leitor RC522. Depois, a partir desta pasta:

```sh
sh build-arcade.sh rpi
sh run-arcade-rpi.sh
```

`run-arcade-rpi.sh` define `LIBGL_ALWAYS_SOFTWARE=1`. Esta é a flag Mesa que força a renderização por CPU (llvmpipe), contornando falhas de driver OpenGL no Raspberry. Para tentar aceleração por GPU, execute `./RFID_arcade` diretamente.

## Teste no computador

```sh
sh build-arcade.sh demo
sh run-arcade-demo.sh
```

No modo demo não acessa GPIO/RFID; setas ou WASD simulam o direcional e a seta direita abre o cartão `DEMO0001`.

## Atualizar o clone

Na raiz do repositório, antes de editar, atualize sem misturar mudanças locais:

```sh
git status
git pull --ff-only origin main
```

Se `git status` mostrar arquivos alterados, faça commit ou guarde-os com `git stash -u` antes do `pull`.
