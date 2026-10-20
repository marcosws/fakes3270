/*
 * FakeS3270 v2 - s3270 falso para testar bibliotecas que controlam o s3270 via pipe
 * (em especial a S3270Session / S3270Emulator da mf3270).
 *
 * Fala o protocolo de script do s3270: le comandos de stdin (um por linha) e responde
 * em stdout com zero ou mais linhas "data: ...", uma linha de status e "ok" ou "error".
 *
 * Modelo de tela 3270 (24x80):
 *  - cada campo de entrada tem um byte de atributo na celula anterior ao primeiro
 *    caractere (aparece como espaco no Ascii) e e "protegido";
 *  - digitar fora de um campo (texto, atributo) trava o teclado ("L" no status) ate o Reset;
 *  - transicoes podem ter atraso ("delay"): o teclado fica travado e a tela antiga continua
 *    visivel ate o "host responder"; Wait(Unlock) espera. Comandos de teclado enviados nesse
 *    intervalo ficam em "typeahead": esperam o desbloqueio e depois executam.
 *
 * Telas vem de um arquivo de cenario (veja docs/FakeS3270.md) ou do cenario embutido.
 * Compila em Linux (gcc/clang) e Windows (mingw-w64/MSVC). Sem dependencias.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#include <time.h>
#endif
