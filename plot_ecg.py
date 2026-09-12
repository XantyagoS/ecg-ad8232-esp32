#!/usr/bin/env python3
"""
Visor de ECG en tiempo real para AD8232 + Arduino.

  pip install pyserial matplotlib numpy scipy
  python3 plot_ecg.py /dev/cu.usbserial-XXXX

Muestra la senal filtrada (0.5-40 Hz + notch 60 Hz) para que P, QRS y T
se vean limpios, marca los picos R y calcula la frecuencia cardiaca.
Guarda todo en ecg.csv al cerrar la ventana.
"""
import sys, csv, collections
import numpy as np
import serial
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
from scipy.signal import butter, iirnotch, filtfilt, find_peaks

PUERTO = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem101"
BAUD   = 115200
FS     = 250          # debe coincidir con el sketch
VENTANA = 5 * FS      # 5 segundos en pantalla
F_RED  = 60.0         # 60 Hz en Colombia; usa 50.0 en Europa

bp_b, bp_a = butter(2, [0.5 / (FS / 2), 40.0 / (FS / 2)], btype="band")
nt_b, nt_a = iirnotch(F_RED / (FS / 2), Q=30)

buf = collections.deque([0.0] * VENTANA, maxlen=VENTANA)
todo = []
ser = serial.Serial(PUERTO, BAUD, timeout=1)
ser.reset_input_buffer()

fig, ax = plt.subplots(figsize=(11, 4))
t = np.arange(VENTANA) / FS
(linea,) = ax.plot(t, np.zeros(VENTANA), lw=1.2)
(marcas,) = ax.plot([], [], "r.", ms=9)
ax.set_xlabel("tiempo (s)"); ax.set_ylabel("ECG (u.a.)")
ax.set_title("ECG - AD8232"); ax.grid(alpha=0.3)
txt = ax.text(0.01, 0.95, "", transform=ax.transAxes, va="top")


def filtra(x):
    x = x - np.mean(x)
    return filtfilt(bp_b, bp_a, filtfilt(nt_b, nt_a, x))


def actualiza(_):
    n = ser.in_waiting
    for _ in range(max(1, n // 5)):
        linea_raw = ser.readline().decode(errors="ignore").strip()
        if linea_raw.isdigit():
            v = float(linea_raw)
            buf.append(v)
            todo.append(v)

    y = filtra(np.array(buf))
    linea.set_ydata(y)
    ax.set_ylim(y.min() - 20, y.max() + 20)

    # picos R: separacion minima 0.35 s (=171 lpm) y altura por percentil
    umbral = np.percentile(y, 98) * 0.6
    picos, _ = find_peaks(y, height=umbral, distance=int(0.35 * FS))
    marcas.set_data(picos / FS, y[picos])

    if len(picos) > 1:
        rr = np.diff(picos) / FS
        txt.set_text(f"FC = {60 / np.mean(rr):5.1f} lpm    RR = {np.mean(rr):.3f} s")
    return linea, marcas, txt


ani = FuncAnimation(fig, actualiza, interval=40, blit=False, cache_frame_data=False)
plt.tight_layout()
plt.show()

ser.close()
with open("ecg.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["t_s", "adc"])
    for i, v in enumerate(todo):
        w.writerow([i / FS, v])
print(f"Guardadas {len(todo)} muestras en ecg.csv")
