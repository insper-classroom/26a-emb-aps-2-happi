#!/usr/bin/env python3
"""
Controle Bluetooth para The Binding of Isaac
Toda a entrada é roteada para um gamepad Xbox 360 virtual (vgamepad).

Mapeamento:
  Joystick analógico  → Analógico esquerdo (movimento do personagem)
  Botão R (físico)    → Botão X Xbox        (atirar pra esquerda)
  Botão Y (físico)    → Botão A Xbox        (atirar pra baixo)
  Botão B (físico)    → Botão Y Xbox        (atirar pra cima)
  Botão G (físico)    → Botão B Xbox        (atirar pra direita)
  Click SW            → Botão SELECT (Back)  (pill / item extra)
  Gesto "right"       → LB (Left Bumper)     (soltar bomba)
  Gesto "updown"      → LT (Left Trigger)    (usar item ativo)
"""

import sys
import glob
import serial
from serial.tools import list_ports
import vgamepad as vg
import tkinter as tk
from tkinter import ttk
from tkinter import messagebox
import threading
import time

# ─── Gamepad virtual (Xbox 360) ──────────────────────────────────────────────
gamepad = vg.VX360Gamepad()

# ─── Controle de gestos ───────────────────────────────────────────────────────
GESTO_CONFIANCA_MIN = 0.70  # ignora classificações fracas
GESTO_TAP_S        = 0.08   # duração do tap (press → release) em segundos
GESTO_COOLDOWN_S   = 1.20   # cooldown após disparar: ignora novas detecções do mesmo gesto

# Um bool por gesto — True = está em cooldown, não dispara de novo
_gesto_em_cooldown: dict[str, bool] = {"right": False, "updown": False}
_gesto_cooldown_timers: dict[str, threading.Timer | None] = {"right": None, "updown": None}


def _tap_gesto(pressionar_fn, soltar_fn):
    """Pressiona, aguarda GESTO_TAP_S e solta — tudo em thread daemon."""
    pressionar_fn()
    gamepad.update()

    def _soltar():
        soltar_fn()
        gamepad.update()

    t = threading.Timer(GESTO_TAP_S, _soltar)
    t.daemon = True
    t.start()


def _iniciar_cooldown(classe: str):
    """Marca cooldown ativo e agenda reset após GESTO_COOLDOWN_S."""
    _gesto_em_cooldown[classe] = True
    timer_ant = _gesto_cooldown_timers.get(classe)
    if timer_ant:
        timer_ant.cancel()

    def _fim():
        _gesto_em_cooldown[classe] = False

    t = threading.Timer(GESTO_COOLDOWN_S, _fim)
    t.daemon = True
    t.start()
    _gesto_cooldown_timers[classe] = t


def tratar_gesto(classe: str, confianca: float):
    """
    Dispara um tap único no botão/gatilho correspondente ao gesto.
    Após o tap, entra em cooldown (GESTO_COOLDOWN_S) para evitar
    disparo duplo caso o classificador continue retornando o mesmo gesto.
    'idle' é sempre ignorado.
    """
    if confianca < GESTO_CONFIANCA_MIN or classe == "idle":
        return

    if _gesto_em_cooldown.get(classe, False):
        return  # ainda em cooldown, ignora

    if classe == "right":
        # Tap no LB → solta bomba
        _tap_gesto(
            lambda: gamepad.press_button(button=vg.XUSB_BUTTON.XUSB_GAMEPAD_LEFT_SHOULDER),
            lambda: gamepad.release_button(button=vg.XUSB_BUTTON.XUSB_GAMEPAD_LEFT_SHOULDER),
        )
        _iniciar_cooldown("right")
        print(f"Gesto right → LB tap (bomba)")

    elif classe == "updown":
        # Tap no LT → usa item ativo
        _tap_gesto(
            lambda: gamepad.left_trigger(value=255),
            lambda: gamepad.left_trigger(value=0),
        )
        _iniciar_cooldown("updown")
        print(f"Gesto updown → LT tap (item)")

# ─── Escala do joystick analógico ─────────────────────────────────────────────
# O firmware envia valores aprox. -32 a +32 (centrado / 64 de ~2047).
# O vgamepad espera -32768 a +32767.
JOY_SCALE = 1000


# ─── Mapeamento botões físicos → botões Xbox ──────────────────────────────────
# Os valores são constantes do vgamepad (XUSB_BUTTON)
BOTAO_XBOX = {
    "R":  vg.XUSB_BUTTON.XUSB_GAMEPAD_X,            # Atirar pra esquerda
    "Y":  vg.XUSB_BUTTON.XUSB_GAMEPAD_A,            # Atirar pra baixo
    "B":  vg.XUSB_BUTTON.XUSB_GAMEPAD_Y,            # Atirar pra cima
    "G":  vg.XUSB_BUTTON.XUSB_GAMEPAD_B,            # Atirar pra direita
    "SW": vg.XUSB_BUTTON.XUSB_GAMEPAD_BACK,         # Select
}


def aplicar_joystick(eixo_x: float, eixo_y: float):
    """Atualiza o analógico esquerdo do gamepad virtual."""
    x = max(-32768, min(32767, int(eixo_x * JOY_SCALE)))
    y = max(-32768, min(32767, int(eixo_y * JOY_SCALE)))
    gamepad.left_joystick(x_value=x, y_value=y)
    gamepad.update()


def apertar_botao_xbox(botao: vg.XUSB_BUTTON, pressionar: bool):
    """Pressiona ou solta um botão do gamepad virtual."""
    if pressionar:
        gamepad.press_button(button=botao)
    else:
        gamepad.release_button(button=botao)
    gamepad.update()


# ─── Estado do joystick (acumula X e Y separadamente) ─────────────────────────

joy_state = {"X": 0, "Y": 0}


def controle(ser: serial.Serial):
    """Loop de leitura serial — roda em thread separada."""
    global joy_state

    while ser.is_open:
        try:
            raw_line = ser.readline()
            if not raw_line:
                continue
            line = raw_line.decode("utf-8", errors="ignore").strip()
            if not line:
                continue

            partes = line.split(",")
            if len(partes) != 3:
                continue

            tipo, campo, valor_str = partes

            # ── Joystick analógico ──────────────────────────────────────────
            if tipo == "JOY":
                try:
                    value = int(valor_str)
                except ValueError:
                    continue
                joy_state[campo] = value
                aplicar_joystick(joy_state["X"], joy_state["Y"])

            # ── Botões físicos — processados diretamente, sem fila ──────────
            elif tipo == "BTN":
                pressionar = (valor_str.strip() == "1")
                botao_xbox = BOTAO_XBOX.get(campo)
                if botao_xbox is not None:
                    apertar_botao_xbox(botao_xbox, pressionar)
                    estado = "pressionado" if pressionar else "solto"
                    print(f"Botão {campo} → {botao_xbox.name} {estado}")

            # ── Gestos da IMU — idle filtrado aqui, nunca chega ao handler ──
            elif tipo == "GESTURE":
                # Normaliza capitalização (firmware pode mandar "Idle" ou "idle")
                classe = campo.strip().lower()
                if classe == "idle":
                    continue  # descarta sem processar nem imprimir
                try:
                    confianca = float(valor_str)
                except ValueError:
                    continue
                print(f"Gesto: {campo} ({confianca:.2f})")
                tratar_gesto(classe, confianca)

        except serial.SerialException:
            break
        except Exception as e:
            print(f"Erro na leitura: {e}")
            break




def heartbeat(ser: serial.Serial):
    """Envia um sinal de vida para a Pico a cada 1 segundo."""
    while ser.is_open:
        try:
            ser.write(b"H")
            time.sleep(1)
        except Exception:
            break


# ─── Interface ────────────────────────────────────────────────────────────────

def serial_ports():
    if sys.platform.startswith("win"):
        return [p.device for p in list_ports.comports()]
    elif sys.platform.startswith(("linux", "cygwin")):
        return glob.glob("/dev/tty[A-Za-z]*")
    elif sys.platform.startswith("darwin"):
        return glob.glob("/dev/tty.*")
    else:
        raise EnvironmentError("Plataforma não suportada.")


def conectar_porta(port_name: str, root: tk.Tk, status_label: tk.Label,
                   mudar_cor_circulo):
    if not port_name:
        return
    ser = None
    try:
        ser = serial.Serial(port_name, 115200, timeout=1)
        status_label.config(text=f"Conectado em {port_name}", foreground="green")
        mudar_cor_circulo("green")
        root.update()
        threading.Thread(target=controle, args=(ser,), daemon=True).start()
        threading.Thread(target=heartbeat, args=(ser,), daemon=True).start()
    except Exception as e:
        messagebox.showerror(
            "Erro de Conexão",
            f"Não foi possível conectar em {port_name}.\nErro: {e}"
        )
        mudar_cor_circulo("red")
        if ser and ser.is_open:
            ser.close()


def criar_janela():
    root = tk.Tk()
    root.title("Controle — The Binding of Isaac")
    root.geometry("860x320")
    root.resizable(False, False)

    dark_bg = "#0f0e17"
    dark_fg = "#fffffe"
    accent  = "#ff8906"
    cell_bg = "#1a1a2e"
    root.configure(bg=dark_bg)

    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure("TFrame",    background=dark_bg)
    style.configure("TLabel",    background=dark_bg, foreground=dark_fg,
                    font=("Segoe UI", 11))
    style.configure("TButton",   font=("Segoe UI", 10, "bold"),
                    foreground=dark_fg, background="#2a2a4a", borderwidth=0)
    style.map("TButton",         background=[("active", "#3a3a5a")])
    style.configure("TCombobox", fieldbackground="#2a2a4a",
                    background="#2a2a4a", foreground=dark_fg, padding=4)
    style.map("TCombobox",       fieldbackground=[("readonly", "#2a2a4a")])

    frame = ttk.Frame(root, padding="20")
    frame.pack(expand=True, fill="both")

    tk.Label(frame, text="🎮  The Binding of Isaac — Controle Bluetooth",
             font=("Segoe UI", 14, "bold"), bg=dark_bg, fg=accent).pack(pady=(0, 10))

    # ── Mapeamento visual ──────────────────────────────────────────────────────
    mapa_frame = tk.Frame(frame, bg=dark_bg)
    mapa_frame.pack(pady=(0, 14))

    mapeamentos = [
        ("Joystick X/Y",  "Mover personagem (analógico esq.)"),
        ("Botão R",       "🔵 Xbox X — Atirar pra esquerda"),
        ("Botão Y",       "🟢 Xbox A — Atirar pra baixo"),
        ("Botão B",       "🟡 Xbox Y — Atirar pra cima"),
        ("Botão G",       "🔴 Xbox B — Atirar pra direita"),
        ("Click SW",      "⬛ SELECT — Pill / item extra"),
        ("Gesto right",   "LB — Soltar bomba"),
        ("Gesto updown",  "LT — Usar item ativo"),
    ]

    for i, (entrada, acao) in enumerate(mapeamentos):
        col = i % 4
        row = i // 4
        cell = tk.Frame(mapa_frame, bg=cell_bg, padx=10, pady=8)
        cell.grid(row=row, column=col, padx=5, pady=4, sticky="ew")
        tk.Label(cell, text=entrada, font=("Segoe UI", 9, "bold"),
                 bg=cell_bg, fg=accent).pack(anchor="w")
        tk.Label(cell, text=acao, font=("Segoe UI", 9),
                 bg=cell_bg, fg=dark_fg).pack(anchor="w")

    # ── Footer ─────────────────────────────────────────────────────────────────
    footer = tk.Frame(root, bg=dark_bg)
    footer.pack(side="bottom", fill="x", padx=14, pady=(8, 12))

    status_label = tk.Label(footer, text="Aguardando conexão...",
                             font=("Segoe UI", 10), bg=dark_bg, fg=dark_fg)
    status_label.grid(row=0, column=0, sticky="w")

    porta_var = tk.StringVar(value="")
    portas_disponiveis: list[str] = []

    port_dropdown = ttk.Combobox(footer, textvariable=porta_var,
                                  values=portas_disponiveis,
                                  state="readonly", width=12)
    port_dropdown.grid(row=0, column=1, padx=8)

    circle_canvas = tk.Canvas(footer, width=20, height=20,
                               highlightthickness=0, bg=dark_bg)
    circle_item = circle_canvas.create_oval(2, 2, 18, 18, fill="red", outline="")
    circle_canvas.grid(row=0, column=3, sticky="e")
    footer.columnconfigure(1, weight=1)

    def mudar_cor_circulo(cor: str):
        circle_canvas.itemconfig(circle_item, fill=cor)

    def atualizar_portas():
        nonlocal portas_disponiveis
        portas_disponiveis = serial_ports()
        port_dropdown["values"] = portas_disponiveis
        if portas_disponiveis:
            porta_var.set(portas_disponiveis[0])
            status_label.config(text="Selecione a COM do Bluetooth.",
                                 foreground="white")
        else:
            porta_var.set("")
            status_label.config(text="Nenhuma porta detectada.",
                                 foreground="orange")

    port_dropdown.bind(
        "<<ComboboxSelected>>",
        lambda e: conectar_porta(
            porta_var.get(), root, status_label, mudar_cor_circulo
        )
    )

    ttk.Button(footer, text="Atualizar",
               command=atualizar_portas).grid(row=0, column=2, padx=(0, 8))

    atualizar_portas()
    root.after(200, lambda: conectar_porta(
        porta_var.get(), root, status_label, mudar_cor_circulo
    ))
    root.mainloop()


if __name__ == "__main__":
    criar_janela()