# FakeS3270

Simulador de `s3270` para testes de integração. Ele recebe comandos pelo
protocolo de script do s3270 via stdin/stdout, sem abrir uma conexão TCP, e
emula telas 3270 a partir de um cenário configurável.

O projeto é útil principalmente para testar aplicações que usam
`S3270Session` ou `S3270Emulator` sem depender de um mainframe disponível.

## Recursos

- protocolo de script compatível com os comandos mais usados do `s3270`;
- tela 24×80, campos de entrada, byte de atributo e campos protegidos;
- teclado bloqueado, `Reset`, `Wait(Unlock)` e typeahead;
- transições entre telas, mensagens e atrasos de resposta;
- cenário embutido de operações bancárias e suporte a cenários externos;
- compilação para Linux e Windows, sem dependências de runtime.

## Compilação e testes

Pré-requisitos: `make`, um compilador C99 (por exemplo, GCC ou Clang) e
Python 3 para os testes de contrato.

```bash
make linux     # gera bin/linux-x64/fakes3270
make test      # compila e executa toda a suíte
```

Para gerar o binário Windows em um ambiente Linux, é necessário o compilador
cross `x86_64-w64-mingw32-gcc`:

```bash
make windows   # gera bin/windows-x64/fakes3270.exe
```

## Uso rápido

```bash
./bin/linux-x64/fakes3270
```

No modo interativo, envie comandos do protocolo, um por linha:

```text
Connect(localhost:23)
Ascii()
String("TESTE")
Tab()
String("1234")
Enter
Ascii()
Quit
```

O cenário padrão aceita `TESTE`/`1234` e disponibiliza consultas, cadastro,
relatório com atraso, validação de campos, tabela de clientes, extrato e
transferência, além de uma tela com campos de entrada com o mesmo rótulo.

## Cenários personalizados

Use `--scenario` para carregar telas e transições de um arquivo:

```bash
./bin/linux-x64/fakes3270 --scenario tests/scenario_exemplo.txt
```

Também é possível usar `FAKES3270_SCENARIO`, `FAKES3270_LOG`,
`FAKES3270_REFUSE_CONNECT`, `FAKES3270_QUIT_REPLY` e `FAKES3270_REAL_WAIT`.

Consulte a [documentação completa](docs/FakeS3270.md) para a sintaxe dos
cenários, a lista de comandos suportados e detalhes do protocolo.

## Licença

Este projeto é distribuído sob a [Apache License 2.0](LICENSE).
