# Pac-Man per STM32F767ZI (Zephyr RTOS)

Gioco **Pac-Man** in C eseguito su **NUCLEO-F767ZI** con **Zephyr RTOS**, controllabile tramite joystick analogico o tastiera e visualizzato su terminale seriale tramite sequenze ANSI/VT100 a colori.

## Collegamenti Joystick -> NUCLEO-F767ZI

| Pin Joystick | Pin Nucleo | Connettore | Funzione |
| :--- | :--- | :--- | :--- |
| `GND` | `GND` | `CN8` | Massa |
| `+5V` | `3V3` | `CN8` | Alimentazione 3.3 V |
| `VRx` | `A0` (`PA3`) | `CN9` | Asse X (`ADC1_IN3`) |
| `VRy` | `A1` (`PC0`) | `CN9` | Asse Y (`ADC1_IN10`) |
| `SW` | `D2` (`PF15`) | `CN10` | Pausa (pull-up interno) |

## Compilazione e avvio

```bash
west build -p always -b nucleo_f767zi
west flash --runner openocd
```

Collegarsi via seriale a **115200 baud** e lanciare `pacman` (oppure `pacman 1`..`4` per forzare la scala):
- **Joystick / WASD / Frecce**: movimento
- **SW / P / Tasto B1**: pausa
- **Q**: esci
