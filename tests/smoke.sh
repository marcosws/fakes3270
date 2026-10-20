#!/usr/bin/env bash
# Teste de fumaca do FakeS3270. Uso: tests/smoke.sh caminho/do/fakes3270[.exe]
set -u
BIN="${1:?informe o caminho do binario}"
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$(mktemp)"
fail=0

run() { printf '%b' "$1" | "$BIN" "${@:2}" | tr -d '\r'; }
check() { # nome, saida, trecho esperado
  if printf '%s' "$2" | grep -qF -- "$3"; then echo "OK   $1"; else echo "FAIL $1 (esperado: $3)"; fail=1; fi
}

out=$(run 'Connect(localhost:23)\nAscii(1,24,30)\nQuit\n')
check "connect + tela de login" "$out" "data: *** BANCO FAKE - MAINFRAME ***"
check "status conectado"        "$out" "U F U C(localhost:23) I 2 24 80"

out=$(run 'Connect(h)\nString("TESTE")\nTab\nString("1234")\nEnter\nAscii(1,24,14)\nQuit\n')
check "login valido -> menu" "$out" "data: MENU PRINCIPAL"

out=$(run 'Connect(h)\nString("ERRADO")\nTab\nString("1234")\nEnter\nAscii(22,1,40)\nQuit\n')
check "login invalido"  "$out" "data: LOGIN INVALIDO"

out=$(run 'Connect(h)\nString("BLOQUEADO")\nTab\nString("1234")\nEnter\nAscii(22,1,40)\nQuit\n')
check "usuario bloqueado" "$out" "data: USUARIO BLOQUEADO - CONTATE O ADMIN"

out=$(run 'Connect(h)\nString("INATIVO")\nTab\nString("1234")\nEnter\nAscii(22,1,40)\nQuit\n')
check "usuario inativo" "$out" "data: USUARIO DESATIVADO"

out=$(run 'Connect(h)\nString("TESTE")\nTab\nString("1234")\nEnter\nAscii(1,1,20,4)\nQuit\n' | head -40)
check "senha nao aparece"  "$out" "ok"
if printf '%s' "$out" | grep -q 1234; then echo "FAIL senha vazou na tela"; fail=1; else echo "OK   senha oculta"; fi

out=$(run 'Connect(h)\nString("TESTE")\nTab\nString("1234")\nEnter\nString("4")\nEnter\nAscii()\nQuit\n')
check "menu 4 -> validacao" "$out" "TESTE RIGHT / LEFT / TOP / BOTTOM"

out=$(run 'Connect(h)\nString("TESTE")\nTab\nString("1234")\nEnter\nString("5")\nEnter\nAscii()\nQuit\n')
check "menu 5 -> tabela" "$out" "LISTA DE CLIENTES"

out=$(run 'Connect(h)\nString("TESTE")\nTab\nString("1234")\nEnter\nString("6")\nEnter\nAscii()\nQuit\n')
check "menu 6 -> extrato" "$out" "EXTRATO BANCARIO"

out=$(run 'Connect(h)\nString("TESTE")\nTab\nString("1234")\nEnter\nString("7")\nEnter\nAscii()\nQuit\n')
check "menu 7 -> transferencia" "$out" "TRANSFERENCIA ENTRE CONTAS"

out=$(run 'Connect(h)\nString("TESTE")\nTab\nString("1234")\nEnter\nString("8")\nEnter\nQuery(ConnectionState)\nQuit\n')
check "menu 8 desconecta" "$out" "data: not-connected"

out=$(run 'Ascii\nEnter\nQuit\n')
check "sem conexao -> error" "$out" "error"

out=$(run 'Connect(h)\nFooBar\nQuit\n')
check "comando desconhecido -> error" "$out" "error"

out=$(run 'Connect(a)\nQuit\n' --scenario "$DIR/scenario_exemplo.txt")
check "cenario customizado" "$out" "ok"
out=$(run 'Connect(a)\nString("ABCD")\nEnter\nAscii(0,1,26)\nQuit\n' --scenario "$DIR/scenario_exemplo.txt")
check "transicao customizada" "$out" "data: ABCD EXECUTADA COM SUCESSO"

run 'Connect(a)\nString("x")\nQuit\n' --log "$LOG" >/dev/null
check "log de comandos" "$(tr -d '\r' < "$LOG")" 'String("x")'

out=$(FAKES3270_REFUSE_CONNECT=1 run 'Connect(a)\nQuit\n')
check "conexao recusada" "$out" "error"

out=$(run 'Query(ConnectionState)\nConnect(h)\nQuery(ConnectionState)\nDisconnect\nQuery(ConnectionState)\nQuit\n')
check "query antes de conectar"  "$out" "data: not-connected"
check "query conectado"          "$out" "data: connected-3270"
n=$(printf '%s' "$out" | grep -c 'data: not-connected')
[ "$n" -eq 2 ] && echo "OK   query apos disconnect" || { echo "FAIL query apos disconnect"; fail=1; }

rm -f "$LOG"
[ $fail -eq 0 ] && echo "TODOS OS TESTES PASSARAM" || { echo "HA FALHAS"; exit 1; }
