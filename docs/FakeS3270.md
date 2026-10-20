# FakeS3270 v2 — guia de uso

`fakes3270` substitui o `s3270` nos testes de integração da biblioteca **mf3270**
(`S3270Session` / `S3270Emulator`). Ele fala o protocolo de script do s3270 por stdin/stdout, mas
não abre conexão TCP: simula um mainframe a partir de um **cenário** (telas + transições).

A v2 foi desenhada para validar todos os métodos da `S3270Emulator`: modela byte de atributo,
campos protegidos, travamento de teclado, `Reset`, respostas lentas do host (`Wait(Unlock)`),
typeahead e variáveis entre telas.

## Estrutura

```
src/fakes3270.c                 código-fonte (C99, sem dependências)
bin/linux-x64/fakes3270         binário Linux x86-64
bin/windows-x64/fakes3270.exe   binário Windows x86-64 (estático)
tests/smoke.sh                  teste de fumaça do protocolo
tests/emulator_contract.py      teste de contrato: reproduz os comandos e a aritmética de posição da S3270Emulator
tests/scenario_exemplo.txt      exemplo de cenário customizado
.github/workflows/ci.yml        build + testes no GitHub Actions (Linux e Windows)
Makefile                        make linux | make windows | make test
```

## Uso local

```bash
make test                       # compila e roda smoke.sh + emulator_contract.py
./bin/linux-x64/fakes3270       # modo interativo: digite os comandos
```

Na biblioteca, injete o executável pelo construtor `S3270Session(List<String> command, Map<String,String> env)`.
As opções do s3270 real (`-model`, `-xrm`, `-utf8`, ...) são aceitas e ignoradas. Um argumento sem `-`
é tratado como host e conecta automaticamente.

## Opções e variáveis de ambiente

| Opção / variável                        | Efeito                                                                 |
|-----------------------------------------|------------------------------------------------------------------------|
| `--scenario ARQ` / `FAKES3270_SCENARIO` | Usa o cenário do arquivo (padrão: cenário embutido)                    |
| `--log ARQ` / `FAKES3270_LOG`           | Grava cada comando recebido (para asserts nos testes)                  |
| `FAKES3270_REFUSE_CONNECT=1`            | `connect(...)` devolve `error`                                         |
| `FAKES3270_QUIT_REPLY=1`                | `Quit` responde status + `ok` antes de sair (padrão: sai sem responder) |
| `FAKES3270_REAL_WAIT=1`                 | `Wait(n,Seconds)` dorme de verdade (padrão: retorna na hora)           |

## Protocolo

Cada comando gera 0+ linhas `data: ...`, uma linha de status e `ok` ou `error`. Mensagens de diagnóstico
vão para stderr (a `S3270Session` usa `redirectErrorStream(true)`, então aparecem antes do `error`).

```
connect(localhost:23)
U F U C(localhost:23) I 2 24 80 4 17 0x0 0.001
ok
```

Status: `teclado formatação proteção conexão modo modelo linhas colunas cursor-linha cursor-coluna ...`

- teclado: `U` livre, `L` travado (erro de operador ou host processando);
- proteção: `U` cursor em campo de entrada, `P` cursor em área protegida;
- linha/coluna do cursor, de `Ascii(...)` e de `MoveCursor` são **0-based**, como no s3270.

`Ascii()` devolve 24 linhas `data: ` + 80 colunas, depois status e `ok` — formato que `asciiScreen()`,
`getScreen()` e `getText*()` esperam (a aritmética de posição delas conta com o prefixo `data: `).

## Modelo de tela

- **Byte de atributo:** cada campo de entrada tem um byte de atributo na célula anterior ao primeiro
  caractere. Ele aparece como espaço no `Ascii` e é protegido. Por isso `sendTextByField("Userid:", ...)`
  só funciona quando o campo começa **duas colunas depois do fim do rótulo** (rótulo, atributo, campo) —
  exatamente como numa tela real. Num cenário: `field` 1-based col = col do rótulo + tamanho do rótulo + 1.
- **Teclado travado:** digitar (ou apagar) fora de um campo — texto fixo, byte de atributo — devolve
  `error` e trava o teclado (`L`). Enquanto travado, comandos de teclado e `Wait(Unlock)` devolvem `error`.
  `Reset` destrava (é o que `S3270Emulator.reset()` faz no s3270 real).
- **Resposta lenta do host (`delay`):** numa regra com `delay N`, a tela antiga continua visível, o teclado
  fica `L` por N ms e só então a transição acontece. `Wait(Unlock)`, `Wait(Output)` e `Wait(InputField)`
  esperam; `Wait(0,Unlock)` estoura na hora. Comandos de teclado enviados nesse intervalo ficam em
  **typeahead**: esperam o desbloqueio e executam na tela nova. Um teste que esquece o `waitFor` lê a tela antiga.
- **Variáveis:** ao apertar uma tecla de atenção, os campos com `name=` da tela atual são guardados e podem
  ser exibidos em qualquer texto como `${nome}` — útil para provar que o texto foi parar no campo certo.
- Tela 24x80 (modelo 2); linha 23 é a linha de mensagem.

## Comandos suportados × métodos da S3270Emulator

| Método da `S3270Emulator`                         | Comando enviado             | Comportamento no fake |
|---------------------------------------------------|-----------------------------|-----------------------|
| `asciiScreen()`, `getScreen()`, `getText*()`, `waitForText()` | `Ascii()`      | 24 linhas `data: ` de 80 colunas |
| `moveCursor(r,c)`, `moveCursor(pos)`              | `MoveCursor(r,c)`           | 0-based; fora da tela → `error`; não exige teclado livre |
| `sendString(t)`                                   | `String("t")`               | Digita no cursor; `\n` = Enter, `\t` = Tab; fora de campo → `error` + trava |
| `sendTextByField(...)`, `moveAndSendString(...)`  | `MoveCursor` + `String`     | Idem |
| `tab()` / `home()`                                | `Tab()` / `Home()`          | Próximo campo / primeiro campo |
| `enter()`                                         | `Enter`                     | Avalia regras `on ENTER` |
| `pressPF(k)` / `pressPA(k)`                       | `PF(n)` / `PA(n)`           | `PF` 1–24, `PA` 1–3; fora do intervalo → `error` |
| `clear()`                                         | `Clear()`                   | Regra `on CLEAR` ou, sem regra, limpa os campos |
| `eraseEOF()`                                      | `EraseEOF`                  | Apaga do cursor até o **fim do campo** |
| `eraseInput()`                                    | `EraseInput`                | Limpa **todos** os campos de entrada; cursor no primeiro |
| `deleteField()`                                   | `DeleteField`               | Limpa o campo atual; cursor no início dele |
| `backspace()`                                     | `Backspace()`               | Só move o cursor para a esquerda (como o s3270) |
| `reset()`                                         | `Reset`                     | Destrava o teclado; não altera a tela |
| `waitFor(type)`, `waitFor(type, s)`               | `Wait(Type)` / `Wait(s,Type)` | `Unlock`, `InputField`, `Output`, `3270Mode`, `NVTMode` (sempre `error`), `Disconnect` (`ok` só se desconectado) |
| `waitSeconds(n)`                                  | `Wait(n,Seconds)`           | `ok` imediato (ou dorme com `FAKES3270_REAL_WAIT`) |
| `S3270Session.connect`                            | `connect(host:port)`        | Conecta e mostra a primeira tela do cenário |
| `S3270Session.isConnected`                        | `Query(ConnectionState)`    | `data: connected-3270` ou `data: not-connected` |
| `S3270Session.disconnect` / `quit`                | `Disconnect` / `Quit`       | `Quit` encerra sem responder (ou com `ok` se `FAKES3270_QUIT_REPLY=1`) |

Também aceitos: `Key(c)`, `Left/Right/Up/Down`, `BackTab`, `Newline`, `Erase`, `Delete`, `Snap`, `Snap(Ascii)`,
`Snap(Rows|Cols)`, `Query(Host)`, `Ascii(len)`, `Ascii(l,c,len)`, `Ascii(l,c,linhas,colunas)`; e, como no-op, `Info`,
`Pause`, `Sleep`, `Set`, `Toggle`, `Trace`, `Expect`. Qualquer outro comando → `error` (e o nome vai para o stderr).
Sem conexão, tudo devolve `error` exceto `Query`, `Reset`, `Wait(Disconnect)` e os no-ops.

## Cenários

Texto simples; `#` inicia comentário; linhas e colunas são **1-based**.

```
screen LOGIN                         # a primeira tela é a exibida após o connect
text 5 10 "Userid:"                  # texto fixo: linha coluna "texto" (aceita ${variavel})
field 5 18 8 name=user               # campo de entrada: linha coluna tamanho; atributo na coluna anterior
field 6 20 8 name=pass hidden        # hidden: não aparece no Ascii
on ENTER user=TESTE pass=1234 goto MENU
on ENTER goto LOGIN message "LOGIN INVALIDO"      # sem condição = padrão
on PF3 disconnect
```

`on TECLA [campo=valor ...] (goto TELA | disconnect | stay) [delay MS] [message "texto"]`

- Regras são avaliadas na ordem; vence a primeira que casar. Teclas: `ENTER`, `CLEAR`, `PF1..PF24`, `PA1..PA3`.
- Condições: `campo=valor` (igual, ignorando espaços nas pontas), `campo=*` (não vazio), `campo=` (vazio).
- `message` aparece na linha 23 (coluna 2) da tela de destino.
- Ao entrar numa tela os campos são limpos e o cursor vai ao primeiro campo.
- `field` exige coluna ≥ 2 (a anterior guarda o atributo) e que o campo caiba na linha.
- Limites: 32 telas; por tela 64 textos, 16 campos, 32 regras; 32 variáveis.

### Cenário embutido

`LOGIN` (`TESTE`/`1234`) → `MENU` (1 saldo, 2 cadastro, 3 relatório **lento**, 4 sair) → `SALDO`,
`CADASTRO` → `CADASTRO_OK`, `RELATORIO`. `PF3` volta. A tela `CADASTRO` traz textos preparados para os
métodos de leitura: `getTextByField("Status:", 5)` → `ATIVO`, `getTextByField("Situacao:", 2)` → `OK`,
`getTextBeforeField("Situacao:", 8)` → `REF-0001`, `getTextBottonField("Codigo", 6, 1)` → `000123`,
`getTextTopField("000123", 6, 1)` → `Codigo`.

## Asserts nos testes

Use `--log` (ou `FAKES3270_LOG`) para validar o que a biblioteca enviou, e `Ascii` para validar o que ela leu:

```bash
FAKES3270_LOG=/tmp/cmds.txt ./sua-suite-de-testes
grep -F 'String("TESTE")' /tmp/cmds.txt
```

## GitHub Actions

`.github/workflows/ci.yml` compila o fake em `ubuntu-latest` e `windows-latest`, roda `smoke.sh` e
`emulator_contract.py` e exporta `S3270_PATH` para o passo de testes da sua biblioteca. Os binários em `bin/`
servem para uso local rápido; no CI o recomendado é compilar a partir do código.

## Compilar manualmente

```bash
gcc -std=c99 -O2 -D_POSIX_C_SOURCE=200809L -o fakes3270 src/fakes3270.c                 # Linux
x86_64-w64-mingw32-gcc -std=c99 -O2 -static -o fakes3270.exe src/fakes3270.c             # Windows (cross)
```

## Limitações conhecidas

- Não há 3270 de verdade: sem negociação TN3270, sem campos estendidos (cor, intensidade), sem modo NVT/ANSI,
  sem `Transfer`/`Printtext` e sem `Macro`/`Script`.
- `Clear` sem regra limpa só os campos (no host real a tela inteira seria apagada).
- O formato exato do status varia um pouco entre versões do s3270; o fake usa o formato de 12 campos.
