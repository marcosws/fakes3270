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

#define VERSION "2.1"
#define ROWS 24
#define COLS 80
#define MAX_SCREENS 48
#define MAX_TEXTS 96
#define MAX_FIELDS 24
#define MAX_RULES 48
#define MAX_CONDS 4
#define MAX_VARS 32
#define MAX_TOK 32
#define LINE_MAX_LEN 4096
#define MSG_ROW 22 /* 0-based: linha 23 da tela */

enum { A_STAY, A_GOTO, A_DISCONNECT };

typedef struct { int row, col; char text[COLS + 1]; } Text;
typedef struct {
    int row, col, len, hidden;
    char name[32];
    char value[COLS + 1];
} Field;
typedef struct {
    char key[8];
    int ncond;
    char cname[MAX_CONDS][32];
    char cval[MAX_CONDS][64];
    int action;
    int delay_ms;
    char target[32];
    char msg[COLS + 1];
} Rule;
typedef struct {
    char name[32];
    Text texts[MAX_TEXTS];
    int ntexts;
    Field fields[MAX_FIELDS];
    int nfields;
    Rule rules[MAX_RULES];
    int nrules;
} Screen;
typedef struct { char name[32]; char value[COLS + 1]; } Var;

static Screen screens[MAX_SCREENS];
static int nscreens = 0;
static Var vars[MAX_VARS];
static int nvars = 0;

/* estado de execucao */
static Screen *cur = NULL;
static int connected = 0;
static char host[256] = "";
static int crow = 0, ccol = 0;
static char message[COLS + 1] = "";
static int oerr_lock = 0;       /* teclado travado por erro de operador (ate Reset) */
static int pending = 0;         /* resposta do host em andamento */
static long long deadline = 0;
static Rule pend;
static FILE *logf = NULL;

/* ------------------------------------------------------------------ */
/* Cenario embutido                                                    */
/* ------------------------------------------------------------------ */
static const char *default_scenario[] = {
    /* ============================================================
     * LOGIN
     * TESTE/1234     -> OK
     * BLOQUEADO/1234 -> usuario bloqueado
     * INATIVO/1234   -> usuario desativado
     * outros         -> login invalido
     * ============================================================ */
    "screen LOGIN",
    "text 1 1 \"FAKE3270 v2.1 - SIMULADOR MAINFRAME\"",
    "text 2 25 \"*** BANCO FAKE - MAINFRAME ***\"",
    "text 3 20 \"Ambiente de Testes Integracao\"",
    "text 5 10 \"Userid:\"",
    "field 5 18 12 name=user",
    "text 6 10 \"Password:\"",
    "field 6 20 12 name=pass hidden",
    "text 8 10 \"Dica: TESTE/1234  BLOQUEADO/1234  INATIVO/1234\"",
    "text 23 2 \"PF3=SAIR\"",
    "on ENTER user=TESTE pass=1234 goto MENU",
    "on ENTER user=BLOQUEADO pass=1234 goto LOGIN message \"USUARIO BLOQUEADO - CONTATE O ADMIN\"",
    "on ENTER user=INATIVO pass=1234 goto LOGIN message \"USUARIO DESATIVADO\"",
    "on ENTER user=* pass=* goto LOGIN message \"LOGIN INVALIDO\"",
    "on ENTER goto LOGIN message \"INFORME USERID E PASSWORD\"",
    "on PF3 disconnect",

    /* ============================================================
     * MENU PRINCIPAL
     * ============================================================ */
    "screen MENU",
    "text 1 1 \"FAKE3270 - MENU PRINCIPAL\"",
    "text 2 25 \"MENU PRINCIPAL\"",
    "text 3 10 \"Usuario: ${user}\"",
    "text 5 10 \"1 - SALDO\"",
    "text 6 10 \"2 - CADASTRO\"",
    "text 7 10 \"3 - RELATORIO (LENTO)\"",
    "text 8 10 \"4 - VALIDACAO CAMPOS (R/L/T/B)\"",
    "text 9 10 \"5 - TABELA CLIENTES\"",
    "text 10 10 \"6 - EXTRATO\"",
    "text 11 10 \"7 - TRANSFERENCIA\"",
    "text 12 10 \"8 - SAIR\"",
    "text 13 10 \"9 - CAMPOS COM NOMES REPETIDOS\"",
    "text 14 10 \"Opcao ==>\"",
    "field 14 20 1 name=opt",
    "text 23 2 \"ENTER=CONFIRMA  PF3=LOGIN\"",
    "on ENTER opt=1 goto SALDO",
    "on ENTER opt=2 goto CADASTRO",
    "on ENTER opt=3 goto RELATORIO delay 800",
    "on ENTER opt=4 goto VALIDACAO",
    "on ENTER opt=5 goto TABELA",
    "on ENTER opt=6 goto EXTRATO",
    "on ENTER opt=7 goto TRANSFERENCIA",
    "on ENTER opt=8 disconnect",
    "on ENTER opt=9 goto REPETIDOS",
    "on ENTER goto MENU message \"OPCAO INVALIDA\"",
    "on PF3 goto LOGIN",

    /* ============================================================
     * REPETIDOS - rotulos iguais em campos de entrada distintos.
     * Permite testar clientes que precisam posicionar o cursor na
     * ocorrencia correta antes de enviar texto.
     * ============================================================ */
    "screen REPETIDOS",
    "text 1 1 \"FAKE3270 - CAMPOS REPETIDOS\"",
    "text 2 20 \"VALIDACAO DE NOMES REPETIDOS\"",
    "text 4 10 \"Nome:\"",
    "field 4 16 20 name=nome_principal",
    "text 6 10 \"Nome:\"",
    "field 6 16 20 name=nome_social",
    "text 8 10 \"Nome:\"",
    "field 8 16 20 name=nome_responsavel",
    "text 10 10 \"Preencha cada campo da lista\"",
    "text 23 2 \"ENTER=CONFIRMA  PF3=MENU\"",
    "on ENTER goto REPETIDOS_OK",
    "on PF3 goto MENU",

    "screen REPETIDOS_OK",
    "text 1 1 \"FAKE3270 - CAMPOS REPETIDOS OK\"",
    "text 2 20 \"DADOS CAPTURADOS\"",
    "text 4 10 \"Nome principal: ${nome_principal}\"",
    "text 5 10 \"Nome social: ${nome_social}\"",
    "text 6 10 \"Nome responsavel: ${nome_responsavel}\"",
    "text 23 2 \"PF3=MENU  ENTER=MENU\"",
    "on PF3 goto MENU",
    "on ENTER goto MENU",

    /* ============================================================
     * SALDO
     * ============================================================ */
    "screen SALDO",
    "text 1 1 \"FAKE3270 - CONSULTA DE SALDO\"",
    "text 2 25 \"CONSULTA DE SALDO\"",
    "text 4 10 \"Conta:\"",
    "text 4 17 \"000123-4\"",
    "text 5 10 \"Titular:\"",
    "text 5 19 \"CLIENTE TESTE\"",
    "text 6 10 \"Saldo atual:\"",
    "text 6 23 \"R$ 1.234,56\"",
    "text 7 10 \"Limite:\"",
    "text 7 18 \"R$ 5.000,00\"",
    "text 8 10 \"Status:\"",
    "text 8 18 \"ATIVO\"",
    "text 23 2 \"PF3=VOLTAR  ENTER=MENU\"",
    "on PF3 goto MENU",
    "on ENTER goto MENU",

    /* ============================================================
     * CADASTRO - campos Right + textos Top/Bottom
     * getTextByField(\"Status:\", 5) -> ATIVO
     * getTextByField(\"Situacao:\", 2) -> OK
     * getTextBeforeField(\"Situacao:\", 8) -> REF-0001
     * getTextBottonField(\"Codigo\", 6, 1) -> 000123
     * getTextTopField(\"000123\", 6, 1) -> Codigo
     * ============================================================ */
    "screen CADASTRO",
    "text 1 1 \"FAKE3270 - CADASTRO DE CLIENTE\"",
    "text 2 25 \"CADASTRO DE CLIENTE\"",
    "text 4 10 \"Nome:\"",
    "field 4 16 20 name=nome",
    "text 5 10 \"CPF:\"",
    "field 5 15 14 name=cpf",
    "text 6 10 \"Cidade:\"",
    "field 6 18 15 name=cidade",
    "text 8 10 \"Status:\"",
    "text 8 18 \"ATIVO\"",
    "text 10 10 \"Codigo\"",
    "text 11 10 \"000123\"",
    "text 13 12 \"REF-0001\"",
    "text 13 20 \"Situacao:\"",
    "text 13 30 \"OK\"",
    "text 23 2 \"ENTER=GRAVAR  PF3=VOLTAR\"",
    "on ENTER nome=* cpf=* goto CADASTRO_OK",
    "on ENTER goto CADASTRO message \"PREENCHA NOME E CPF\"",
    "on PF3 goto MENU",

    "screen CADASTRO_OK",
    "text 1 1 \"FAKE3270 - CLIENTE GRAVADO\"",
    "text 2 25 \"CLIENTE GRAVADO COM SUCESSO\"",
    "text 4 10 \"Nome: ${nome}\"",
    "text 5 10 \"CPF: ${cpf}\"",
    "text 6 10 \"Cidade: ${cidade}\"",
    "text 8 10 \"Status:\"",
    "text 8 18 \"GRAVADO\"",
    "text 23 2 \"PF3=MENU  ENTER=MENU\"",
    "on PF3 goto MENU",
    "on ENTER goto MENU",

    /* ============================================================
     * RELATORIO (delay - Wait Unlock)
     * ============================================================ */
    "screen RELATORIO",
    "text 1 1 \"FAKE3270 - RELATORIO MENSAL\"",
    "text 2 25 \"RELATORIO MENSAL\"",
    "text 5 10 \"RELATORIO GERADO COM SUCESSO\"",
    "text 6 10 \"Periodo:\"",
    "text 6 19 \"01/2026 a 10/2026\"",
    "text 7 10 \"Registros:\"",
    "text 7 21 \"1.248\"",
    "text 23 2 \"PF3=MENU  ENTER=MENU\"",
    "on PF3 goto MENU",
    "on ENTER goto MENU",

    /* ============================================================
     * VALIDACAO - layout dedicado Right / Left / Top / Bottom
     *
     * Right:  getTextByField(\"RightLbl:\", 8)  -> VALRIGHT
     * Left:   getTextBeforeField(\"LeftLbl:\", 8) -> VALLEFT
     * Top:    getTextTopField(\"BottomVal\", 7, 1) -> TopLbl
     * Bottom: getTextBottonField(\"TopLbl\", 9, 1) -> BottomVal
     *
     * Campos editaveis para Send Text:
     *   sendTextByField(\"InputR:\", \"ABC\")  etc.
     * ============================================================ */
    "screen VALIDACAO",
    "text 1 1 \"FAKE3270 - VALIDACAO DE CAMPOS\"",
    "text 2 20 \"TESTE RIGHT / LEFT / TOP / BOTTOM\"",

    "text 4 10 \"RightLbl:\"",
    "text 4 20 \"VALRIGHT\"",
    "text 4 40 \"InputR:\"",
    "field 4 48 10 name=inputr",

    "text 6 10 \"VALLEFT\"",
    "text 6 18 \"LeftLbl:\"",
    "text 6 40 \"InputL:\"",
    "field 6 48 10 name=inputl",

    "text 8 10 \"TopLbl\"",
    "text 9 10 \"BottomVal\"",
    "text 8 40 \"InputT:\"",
    "field 8 48 10 name=inputt",
    "text 10 40 \"InputB:\"",
    "field 10 48 10 name=inputb",

    "text 12 10 \"Descricao:\"",
    "field 12 21 30 name=desc",

    "text 14 10 \"Codigo:\"",
    "text 15 10 \"XYZ999\"",
    "text 14 30 \"Antes\"",
    "text 14 36 \"Marca:\"",
    "text 14 43 \"DEPOIS\"",

    "text 23 2 \"ENTER=CONFIRMA  PF3=MENU\"",
    "on ENTER goto VALIDACAO_OK",
    "on PF3 goto MENU",

    "screen VALIDACAO_OK",
    "text 1 1 \"FAKE3270 - VALIDACAO OK\"",
    "text 2 25 \"DADOS CAPTURADOS\"",
    "text 4 10 \"InputR: ${inputr}\"",
    "text 5 10 \"InputL: ${inputl}\"",
    "text 6 10 \"InputT: ${inputt}\"",
    "text 7 10 \"InputB: ${inputb}\"",
    "text 8 10 \"Descricao: ${desc}\"",
    "text 23 2 \"PF3=MENU  ENTER=MENU\"",
    "on PF3 goto MENU",
    "on ENTER goto MENU",

    /* ============================================================
     * TABELA CLIENTES
     * ============================================================ */
    "screen TABELA",
    "text 1 1 \"FAKE3270 - TABELA DE CLIENTES\"",
    "text 2 25 \"LISTA DE CLIENTES\"",
    "text 4 5 \"COD    NOME                 CIDADE          STATUS\"",
    "text 5 5 \"-----  -------------------  --------------  ------\"",
    "text 6 5 \"00001  JOAO DA SILVA        SAO PAULO       ATIVO\"",
    "text 7 5 \"00002  MARIA OLIVEIRA       RIO DE JANEIRO  ATIVO\"",
    "text 8 5 \"00003  PEDRO SANTOS         BELO HORIZONTE  BLOQUE\"",
    "text 9 5 \"00004  ANA COSTA            CURITIBA        INATIV\"",
    "text 10 5 \"00005  CARLOS SOUZA         BRASILIA        ATIVO\"",
    "text 12 5 \"Filtro:\"",
    "field 12 13 10 name=filtro",
    "text 13 5 \"Total:\"",
    "text 13 12 \"5\"",
    "text 23 2 \"ENTER=BUSCAR  PF3=MENU\"",
    "on ENTER goto TABELA message \"FILTRO APLICADO\"",
    "on PF3 goto MENU",

    /* ============================================================
     * EXTRATO
     * ============================================================ */
    "screen EXTRATO",
    "text 1 1 \"FAKE3270 - EXTRATO\"",
    "text 2 25 \"EXTRATO BANCARIO\"",
    "text 4 5 \"DATA        HISTORICO              VALOR\"",
    "text 5 5 \"----------  ---------------------  ----------\"",
    "text 6 5 \"01/10/2026  CREDITO SALARIO        + 5.000,00\"",
    "text 7 5 \"05/10/2026  DEBITO CARTAO          -   320,50\"",
    "text 8 5 \"10/10/2026  TED RECEBIDA           + 1.200,00\"",
    "text 9 5 \"15/10/2026  PAGTO CONTA LUZ        -   180,00\"",
    "text 11 5 \"Saldo:\"",
    "text 11 12 \"R$ 5.699,50\"",
    "text 23 2 \"PF3=MENU  ENTER=MENU\"",
    "on PF3 goto MENU",
    "on ENTER goto MENU",

    /* ============================================================
     * TRANSFERENCIA
     * ============================================================ */
    "screen TRANSFERENCIA",
    "text 1 1 \"FAKE3270 - TRANSFERENCIA\"",
    "text 2 25 \"TRANSFERENCIA ENTRE CONTAS\"",
    "text 4 10 \"Conta destino:\"",
    "field 4 25 10 name=conta",
    "text 5 10 \"Valor:\"",
    "field 5 17 12 name=valor",
    "text 6 10 \"Tipo:\"",
    "text 6 16 \"TED\"",
    "text 8 10 \"Obs:\"",
    "field 8 15 40 name=obs",
    "text 23 2 \"ENTER=CONFIRMA  PF3=CANCELA\"",
    "on ENTER conta=* valor=* goto TRANSF_OK",
    "on ENTER goto TRANSFERENCIA message \"INFORME CONTA E VALOR\"",
    "on PF3 goto MENU",

    "screen TRANSF_OK",
    "text 1 1 \"FAKE3270 - TRANSFERENCIA OK\"",
    "text 2 25 \"TRANSFERENCIA EFETUADA\"",
    "text 4 10 \"Conta: ${conta}\"",
    "text 5 10 \"Valor: ${valor}\"",
    "text 6 10 \"Obs: ${obs}\"",
    "text 8 10 \"Protocolo:\"",
    "text 8 21 \"TF-2026-00042\"",
    "text 23 2 \"PF3=MENU  ENTER=MENU\"",
    "on PF3 goto MENU",
    "on ENTER goto MENU",

    NULL
};

/* ------------------------------------------------------------------ */
/* Utilitarios                                                         */
/* ------------------------------------------------------------------ */
static long long now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void sleep_ms(long long ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static void rtrim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static char *ltrim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Divide linha em tokens separados por espaco; "aspas" agrupam. Altera a linha. */
static int tokenize(char *line, char **tok, int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            tok[n++] = p;
            while (*p && *p != '"') p++;
        } else {
            tok[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
    }
    return n;
}

static Screen *find_screen(const char *name) {
    for (int i = 0; i < nscreens; i++)
        if (strcasecmp(screens[i].name, name) == 0) return &screens[i];
    return NULL;
}

/* variaveis: valores dos campos nomeados da tela anterior, usados em ${nome} */
static void set_var(const char *name, const char *value) {
    for (int i = 0; i < nvars; i++)
        if (strcasecmp(vars[i].name, name) == 0) { copy_str(vars[i].value, sizeof vars[i].value, value); return; }
    if (nvars < MAX_VARS) {
        copy_str(vars[nvars].name, sizeof vars[nvars].name, name);
        copy_str(vars[nvars].value, sizeof vars[nvars].value, value);
        nvars++;
    }
}

static const char *get_var(const char *name) {
    for (int i = 0; i < nvars; i++)
        if (strcasecmp(vars[i].name, name) == 0) return vars[i].value;
    return "";
}

static void expand(const char *t, char *out, size_t cap) {
    size_t o = 0;
    while (*t && o + 1 < cap) {
        if (t[0] == '$' && t[1] == '{') {
            const char *e = strchr(t + 2, '}');
            if (e) {
                char nm[32];
                size_t n = (size_t)(e - (t + 2));
                if (n < sizeof nm) {
                    memcpy(nm, t + 2, n);
                    nm[n] = 0;
                    for (const char *v = get_var(nm); *v && o + 1 < cap; v++) out[o++] = *v;
                    t = e + 1;
                    continue;
                }
            }
        }
        out[o++] = *t++;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* Parser de cenario                                                   */
/* ------------------------------------------------------------------ */
static int parse_line(char *line, int lineno, const char *src) {
    char *tok[MAX_TOK];
    rtrim(line);
    char *p = ltrim(line);
    if (*p == 0 || *p == '#') return 0;
    int n = tokenize(p, tok, MAX_TOK);
    if (n == 0) return 0;

    if (strcasecmp(tok[0], "screen") == 0) {
        if (n < 2 || nscreens >= MAX_SCREENS) goto bad;
        memset(&screens[nscreens], 0, sizeof(Screen));
        copy_str(screens[nscreens].name, sizeof screens[nscreens].name, tok[1]);
        nscreens++;
        return 0;
    }
    if (nscreens == 0) goto bad;
    Screen *s = &screens[nscreens - 1];

    if (strcasecmp(tok[0], "text") == 0) {
        if (n < 4 || s->ntexts >= MAX_TEXTS) goto bad;
        Text *t = &s->texts[s->ntexts++];
        t->row = atoi(tok[1]) - 1;
        t->col = atoi(tok[2]) - 1;
        copy_str(t->text, sizeof t->text, tok[3]);
        return 0;
    }
    if (strcasecmp(tok[0], "field") == 0) {
        if (n < 4 || s->nfields >= MAX_FIELDS) goto bad;
        Field *f = &s->fields[s->nfields++];
        memset(f, 0, sizeof *f);
        f->row = atoi(tok[1]) - 1;
        f->col = atoi(tok[2]) - 1;
        f->len = atoi(tok[3]);
        /* col >= 2 (1-based): a celula anterior guarda o byte de atributo */
        if (f->len < 1 || f->len > COLS || f->col < 1 || f->row < 0 || f->row >= ROWS ||
            f->col + f->len > COLS) goto bad;
        for (int i = 4; i < n; i++) {
            if (strncasecmp(tok[i], "name=", 5) == 0) copy_str(f->name, sizeof f->name, tok[i] + 5);
            else if (strcasecmp(tok[i], "hidden") == 0) f->hidden = 1;
        }
        return 0;
    }
    if (strcasecmp(tok[0], "on") == 0) {
        if (n < 2 || s->nrules >= MAX_RULES) goto bad;
        Rule *r = &s->rules[s->nrules++];
        memset(r, 0, sizeof *r);
        copy_str(r->key, sizeof r->key, tok[1]);
        r->action = A_STAY;
        for (int i = 2; i < n; i++) {
            if (strcasecmp(tok[i], "goto") == 0 && i + 1 < n) {
                r->action = A_GOTO;
                copy_str(r->target, sizeof r->target, tok[++i]);
            } else if (strcasecmp(tok[i], "disconnect") == 0) {
                r->action = A_DISCONNECT;
            } else if (strcasecmp(tok[i], "stay") == 0) {
                r->action = A_STAY;
            } else if (strcasecmp(tok[i], "delay") == 0 && i + 1 < n) {
                r->delay_ms = atoi(tok[++i]);
            } else if (strcasecmp(tok[i], "message") == 0 && i + 1 < n) {
                copy_str(r->msg, sizeof r->msg, tok[++i]);
            } else if (strchr(tok[i], '=') && r->ncond < MAX_CONDS) {
                char *eq = strchr(tok[i], '=');
                *eq = 0;
                copy_str(r->cname[r->ncond], sizeof r->cname[0], tok[i]);
                copy_str(r->cval[r->ncond], sizeof r->cval[0], eq + 1);
                r->ncond++;
            } else goto bad;
        }
        return 0;
    }
bad:
    fprintf(stderr, "fakes3270: cenario %s linha %d invalida\n", src, lineno);
    return -1;
}

static int load_scenario_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "fakes3270: nao foi possivel abrir %s\n", path);
        return -1;
    }
    char buf[LINE_MAX_LEN];
    int ln = 0, rc = 0;
    while (fgets(buf, sizeof buf, f)) {
        ln++;
        if (parse_line(buf, ln, path) != 0) { rc = -1; break; }
    }
    fclose(f);
    if (rc == 0 && nscreens == 0) {
        fprintf(stderr, "fakes3270: cenario %s sem telas\n", path);
        rc = -1;
    }
    return rc;
}

static int load_default_scenario(void) {
    for (int i = 0; default_scenario[i]; i++) {
        char buf[LINE_MAX_LEN];
        copy_str(buf, sizeof buf, default_scenario[i]);
        if (parse_line(buf, i + 1, "<embutido>") != 0) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Modelo da tela                                                      */
/* ------------------------------------------------------------------ */
static void render(char g[ROWS][COLS + 1], int show_hidden) {
    for (int r = 0; r < ROWS; r++) { memset(g[r], ' ', COLS); g[r][COLS] = 0; }
    if (!cur) return;
    for (int i = 0; i < cur->ntexts; i++) {
        Text *t = &cur->texts[i];
        char buf[COLS + 1];
        expand(t->text, buf, sizeof buf);
        for (int k = 0; buf[k] && t->col + k < COLS; k++)
            if (t->row >= 0 && t->row < ROWS && t->col + k >= 0) g[t->row][t->col + k] = buf[k];
    }
    for (int i = 0; i < cur->nfields; i++) {
        Field *f = &cur->fields[i];
        g[f->row][f->col - 1] = ' '; /* byte de atributo aparece como espaco */
        for (int k = 0; k < f->len; k++) {
            char c = f->value[k];
            if (f->hidden && !show_hidden) c = ' ';
            g[f->row][f->col + k] = c ? c : ' ';
        }
    }
    for (int k = 0; message[k] && k < COLS - 1; k++) g[MSG_ROW][1 + k] = message[k];
}

static void reset_fields(Screen *s) {
    for (int i = 0; i < s->nfields; i++) {
        memset(s->fields[i].value, ' ', s->fields[i].len);
        s->fields[i].value[s->fields[i].len] = 0;
    }
}

static void enter_screen(Screen *s) {
    cur = s;
    message[0] = 0;
    reset_fields(s);
    if (s->nfields > 0) { crow = s->fields[0].row; ccol = s->fields[0].col; }
    else { crow = 0; ccol = 0; }
}

static int field_at(int r, int c) {
    if (!cur) return -1;
    for (int i = 0; i < cur->nfields; i++) {
        Field *f = &cur->fields[i];
        if (f->row == r && c >= f->col && c < f->col + f->len) return i;
    }
    return -1;
}

static void goto_field(int i) {
    crow = cur->fields[i].row;
    ccol = cur->fields[i].col;
}

static void capture_vars(void) {
    if (!cur) return;
    for (int i = 0; i < cur->nfields; i++) {
        Field *f = &cur->fields[i];
        if (!f->name[0]) continue;
        char tmp[COLS + 1];
        copy_str(tmp, sizeof tmp, f->value);
        rtrim(tmp);
        set_var(f->name, ltrim(tmp));
    }
}

static void do_tab(int back) {
    if (!cur || cur->nfields == 0) return;
    int idx = field_at(crow, ccol), n = cur->nfields;
    int next;
    if (idx < 0) {
        next = back ? n - 1 : 0;
        for (int i = 0; i < n; i++) {
            Field *f = &cur->fields[i];
            int after = f->row > crow || (f->row == crow && f->col > ccol);
            if (!back && after) { next = i; break; }
            if (back && !after) next = i;
        }
    } else {
        next = back ? (idx + n - 1) % n : (idx + 1) % n;
    }
    goto_field(next);
}

static void move_cursor(int dr, int dc) {
    int r = crow + dr, c = ccol + dc;
    if (c < 0) { c = COLS - 1; r--; }
    if (c >= COLS) { c = 0; r++; }
    if (r < 0) r = ROWS - 1;
    if (r >= ROWS) r = 0;
    crow = r; ccol = c;
}

/* erro de operador (campo protegido): trava o teclado ate o Reset */
static int protected_error(void) {
    oerr_lock = 1;
    fprintf(stderr, "fakes3270: campo protegido na posicao (%d,%d); teclado travado\n", crow, ccol);
    return 0;
}

static int type_char(char ch) {
    int i = field_at(crow, ccol);
    if (i < 0) return protected_error();
    Field *f = &cur->fields[i];
    f->value[ccol - f->col] = ch;
    if (ccol + 1 < f->col + f->len) ccol++;
    else do_tab(0);
    return 1;
}

static int do_delete(void) {
    int i = field_at(crow, ccol);
    if (i < 0) return protected_error();
    Field *f = &cur->fields[i];
    int k = ccol - f->col;
    memmove(f->value + k, f->value + k + 1, (size_t)(f->len - k - 1));
    f->value[f->len - 1] = ' ';
    return 1;
}

static int match_rule(Rule *r, const char *key) {
    if (strcasecmp(r->key, key) != 0) return 0;
    for (int c = 0; c < r->ncond; c++) {
        int ok = 0;
        for (int i = 0; i < cur->nfields; i++) {
            Field *f = &cur->fields[i];
            if (strcasecmp(f->name, r->cname[c]) == 0) {
                char tmp[COLS + 1];
                copy_str(tmp, sizeof tmp, f->value);
                rtrim(tmp);
                const char *got = ltrim(tmp);
                ok = strcmp(r->cval[c], "*") == 0 ? got[0] != 0 : strcmp(got, r->cval[c]) == 0;
                break;
            }
        }
        if (!ok) return 0;
    }
    return 1;
}

static int apply_rule(const Rule *r) {
    char msg[COLS + 1];
    copy_str(msg, sizeof msg, r->msg);
    if (r->action == A_DISCONNECT) {
        connected = 0;
        cur = NULL;
        pending = 0;
        oerr_lock = 0;
        return 1;
    }
    if (r->action == A_GOTO) {
        Screen *t = find_screen(r->target);
        if (!t) {
            fprintf(stderr, "fakes3270: tela '%s' inexistente\n", r->target);
            return 0;
        }
        enter_screen(t);
    }
    copy_str(message, sizeof message, msg);
    return 1;
}

/* aplica a resposta pendente do host se ja venceu (ou, com block, espera ate vencer) */
static void settle(int block) {
    if (!pending) return;
    long long n = now_ms();
    if (n < deadline) {
        if (!block) return;
        sleep_ms(deadline - n);
    }
    pending = 0;
    apply_rule(&pend);
}

static int kbd_locked(void) {
    return oerr_lock || (pending && now_ms() < deadline);
}

/* Comandos de teclado: falham se travado por erro; esperam o host se houver resposta em curso. */
static int need_keyboard(void) {
    if (!connected || !cur) return 0;
    if (oerr_lock) {
        fprintf(stderr, "fakes3270: teclado travado (use Reset)\n");
        return 0;
    }
    settle(1);
    return connected && cur != NULL;
}

/* tecla de atencao (ENTER, PFn, PAn, CLEAR). Devolve 0 em erro. */
static int press_aid(const char *key) {
    if (!need_keyboard()) return 0;
    capture_vars();
    for (int i = 0; i < cur->nrules; i++) {
        Rule *r = &cur->rules[i];
        if (!match_rule(r, key)) continue;
        if (r->delay_ms > 0) {
            pend = *r;
            pending = 1;
            deadline = now_ms() + r->delay_ms;
            return 1;
        }
        return apply_rule(r);
    }
    if (strcasecmp(key, "CLEAR") == 0) {
        reset_fields(cur);
        message[0] = 0;
        if (cur->nfields > 0) goto_field(0); else { crow = 0; ccol = 0; }
    }
    return 1;
}

/* espera a resposta do host; timeout_s < 0 = sem limite. 0 = estourou o tempo */
static int wait_host(int timeout_s) {
    if (!pending) return 1;
    long long rem = deadline - now_ms();
    if (rem < 0) rem = 0;
    if (timeout_s >= 0 && rem > (long long)timeout_s * 1000) {
        sleep_ms((long long)timeout_s * 1000);
        return 0;
    }
    settle(1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Protocolo                                                           */
/* ------------------------------------------------------------------ */
static void print_status(void) {
    if (connected) {
        printf("%c %c %c C(%s) I 2 %d %d %d %d 0x0 0.001\n",
               kbd_locked() ? 'L' : 'U',
               (cur && cur->nfields > 0) ? 'F' : 'U',
               field_at(crow, ccol) >= 0 ? 'U' : 'P',
               host, ROWS, COLS, crow, ccol);
    } else {
        printf("U F U N N 2 %d %d 0 0 0x0 -\n", ROWS, COLS);
    }
}

static void finish(int ok) {
    print_status();
    printf(ok ? "ok\n" : "error\n");
    fflush(stdout);
}

/* "arg1","arg2",arg3 -> argv[]; trata \" \\ \n \t dentro de aspas */
static int parse_args(char *s, char **argv, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '"') {
            s++;
            argv[n++] = s;
            char *w = s;
            while (*s && *s != '"') {
                if (*s == '\\' && s[1]) {
                    s++;
                    *w++ = (*s == 'n') ? '\n' : (*s == 't') ? '\t' : *s;
                    s++;
                } else *w++ = *s++;
            }
            if (*s == '"') s++;
            *w = 0;
        } else {
            char *out = argv[n++] = s;
            while (*s && *s != ',') s++;
            char *e = s;
            int comma = (*s == ',');
            while (e > out && (e[-1] == ' ' || e[-1] == '\t')) e--;
            *e = 0;
            if (comma) { s++; continue; }
            break;
        }
        while (*s == ' ' || *s == '\t') s++;
        if (*s == ',') s++;
    }
    return n;
}

static void emit_ascii(int r0, int c0, int rows, int cols) {
    char g[ROWS][COLS + 1];
    render(g, 0);
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    for (int r = r0; r < r0 + rows && r < ROWS; r++) {
        printf("data: ");
        int written = 0;
        for (int c = c0; c < c0 + cols && c < COLS; c++) {
            char ch = g[r][c];
            /* nunca emite \0 — sempre espaco ou caractere visivel */
            putchar(ch ? ch : ' ');
            written++;
        }
        /* completa ate a largura solicitada (ex.: 80 no Ascii completo) */
        while (written < cols) {
            putchar(' ');
            written++;
        }
        putchar('\n');
    }
}

static int type_string(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') { if (!press_aid("ENTER")) return 0; }
        else if (*s == '\t') do_tab(0);
        else if (!type_char(*s)) return 0;
    }
    return 1;
}

static int do_wait(int argc, char **argv) {
    int timeout = -1;
    const char *what;
    if (argc == 1) what = argv[0];
    else if (argc == 2) { timeout = atoi(argv[0]); what = argv[1]; }
    else return 0;

    if (!strcasecmp(what, "Seconds")) {
        if (argc != 2) return 0;
        if (getenv("FAKES3270_REAL_WAIT")) sleep_ms((long long)timeout * 1000);
        return 1;
    }
    if (!strcasecmp(what, "Disconnect")) {
        if (connected && !wait_host(timeout)) return 0;
        return !connected;
    }
    if (strcasecmp(what, "Unlock") && strcasecmp(what, "InputField") && strcasecmp(what, "Output") &&
        strcasecmp(what, "3270Mode") && strcasecmp(what, "NVTMode") && strcasecmp(what, "ansi"))
        return 0; /* tipo de espera desconhecido */
    if (!connected) return 0;
    if (!wait_host(timeout)) return 0;
    if (!connected) return 0;
    if (!strcasecmp(what, "Unlock")) return !oerr_lock;
    if (!strcasecmp(what, "InputField")) return field_at(crow, ccol) >= 0;
    if (!strcasecmp(what, "3270Mode") || !strcasecmp(what, "Output")) return 1;
    return 0; /* NVTMode / ansi: o fake e sempre 3270 */
}

static int is_kbd_action(const char *n) {
    static const char *t[] = {"String", "Key", "Enter", "Clear", "PF", "PA", "Tab", "BackTab", "Newline",
                              "Home", "Up", "Down", "Left", "Right", "Erase", "BackSpace", "Delete",
                              "DeleteField", "EraseEOF", "EraseInput", NULL};
    for (int i = 0; t[i]; i++)
        if (!strcasecmp(n, t[i])) return 1;
    return 0;
}

static int handle_command(char *line) {
    settle(0); /* aplica a resposta do host que ja venceu */
    char *p = ltrim(line);
    char name[64];
    char *argv[16];
    int argc = 0;

    char *paren = strchr(p, '(');
    if (paren) {
        size_t nl = (size_t)(paren - p);
        if (nl >= sizeof name) return 0;
        memcpy(name, p, nl);
        name[nl] = 0;
        char *args = paren + 1;
        char *close = strrchr(args, ')');
        if (close) *close = 0;
        argc = parse_args(args, argv, 16);
    } else {
        copy_str(name, sizeof name, p);
        rtrim(name);
        char *sp = strchr(name, ' ');
        if (sp) *sp = 0;
    }
    rtrim(name);

    if (!strcasecmp(name, "Quit") || !strcasecmp(name, "Exit")) {
        if (getenv("FAKES3270_QUIT_REPLY")) finish(1);
        fflush(stdout);
        exit(0);
    }
    if (!strcasecmp(name, "Connect")) {
        if (connected || nscreens == 0) return 0;
        if (getenv("FAKES3270_REFUSE_CONNECT")) return 0;
        copy_str(host, sizeof host, argc > 0 ? argv[0] : "fake");
        connected = 1;
        pending = 0;
        oerr_lock = 0;
        nvars = 0;
        enter_screen(&screens[0]);
        return 1;
    }
    if (!strcasecmp(name, "Disconnect")) {
        connected = 0;
        cur = NULL;
        pending = 0;
        oerr_lock = 0;
        return 1;
    }
    if (!strcasecmp(name, "Reset")) { oerr_lock = 0; return 1; }
    if (!strcasecmp(name, "Wait")) return do_wait(argc, argv);
    if (!strcasecmp(name, "Query")) {
        if (argc >= 1 && !strcasecmp(argv[0], "ConnectionState")) {
            printf("data: %s\n", connected ? "connected-3270" : "not-connected");
            return 1;
        }
        if (argc >= 1 && !strcasecmp(argv[0], "Host")) {
            if (!connected) return 0;
            printf("data: %s\n", host);
            return 1;
        }
        return 1;
    }
    if (!strcasecmp(name, "Ascii") || !strcasecmp(name, "Snap") || !strcasecmp(name, "Ascii1")) {
        if (!strcasecmp(name, "Snap") && argc == 0) return connected;
        if (!strcasecmp(name, "Snap") && argc >= 1) {
            if (!strcasecmp(argv[0], "Rows")) { printf("data: %d\n", ROWS); return 1; }
            if (!strcasecmp(argv[0], "Cols")) { printf("data: %d\n", COLS); return 1; }
            if (!strcasecmp(argv[0], "Ascii")) argc = 0;
            else return 1;
        }
        if (!connected) return 0;
        if (argc == 0) emit_ascii(0, 0, ROWS, COLS);
        else if (argc == 1) emit_ascii(crow, ccol, 1, atoi(argv[0]));
        else if (argc == 3) emit_ascii(atoi(argv[0]), atoi(argv[1]), 1, atoi(argv[2]));
        else if (argc == 4) emit_ascii(atoi(argv[0]), atoi(argv[1]), atoi(argv[2]), atoi(argv[3]));
        else return 0;
        return 1;
    }
    if (!strcasecmp(name, "Info") || !strcasecmp(name, "Pause") || !strcasecmp(name, "Sleep") ||
        !strcasecmp(name, "Trace") || !strcasecmp(name, "Set") || !strcasecmp(name, "Toggle") ||
        !strcasecmp(name, "Expect"))
        return 1;

    if (!connected) return 0; /* demais acoes exigem conexao, como no s3270 real */

    if (!strcasecmp(name, "MoveCursor")) {
        if (argc < 2) return 0;
        int r = atoi(argv[0]), c = atoi(argv[1]);
        if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return 0;
        crow = r; ccol = c;
        return 1;
    }

    if (is_kbd_action(name) && !need_keyboard()) return 0;

    if (!strcasecmp(name, "String")) return argc > 0 ? type_string(argv[0]) : 0;
    if (!strcasecmp(name, "Key")) return argc > 0 ? type_char(argv[0][0]) : 0;
    if (!strcasecmp(name, "Enter")) return press_aid("ENTER");
    if (!strcasecmp(name, "Clear")) return press_aid("CLEAR");
    if (!strcasecmp(name, "PF") || !strcasecmp(name, "PA")) {
        char key[16];
        int pf = (name[1] == 'F' || name[1] == 'f');
        if (argc < 1) return 0;
        int n = atoi(argv[0]);
        if (n < 1 || n > (pf ? 24 : 3)) return 0;
        snprintf(key, sizeof key, "%s%d", pf ? "PF" : "PA", n);
        return press_aid(key);
    }
    if (!strcasecmp(name, "Tab") || !strcasecmp(name, "Newline")) { do_tab(0); return 1; }
    if (!strcasecmp(name, "BackTab")) { do_tab(1); return 1; }
    if (!strcasecmp(name, "Home")) {
        if (cur->nfields) goto_field(0); else { crow = 0; ccol = 0; }
        return 1;
    }
    if (!strcasecmp(name, "Up")) { move_cursor(-1, 0); return 1; }
    if (!strcasecmp(name, "Down")) { move_cursor(1, 0); return 1; }
    if (!strcasecmp(name, "Left") || !strcasecmp(name, "BackSpace")) { move_cursor(0, -1); return 1; }
    if (!strcasecmp(name, "Right")) { move_cursor(0, 1); return 1; }
    if (!strcasecmp(name, "Delete")) return do_delete();
    if (!strcasecmp(name, "Erase")) {
        int i = field_at(crow, ccol);
        if (i < 0) return protected_error();
        if (ccol > cur->fields[i].col) { ccol--; do_delete(); }
        return 1;
    }
    if (!strcasecmp(name, "DeleteField")) {
        int i = field_at(crow, ccol);
        if (i < 0) return protected_error();
        Field *f = &cur->fields[i];
        memset(f->value, ' ', (size_t)f->len);
        goto_field(i);
        return 1;
    }
    if (!strcasecmp(name, "EraseEOF")) {
        int i = field_at(crow, ccol);
        if (i < 0) return protected_error();
        Field *f = &cur->fields[i];
        for (int k = ccol - f->col; k < f->len; k++) f->value[k] = ' ';
        return 1;
    }
    if (!strcasecmp(name, "EraseInput")) {
        reset_fields(cur);
        if (cur->nfields) goto_field(0);
        return 1;
    }

    fprintf(stderr, "fakes3270: comando nao suportado: %s\n", name);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static void usage(void) {
    puts("Uso: fakes3270 [--scenario ARQ] [--log ARQ] [opcoes do s3270 ignoradas] [host]\n"
         "  --scenario ARQ   arquivo de cenario (padrao: cenario embutido)\n"
         "  --log ARQ        grava cada comando recebido (para asserts nos testes)\n"
         "  --version        mostra a versao\n"
         "Variaveis: FAKES3270_SCENARIO, FAKES3270_LOG, FAKES3270_REFUSE_CONNECT,\n"
         "           FAKES3270_QUIT_REPLY (Quit responde ok antes de sair),\n"
         "           FAKES3270_REAL_WAIT (Wait(n,Seconds) dorme de verdade)");
}

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const char *scenario = getenv("FAKES3270_SCENARIO");
    const char *logpath = getenv("FAKES3270_LOG");
    const char *autohost = NULL;
    /* opcoes reais do s3270 que consomem um valor */
    static const char *with_value[] = {"-model", "-xrm", "-tracefile", "-charset", "-port", "-connecttimeout",
                                       "-proxy", "-scriptport", "-httpd", "-cafile", "-capath", "-certfile",
                                       "-keyfile", "-user", "-loginmacro", "-tracefilesize", "-set", NULL};

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logpath = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) { puts("fakes3270 " VERSION); return 0; }
        else if (argv[i][0] == '-') {
            for (int k = 0; with_value[k]; k++)
                if (!strcmp(argv[i], with_value[k])) { i++; break; }
        } else autohost = argv[i];
    }

    if (scenario ? load_scenario_file(scenario) != 0 : load_default_scenario() != 0) return 2;
    if (logpath) {
        logf = fopen(logpath, "a");
        if (!logf) { fprintf(stderr, "fakes3270: nao foi possivel abrir log %s\n", logpath); return 2; }
    }
    if (autohost) {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "Connect(%s)", autohost);
        handle_command(cmd);
    }

    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof line, stdin)) {
        rtrim(line);
        if (logf) { fprintf(logf, "%s\n", line); fflush(logf); }
        int ok = handle_command(line);
        finish(ok);
    }
    return 0;
}
