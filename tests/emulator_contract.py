#!/usr/bin/env python3
"""
Teste de contrato do FakeS3270 contra o comportamento da S3270Session/S3270Emulator (mf3270).

Reproduz, em Python, os comandos e a aritmetica de posicao do S3270Emulator
(asciiScreen, getPositionField, sendTextByField, getText*, waitFor*, ...), para garantir que
o fake devolve exatamente o que essas rotinas esperam. Uso: tests/emulator_contract.py BINARIO
"""
import os
import subprocess
import sys
import time

FAILS = []


def check(name, cond, detail=""):
    print(("OK   " if cond else "FAIL ") + name + ("" if cond else f"  [{detail}]"))
    if not cond:
        FAILS.append(name)


class CmdError(Exception):
    pass


class Session:
    """Equivalente a S3270Session.sendCommand: le ate a linha 'ok' / 'error'."""

    def __init__(self, binary, extra_env=None, args=()):
        env = dict(os.environ)
        env.update(extra_env or {})
        self.p = subprocess.Popen([binary, "-model", "3279-2", *args], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)

    def send(self, cmd):
        self.p.stdin.write((cmd + "\n").encode())
        self.p.stdin.flush()
        resp = ""
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise CmdError("stream closed unexpectedly")
            line = line.decode().rstrip("\r\n")
            resp += line + "\n"
            if line == "ok":
                return resp
            if line == "error":
                raise CmdError(resp)

    def fails(self, cmd):
        try:
            self.send(cmd)
            return False
        except CmdError:
            return True

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        self.p.terminate()
        self.p.wait()


# ---- reproducao do S3270Emulator ------------------------------------------------------------
def ascii_screen(s):               # asciiScreen(): devolve o rawScreen
    return s.send("Ascii()")


def get_screen(s):                 # getScreen(): remove as 2 ultimas linhas e "data:"
    lines = s.send("Ascii()").split("\n")
    return "\n".join(l.strip().replace("data:", "") for l in lines[:max(0, len(lines) - 2)])


def get_position_field(screen, field):
    for i, line in enumerate(screen.split("\n")):
        c = line.find(field)
        if c >= 0:
            return (i + 1, c + len(field) + 1)
    return None


def move_and_send(s, row, col, text):
    s.send(f"MoveCursor({row},{col})")
    s.send(f'String("{text}")')


def send_text_by_field(s, field, text):
    pos = get_position_field(ascii_screen(s).replace("data:", ""), field)
    if pos is None:
        raise KeyError(field)
    move_and_send(s, pos[0] - 1, pos[1] - 1, text)


def get_positions_field(screen, field):
    """Retorna todas as ocorrencias de um rotulo, em ordem de tela."""
    positions = []
    for i, line in enumerate(screen.split("\n")):
        start = 0
        while True:
            c = line.find(field, start)
            if c < 0:
                break
            positions.append((i + 1, c + len(field) + 1))
            start = c + len(field)
    return positions


def send_text_by_field_offset(s, field, text, off_row, off_col):
    pos = get_position_field(ascii_screen(s).replace("data:", ""), field)
    row, col = pos
    t_row = abs(row - 1) + abs(off_row) if off_row >= 0 else abs(row - 1) - abs(off_row)
    t_col = abs(col - 1) + abs(off_col) if off_col >= 0 else abs(col - 1) - (len(field) + abs(off_col))
    move_and_send(s, t_row, t_col, text)


def get_text(s, row, col, size):
    lines = ascii_screen(s).split("\n")
    line = lines[row - 1]
    if col + size > len(line):     # em Java: StringIndexOutOfBoundsException
        raise IndexError(f"substring({col},{col + size}) em linha de {len(line)}")
    return line[col:col + size]


def get_text_by_field(s, field, size):
    r, c = get_position_field(ascii_screen(s), field)
    return get_text(s, r, c, size)


def get_text_before_field(s, field, size):
    r, c = get_position_field(ascii_screen(s), field)
    new_col = abs(abs((c - 1) - size) - len(field))
    return get_text(s, r, new_col, size)


def get_text_bottom_field(s, field, size, off_row):
    r, c = get_position_field(ascii_screen(s), field)
    return get_text(s, abs(r + off_row), abs((c - 1) - len(field)), size)


def get_text_top_field(s, field, size, off_row):
    r, c = get_position_field(ascii_screen(s), field)
    return get_text(s, abs(r - off_row), abs((c - 1) - len(field)), size)


def has(s, text):
    return text in get_screen(s)


def login(s):
    s.send("connect(localhost:23)")
    send_text_by_field(s, "Userid:", "TESTE")
    send_text_by_field(s, "Password:", "1234")
    s.send("Enter")


def select_menu(s, option):
    send_text_by_field(s, "Opcao ==>", option)
    s.send("Enter")


def run(binary):
    # ---- sessao / protocolo ----
    s = Session(binary)
    check("Query(ConnectionState) antes de conectar", "not-connected" in s.send("Query(ConnectionState)"))
    check("Ascii sem conexao -> error", s.fails("Ascii()"))
    r = s.send("connect(localhost:23)")
    check("connect(host:port) -> ok + status C(host)", "C(localhost:23)" in r and r.endswith("ok\n"), r)
    check("Query(ConnectionState) conectado", "connected-3270" in s.send("Query(ConnectionState)"))
    raw = ascii_screen(s)
    lines = raw.split("\n")
    check("Ascii(): 24 linhas 'data: ' de 80 colunas + status + ok",
          len(lines) == 24 + 3 and all(l.startswith("data: ") and len(l) == 86 for l in lines[:24]), len(lines))
    check("getScreen(): tela de login", "*** BANCO FAKE - MAINFRAME ***" in get_screen(s))
    check("comando desconhecido -> error", s.fails("Foo()"))

    # ---- sendTextByField / getPositionField ----
    send_text_by_field(s, "Userid:", "TESTE")
    check("sendTextByField digita no campo apos o atributo", "Userid: TESTE" in get_screen(s).replace("  ", " "),
          get_screen(s).split("\n")[4])
    send_text_by_field(s, "Password:", "1234")
    password_line = next(line for line in get_screen(s).split("\n") if "Password:" in line)
    check("campo senha oculto no Ascii", "1234" not in password_line, password_line)
    s.send("Enter")
    check("Enter -> MENU", has(s, "MENU PRINCIPAL"))
    check("variavel ${user} da tela anterior", has(s, "Usuario: TESTE"))

    # ---- offsets ----
    s.send("PF(3)")
    check("PF(3) volta ao login", has(s, "Userid:"))
    send_text_by_field_offset(s, "Userid:", "9999", 1, 2)   # 1 linha abaixo, 2 colunas a direita
    send_text_by_field(s, "Userid:", "ABC")
    s.send("Enter")
    check("login invalido: mensagem na linha 23", has(s, "LOGIN INVALIDO"))
    s.send("Reset")

    # ---- navegacao ate CADASTRO ----
    send_text_by_field(s, "Userid:", "TESTE")
    send_text_by_field(s, "Password:", "1234")
    s.send("Enter")
    select_menu(s, "2")
    check("menu 2 -> CADASTRO", has(s, "CADASTRO DE CLIENTE"))

    # ---- getText* ----
    check("getTextByField('Status:', 5)", get_text_by_field(s, "Status:", 5) == "ATIVO",
          get_text_by_field(s, "Status:", 5))
    check("getTextByField('Situacao:', 2)", get_text_by_field(s, "Situacao:", 2) == "OK")
    check("getTextBeforeField('Situacao:', 8)", get_text_before_field(s, "Situacao:", 8) == "REF-0001",
          get_text_before_field(s, "Situacao:", 8))
    check("getTextBottonField('Codigo', 6, 1)", get_text_bottom_field(s, "Codigo", 6, 1) == "000123",
          get_text_bottom_field(s, "Codigo", 6, 1))
    check("getTextTopField('000123', 6, 1)", get_text_top_field(s, "000123", 6, 1) == "Codigo",
          get_text_top_field(s, "000123", 6, 1))

    # ---- campo vazio -> mensagem; preenchendo -> gravado ----
    s.send("Enter")
    check("Enter sem preencher -> PREENCHA NOME E CPF", has(s, "PREENCHA NOME E CPF"))
    send_text_by_field(s, "Nome:", "JOAO DA SILVA")
    send_text_by_field(s, "CPF:", "12345678901")
    send_text_by_field(s, "Cidade:", "CAMPINAS")
    s.send("Enter")
    scr = get_screen(s)
    check("dados chegaram nos campos certos",
          "Nome: JOAO DA SILVA" in scr and "CPF: 12345678901" in scr and "Cidade: CAMPINAS" in scr, scr)
    s.send("PF(3)")

    # ---- edicao de campo: EraseEOF / DeleteField / EraseInput / Tab / Home / Backspace ----
    select_menu(s, "2")                                       # CADASTRO
    s.send("Home()")
    s.send('String("ABCDEFG")')
    check("Home() + String() no 1o campo", has(s, "Nome: ABCDEFG"))
    s.send("Backspace()")                                     # so move o cursor (como no s3270)
    s.send("EraseEOF")
    check("EraseEOF apaga ate o fim do campo (nao da linha)", has(s, "Nome: ABCDEF") and "ABCDEFG" not in get_screen(s))
    s.send("Home()")
    s.send("EraseEOF")
    check("Home + EraseEOF limpa o campo", "ABCDEF" not in get_screen(s))
    s.send('String("XYZ")')
    s.send("Tab()")
    s.send('String("111")')
    check("Tab() vai para o proximo campo", has(s, "CPF: 111"))
    s.send("DeleteField")
    check("DeleteField limpa so o campo atual", has(s, "Nome: XYZ") and "111" not in get_screen(s))
    s.send("EraseInput")
    check("EraseInput limpa todos os campos", "XYZ" not in get_screen(s))
    s.send('String("ZZ")')
    s.send("Clear()")
    check("Clear() limpa os campos", "ZZ" not in get_screen(s))

    # ---- campo protegido trava o teclado; Reset destrava ----
    s.send("MoveCursor(0,0)")
    check("String em area protegida -> error", s.fails('String("x")'))
    check("teclado travado: status 'L'", s.send("Ascii(0,0,1)").split("\n")[1].startswith("L "))
    check("teclado travado: Enter -> error", s.fails("Enter"))
    check("teclado travado: Wait(Unlock) -> error", s.fails("Wait(Unlock)"))
    s.send("Reset")
    check("Reset destrava", not s.fails("Home()"))
    check("MoveCursor fora da tela -> error", s.fails("MoveCursor(24,0)"))
    s.send("MoveCursor(3,14)")                                # byte de atributo do campo Nome
    check("String no byte de atributo -> error", s.fails('String("x")'))
    s.send("Reset")

    # ---- PF / PA ----
    check("PA(1) ok", not s.fails("PA(1)"))
    check("PA(4) -> error", s.fails("PA(4)"))
    check("PF(25) -> error", s.fails("PF(25)"))
    s.send("PF(3)")
    check("PF(3) volta ao MENU", has(s, "MENU PRINCIPAL"))

    # ---- transicao lenta: Wait(Unlock) / typeahead ----
    select_menu(s, "3")
    check("host ainda processando: tela antiga e teclado 'L'",
          has(s, "MENU PRINCIPAL") and s.send("Ascii(0,0,1)").split("\n")[1].startswith("L "))
    check("Wait(0,Unlock) estoura enquanto o host processa", s.fails("Wait(0,Unlock)"))
    t0 = time.time()
    s.send("Wait(Unlock)")
    check("Wait(Unlock) espera o host (>= ~0.5s)", time.time() - t0 > 0.4, f"{time.time() - t0:.2f}s")
    check("depois do Unlock: nova tela", has(s, "RELATORIO GERADO COM SUCESSO"))
    s.send("PF(3)")
    select_menu(s, "3")
    s.send("PF(3)")                                           # typeahead: espera e executa sobre RELATORIO
    check("typeahead: PF(3) apos Enter lento volta ao MENU", has(s, "MENU PRINCIPAL"))

    # ---- Wait variantes ----
    check("Wait(Output)", not s.fails("Wait(Output)"))
    check("Wait(10,InputField) com cursor em campo", not s.fails("Wait(10,InputField)"))
    check("Wait(3270Mode)", not s.fails("Wait(3270Mode)"))
    check("Wait(NVTMode) -> error (sempre 3270)", s.fails("Wait(NVTMode)"))
    check("Wait(Disconnect) conectado -> error", s.fails("Wait(Disconnect)"))
    check("Wait(5,Seconds) nao dorme", (lambda t: (s.send("Wait(5,Seconds)"), time.time() - t)[1] < 1)(time.time()))
    check("Wait(Seconds) sem tempo -> error", s.fails("Wait(Seconds)"))
    check("Wait(tipo invalido) -> error", s.fails("Wait(Foo)"))

    # ---- telas adicionadas ao cenario padrao ----
    select_menu(s, "4")
    check("menu 4 -> VALIDACAO", has(s, "TESTE RIGHT / LEFT / TOP / BOTTOM"))
    check("VALIDACAO getTextByField('RightLbl:', 8)",
          get_text_by_field(s, "RightLbl:", 8) == "VALRIGHT")
    check("VALIDACAO getTextBeforeField('LeftLbl:', 8)",
          get_text_before_field(s, "LeftLbl:", 8).strip() == "VALLEFT",
          repr(get_text_before_field(s, "LeftLbl:", 8)))
    check("VALIDACAO getTextTopField('BottomVal', 6, 1)",
          get_text_top_field(s, "BottomVal", 6, 1) == "TopLbl")
    check("VALIDACAO getTextBottonField('TopLbl', 9, 1)",
          get_text_bottom_field(s, "TopLbl", 9, 1) == "BottomVal")
    send_text_by_field(s, "InputR:", "ABC")
    send_text_by_field(s, "InputL:", "DEF")
    send_text_by_field(s, "Descricao:", "TESTE DE CONTRATO")
    s.send("Enter")
    check("VALIDACAO preserva dados digitados", has(s, "InputR: ABC") and has(s, "InputL: DEF") and
          has(s, "Descricao: TESTE DE CONTRATO"))
    s.send("PF(3)")

    select_menu(s, "9")
    check("menu 9 -> CAMPOS REPETIDOS", has(s, "VALIDACAO DE NOMES REPETIDOS"))
    positions = get_positions_field(ascii_screen(s).replace("data:", ""), "Nome:")
    check("CAMPOS REPETIDOS exibe tres rotulos Nome:", len(positions) == 3, repr(positions))
    for position, value in zip(positions, ("ANA", "BIA", "CARLA")):
        move_and_send(s, position[0] - 1, position[1] - 1, value)
    s.send("Enter")
    check("CAMPOS REPETIDOS preserva cada ocorrencia",
          has(s, "Nome principal: ANA") and has(s, "Nome social: BIA") and
          has(s, "Nome responsavel: CARLA"))
    s.send("PF(3)")

    select_menu(s, "5")
    check("menu 5 -> TABELA", has(s, "LISTA DE CLIENTES") and has(s, "00001  JOAO DA SILVA"))
    s.send("PF(3)")

    select_menu(s, "6")
    check("menu 6 -> EXTRATO", has(s, "EXTRATO BANCARIO") and has(s, "CREDITO SALARIO"))
    s.send("PF(3)")

    select_menu(s, "7")
    check("menu 7 -> TRANSFERENCIA", has(s, "TRANSFERENCIA ENTRE CONTAS"))
    s.send("Enter")
    check("TRANSFERENCIA exige conta e valor", has(s, "INFORME CONTA E VALOR"))
    send_text_by_field(s, "Conta destino:", "000456-7")
    send_text_by_field(s, "Valor:", "150,00")
    send_text_by_field(s, "Obs:", "PAGAMENTO")
    s.send("Enter")
    check("TRANSFERENCIA confirma valores", has(s, "Conta: 000456-7") and has(s, "Valor: 150,00") and
          has(s, "Obs: PAGAMENTO"))
    s.send("PF(3)")

    # ---- encerrar ----
    select_menu(s, "8")
    check("opcao 8 desconecta", "not-connected" in s.send("Query(ConnectionState)"))
    check("Wait(Disconnect) desconectado -> ok", not s.fails("Wait(Disconnect)"))
    check("Ascii apos desconectar -> error", s.fails("Ascii()"))
    s.send("connect(outro:23)")
    try:
        s.send("Quit")
        check("Quit sem resposta encerra o stream", False)
    except CmdError as e:
        check("Quit sem resposta encerra o stream (stream closed)", "stream closed" in str(e))
    s.close()

    # ---- Quit com FAKES3270_QUIT_REPLY ----
    q = Session(binary, {"FAKES3270_QUIT_REPLY": "1"})
    q.send("connect(x:23)")
    check("FAKES3270_QUIT_REPLY: Quit responde ok", q.send("Quit").endswith("ok\n"))
    q.close()

    # ---- recusar conexao ----
    r = Session(binary, {"FAKES3270_REFUSE_CONNECT": "1"})
    check("FAKES3270_REFUSE_CONNECT -> error", r.fails("connect(x:23)"))
    r.close()


if __name__ == "__main__":
    run(sys.argv[1])
    print("\nTODOS OS TESTES PASSARAM" if not FAILS else f"\nFALHAS ({len(FAILS)}): {FAILS}")
    sys.exit(1 if FAILS else 0)
