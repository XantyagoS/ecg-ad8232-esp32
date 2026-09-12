#!/usr/bin/env python3
"""
Visor de ECG en la terminal para AD8232 + ESP32.  Sin dependencias:
solo la libreria estandar de Python 3.

  python3 ecg_term.py                      # lee /dev/cu.usbmodem101
  python3 ecg_term.py /dev/cu.usbmodemXXX  # otro puerto
  python3 ecg_term.py --demo               # senal PQRST sintetica, sin hardware
  python3 ecg_term.py --hum 50             # notch de 50 Hz (Europa)
  python3 ecg_term.py --raw                # solo numeros, sin dibujo

Filtra 0.5-40 Hz + notch de red, marca los picos R y calcula la
frecuencia cardiaca.  Salir con Ctrl+C.
"""
import sys, os, time, math, shutil, subprocess, threading, collections, argparse

FS       = 250          # debe coincidir con el sketch
SEGUNDOS = 4            # ventana visible
BAUD     = 115200

# ---------------------------------------------------------------- filtros
class Biquad:
    """Biquad en forma directa II transpuesta."""
    def __init__(self, b, a):
        self.b0, self.b1, self.b2 = b[0] / a[0], b[1] / a[0], b[2] / a[0]
        self.a1, self.a2 = a[1] / a[0], a[2] / a[0]
        self.z1 = self.z2 = 0.0

    def __call__(self, x):
        y = self.b0 * x + self.z1
        self.z1 = self.b1 * x - self.a1 * y + self.z2
        self.z2 = self.b2 * x - self.a2 * y
        return y


def _w(f0, fs, Q):
    w0 = 2.0 * math.pi * f0 / fs
    return w0, math.cos(w0), math.sin(w0) / (2.0 * Q)


def notch(f0, fs, Q=30.0):
    w0, c, al = _w(f0, fs, Q)
    return Biquad([1.0, -2.0 * c, 1.0], [1.0 + al, -2.0 * c, 1.0 - al])


def lowpass(fc, fs, Q=0.7071):
    w0, c, al = _w(fc, fs, Q)
    return Biquad([(1 - c) / 2, 1 - c, (1 - c) / 2], [1 + al, -2 * c, 1 - al])


def highpass(fc, fs, Q=0.7071):
    w0, c, al = _w(fc, fs, Q)
    return Biquad([(1 + c) / 2, -(1 + c), (1 + c) / 2], [1 + al, -2 * c, 1 - al])


# ---------------------------------------------------------------- fuentes
# La placa emite una linea "#EST <estado>" cada segundo con el resultado
# de su autodiagnostico de cableado.  Empieza por '#', asi que no
# interfiere con el parseo de muestras.
ESTADO = {"placa": None}


def fuente_serial(puerto, cola, parar):
    subprocess.run(["stty", "-f", puerto, str(BAUD), "raw", "-echo"],
                   check=True, capture_output=True)
    with open(puerto, "rb", buffering=0) as f:
        resto = b""
        while not parar.is_set():
            trozo = f.read(256)
            if not trozo:
                continue
            lineas = (resto + trozo).split(b"\n")
            resto = lineas.pop()
            for ln in lineas:
                ln = ln.strip()
                if ln.isdigit():
                    cola.append(int(ln))
                elif ln.startswith(b"#EST"):
                    ESTADO["placa"] = ln.split()[-1].decode(errors="ignore")


def fuente_demo(cola, parar):
    """PQRST sintetico + ruido + zumbido de red, para probar sin hardware."""
    ondas = [(-0.21, 0.13, 0.026), (-0.025, -0.09, 0.008), (0.0, 1.0, 0.011),
             (0.028, -0.22, 0.011), (0.30, 0.30, 0.048)]
    n, rr = 0, 0.92
    import random
    while not parar.is_set():
        t = (n / FS) % rr
        if t > rr / 2:
            t -= rr
        v = sum(a * math.exp(-((t - c) ** 2) / (2 * w * w)) for c, a, w in ondas)
        v += 0.02 * math.sin(2 * math.pi * 60 * n / FS)     # zumbido 60 Hz
        v += random.gauss(0, 0.012)
        v += 0.05 * math.sin(2 * math.pi * 0.25 * n / FS)   # deriva por respiracion
        cola.append(int(2048 + 420 * v))
        n += 1
        time.sleep(1.0 / FS)


# ---------------------------------------------------------------- dibujo
VERDE, ROJO, GRIS, AMAR, RESET = "\x1b[32m", "\x1b[31m", "\x1b[90m", "\x1b[33m", "\x1b[0m"


def dibuja(muestras, picos, ancho, alto):
    """Cada columna = un bucket; se traza de min a max para no perder el QRS."""
    n = len(muestras)
    if n < 2:
        return []
    lo, hi = min(muestras), max(muestras)
    if hi - lo < 1e-9:
        hi = lo + 1.0
    margen = (hi - lo) * 0.08
    lo, hi = lo - margen, hi + margen

    rej = [[" "] * ancho for _ in range(alto)]
    fila = lambda v: min(alto - 1, max(0, int((hi - v) / (hi - lo) * (alto - 1))))

    linea_cero = fila(0.0)
    for x in range(ancho):
        rej[linea_cero][x] = "·"

    picos_set = set(picos)
    for x in range(ancho):
        i0 = x * n // ancho
        i1 = max(i0 + 1, (x + 1) * n // ancho)
        trozo = muestras[i0:i1]
        f_hi, f_lo = fila(max(trozo)), fila(min(trozo))
        es_r = any(i0 <= p < i1 for p in picos_set)
        ch = "█" if not es_r else "▓"
        for y in range(f_hi, f_lo + 1):
            rej[y][x] = ch
        if es_r:
            rej[max(0, f_hi - 1)][x] = "▼"

    salida = []
    for y, f in enumerate(rej):
        s = "".join(f)
        s = s.replace("·", GRIS + "·" + VERDE)
        s = s.replace("▓", ROJO + "▓" + VERDE).replace("▼", ROJO + "▼" + VERDE)
        salida.append(VERDE + s + RESET)
    return salida


# ---------------------------------------------------------------- principal
def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("puerto", nargs="?", default="/dev/cu.usbmodem101")
    ap.add_argument("--demo", action="store_true", help="senal sintetica")
    ap.add_argument("--raw",  action="store_true", help="solo numeros")
    ap.add_argument("--hum",  type=float, default=60.0, help="frecuencia de red")
    ap.add_argument("--frames", type=int, default=0, help="salir tras N cuadros")
    args = ap.parse_args()

    cola  = collections.deque(maxlen=8192)
    parar = threading.Event()

    if args.demo:
        hilo = threading.Thread(target=fuente_demo, args=(cola, parar), daemon=True)
    else:
        if not os.path.exists(args.puerto):
            sys.exit(f"No existe {args.puerto}.  Conecta la placa o pasa el puerto correcto.")
        hilo = threading.Thread(target=fuente_serial,
                                args=(args.puerto, cola, parar), daemon=True)
    hilo.start()

    if args.raw:
        try:
            while True:
                while cola:
                    print(cola.popleft(), flush=True)
                time.sleep(0.01)
        except KeyboardInterrupt:
            parar.set()
        return

    cadena = [highpass(0.5, FS), notch(args.hum, FS), lowpass(40.0, FS)]
    N = SEGUNDOS * FS
    filtradas = collections.deque([0.0] * N, maxlen=N)
    crudas    = collections.deque([0] * FS, maxlen=FS)

    idx, picos, ult_pico, rr = 0, collections.deque(maxlen=32), -1e9, collections.deque(maxlen=8)
    cuadro = 0
    recientes = collections.deque(maxlen=3 * FS)   # ventana para el umbral
    en_pico, pico_val, pico_idx = False, 0.0, 0

    # El pasa-altos tarda en asentarse: su transitorio inicial es un pico
    # gigante que dispararia la deteccion de R y aplastaria la escala.
    # Se descartan las primeras muestras y se arranca el filtro desde el
    # nivel de reposo real en vez de desde cero.
    CALENT = int(1.5 * FS)
    offset = None

    sys.stdout.write("\x1b[?25l")   # ocultar cursor
    try:
        while True:
            while cola:
                v = cola.popleft()
                crudas.append(v)
                if offset is None:
                    offset = float(v)
                y = float(v) - offset
                for f in cadena:
                    y = f(y)

                idx += 1
                if idx <= CALENT:      # descartando transitorio
                    continue
                filtradas.append(y)

                # Deteccion de R.  El umbral sale del maximo de los ultimos
                # 3 s (se adapta rapido; un decaimiento exponencial se queda
                # colgado alto tras un artefacto y se salta latidos).  El pico
                # se registra en el maximo local, no en el primer cruce: asi
                # el intervalo RR no depende de la pendiente de subida.
                recientes.append(y)
                umbral = 0.5 * max(recientes) if len(recientes) > FS // 2 else 1e9

                if y > umbral:
                    if not en_pico:
                        en_pico, pico_val, pico_idx = True, y, idx
                    elif y > pico_val:
                        pico_val, pico_idx = y, idx
                elif en_pico:
                    en_pico = False
                    t = pico_idx / FS
                    if (t - ult_pico) > 0.30:      # refractario fisiologico
                        ult_pico = t
                        picos.append(pico_idx)
                        if len(picos) > 1:
                            d = (picos[-1] - picos[-2]) / FS
                            if 0.3 < d < 2.0:
                                rr.append(d)

            cols, filas = shutil.get_terminal_size((100, 30))
            ancho, alto = max(20, cols - 2), max(8, filas - 5)

            base = idx - len(filtradas)
            vis  = [p - base for p in picos if p >= base]
            m    = list(filtradas)

            despegado = len(crudas) == crudas.maxlen and max(crudas) == 0
            fc = 60.0 / (sum(rr) / len(rr)) if rr else 0.0

            placa = {
                "SIN_MODULO": ROJO + "AD8232 NO DETECTADO — revisa los jumpers" + RESET,
                "SIN_SALIDA": AMAR + "OUTPUT al aire — revisa el cable de GPIO4" + RESET,
                "ESPERANDO":  VERDE + "modulo OK" + RESET,
                "MIDIENDO":   VERDE + "modulo OK" + RESET,
            }.get(ESTADO["placa"], GRIS + "sin reporte de la placa" + RESET)

            cab = (f" ECG  {args.puerto if not args.demo else 'DEMO'}   "
                   f"{FS} Hz   {SEGUNDOS}s   [{placa}{GRIS}] ")
            if ESTADO["placa"] == "SIN_MODULO":
                est = ROJO + "El AD8232 no responde: uno de los jumpers esta suelto" + RESET
            elif idx <= CALENT:
                est = GRIS + "estabilizando el filtro…" + RESET
            elif despegado:
                est = AMAR + "ELECTRODO DESPEGADO — revisa el contacto" + RESET
            elif fc:
                est = f"FC = {fc:5.1f} lpm    RR = {sum(rr)/len(rr):.3f} s    picos R: {len(picos)}"
            else:
                est = GRIS + "esperando latidos…" + RESET

            lineas = [GRIS + cab + RESET, "  " + est, ""]
            lineas += ["  " + l for l in dibuja(m, vis, ancho - 2, alto)]
            lineas.append(GRIS + "  Ctrl+C para salir" + RESET)

            sys.stdout.write("\x1b[H")
            for l in lineas:
                sys.stdout.write("\x1b[2K" + l + "\n")
            sys.stdout.write("\x1b[J")
            sys.stdout.flush()

            cuadro += 1
            if args.frames and cuadro >= args.frames:
                break
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        parar.set()
        sys.stdout.write("\x1b[?25h\n")   # restaurar cursor
        sys.stdout.flush()


if __name__ == "__main__":
    main()
