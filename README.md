# Pac-Man per STM32F767ZIT6 (Zephyr RTOS)

Implementazione completa del gioco **Pac-Man** in linguaggio C eseguita interamente sul microcontrollore **STMicroelectronics STM32F767ZIT6** (scheda **NUCLEO-F767ZI**, core ARM Cortex-M7 @ 216 MHz) con **Zephyr RTOS**.

Tutta la logica di gioco, la lettura dei sensori analogici/digitali, l'intelligenza artificiale dei fantasmi e la generazione dei frame grafici avvengono sul microcontrollore. Il computer collegato via USB svolge esclusivamente il ruolo di terminale seriale **ANSI/VT100** (a 115200 baud) per la visualizzazione a colori e l'eventuale input da tastiera.

## Caratteristiche principali

- **Motore grafico ANSI/VT100 a 256 colori:**
  - Rilevamento automatico delle dimensioni della finestra del terminale tramite sequenza DSR (`\x1b[6n`) e selezione dinamica della scala grafica da **`1x`** (minimo 40x24 caratteri) fino a **`4x`** (ideale per schermi Full HD a schermo intero).
  - Layout adattivo dell'interfaccia (HUD sopra/sotto il labirinto su finestre strette, oppure colonna laterale dedicata su schermi ampi).
  - Rendering differenziale per singola cella (`disegna_cella`) e buffering di riga per eliminare lo sfarfallio (*flickering*) e ridurre il traffico sulla UART.
- **Supporto Joystick analogico con auto-calibrazione:**
  - Acquisizione degli assi X e Y tramite **ADC1 a 12 bit** (`0..4095`).
  - Calibrazione automatica del punto neutro all'avvio della partita (media su 8 campionamenti a riposo) e gestione della zona morta (*deadband* di +/-650 step) per evitare movimenti involontari.
  - Bufferizzazione della svolta (*pre-turn*) per cambiare direzione in modo fluido agli incroci.
- **Intelligenza Artificiale dei fantasmi:**
  - Alternanza temporizzata tra fasi **Scatter** (dispersione agli angoli) e **Chase** (inseguimento).
  - Personalita' distinte per ciascun fantasma: **Blinky** (inseguimento diretto), **Pinky** (imboscata 4 caselle davanti a Pac-Man), **Inky** (triangolazione vettoriale rispetto a Blinky) e **Clyde** (inseguimento a distanza, ritirata entro il raggio di 8 caselle).
  - Rientro rapido alla base (`ST_OCCHI`) guidato da una mappa delle distanze minime precalcolata tramite visita in ampiezza (**BFS**).
  - Rallentamento nel tunnel laterale e punteggi combo progressivi (`200`, `400`, `800`, `1600`) in modalita' vulnerabile.

## Hardware e collegamenti

### Joystick analogico -> NUCLEO-F767ZI

> **Nota elettrica:** gli ingressi analogici dell'STM32F767 operano a **3.3 V** e non sono *5V-tolerant* in modalita' ADC. Alimentare il modulo joystick dal pin **`3V3`** della scheda (non `5V`).

| Pin Joystick | Pin Nucleo | GPIO / Canale STM32 | Connettore | Funzione |
| :--- | :--- | :--- | :--- | :--- |
| `GND` | `GND` | — | `CN8` | Massa di riferimento |
| `+5V` / `VCC` | `3V3` | — | `CN8` | Alimentazione 3.3 V |
| `VRx` | `A0` | `PA3` (`ADC1_IN3`) | `CN9` | Asse orizzontale X |
| `VRy` | `A1` | `PC0` (`ADC1_IN10`) | `CN9` | Asse verticale Y |
| `SW` | `D2` | `PF15` (Pull-up attivo) | `CN10` | Pulsante Pausa |

### Periferiche integrate sulla scheda Nucleo

- **LED Blu (`LD2` / `led1`):** acceso durante la partita in corso.
- **LED Verde (`LD1` / `led0`):** acceso mentre i fantasmi sono vulnerabili (effetto super-pillola) e al completamento di un livello.
- **LED Rosso (`LD3` / `led2`):** acceso durante l'animazione di perdita vita e nella schermata di *Game Over*.
- **Pulsante Utente Blu (`B1` / `sw0`):** mette in pausa o riprende la partita.

## Struttura del progetto

    pacman/
    ├── CMakeLists.txt      # Configurazione build CMake/Zephyr
    ├── prj.conf            # Abilitazione driver GPIO, ADC e sottosistema Shell
    ├── app.overlay         # Configurazione Devicetree per ADC1 (PA3, PC0) e GPIO (PF15)
    └── src/
        └── main.c          # Logica di gioco, IA fantasmi, driver input e renderer ANSI

## Compilazione e avvio

1. **Compilazione pulita (Pristine Build) e flash sulla scheda:**

       west build -p always -b nucleo_f767zi
       west flash --runner openocd

2. **Connessione al terminale seriale:**
   Aprire un emulatore di terminale con supporto ANSI/VT100 a **115200 baud** sulla porta seriale della ST-Link (es. `/dev/ttyACM0` su Linux, `/dev/cu.usbmodem*` su macOS o `COMx` su Windows), ingrandire la finestra e premere `Invio` per visualizzare il prompt `uart:~$`.

3. **Avvio del gioco:**
   Tenendo la levetta del joystick al centro durante il primo istante per consentire la calibrazione, digitare:

       pacman

   *(Opzionale: e' possibile forzare manualmente il fattore di ingrandimento con `pacman 1`, `pacman 2`, `pacman 3` o `pacman 4`).*

## Comandi di gioco

- **Movimento:** Joystick analogico, tasti `W` `A` `S` `D` oppure **Frecce direzionali**
- **Pausa / Riprendi:** Pressione levetta joystick (`SW`), tasto blu `B1` sulla scheda oppure tasto `P`
- **Uscita:** Tasto `Q`

*(Se un asse del joystick risultasse invertito rispetto all'orientamento fisico del modulo, impostare a `1` le macro `JOY_INV_X` o `JOY_INV_Y` in cima a `src/main.c`).*
