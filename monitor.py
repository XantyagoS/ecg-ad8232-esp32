#!/usr/bin/env python3
"""
Monitor serie simple: imprime lo que mande la placa, tal cual.
Pensado para el firmware que reporta una linea cada 5 segundos.

  python3 monitor.py                       # /dev/cu.usbmodem101
  python3 monitor.py /dev/cu.usbmodemXXX

Ctrl+C para salir.
"""
import sys, time, subprocess

PUERTO = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem101"
BAUD = 115200


def abre():
    """pyserial si esta disponible; si no, stty + open normal."""
    try:
        import serial
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = PUERTO, BAUD, 1
        # No tocar DTR/RTS: en el USB nativo del ESP32-S3 esas lineas
        # pueden tirar el chip a modo bootloader y dejarlo mudo.
        s.dtr = s.rts = False
        s.open()
        return s.readline
    except ImportError:
        subprocess.run(["stty", "-f", PUERTO, str(BAUD), "raw", "-echo"],
                       check=True, capture_output=True)
        f = open(PUERTO, "rb", buffering=0)
        pendiente = bytearray()

        def leer():
            while b"\n" not in pendiente:
                trozo = f.read(64)
                if not trozo:
                    return b""
                pendiente.extend(trozo)
            i = pendiente.index(b"\n")
            ln = bytes(pendiente[:i + 1])
            del pendiente[:i + 1]
            return ln
        return leer


def main():
    try:
        leer = abre()
    except Exception as e:
        sys.exit(f"No se pudo abrir {PUERTO}: {e}\n"
                 f"Revisa que la placa este conectada (ls /dev/cu.*).")

    print(f"Escuchando {PUERTO} a {BAUD} baudios.  Ctrl+C para salir.\n")
    try:
        while True:
            ln = leer()
            if ln:
                sys.stdout.write(ln.decode(errors="replace"))
                sys.stdout.flush()
            else:
                time.sleep(0.05)
    except KeyboardInterrupt:
        print("\nfin")


if __name__ == "__main__":
    main()
