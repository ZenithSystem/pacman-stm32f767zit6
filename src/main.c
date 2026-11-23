/*
 * PAC-MAN a colori per Zephyr shell (STM32F767 Nucleo)
 *
 * Comando:  pacman
 * Comandi:  Joystick analogico / WASD / frecce = direzione
 *           Premi joystick / P / tasto blu B1 = pausa, Q = esci
 *
 * LED: blu = in gioco, verde = modalita' "fantasmi spaventati", rosso = vita persa
 * Serve un terminale ANSI/VT100 con colori, almeno 40 colonne x 24 righe.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/shell/shell.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Periferiche integrate sulla scheda Nucleo (LED utente e pulsante blu B1)
static const struct gpio_dt_spec led_verde = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_blu = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_rosso = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec pulsante = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

// Canali ADC e pulsante centrale del joystick definiti in app.overlay
static const struct adc_dt_spec joy_x = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
static const struct adc_dt_spec joy_y = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1);
static const struct gpio_dt_spec joy_sw = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), sw_gpios);

// Imposta a 1 se un asse del joystick risulta invertito
#define JOY_INV_X 0
#define JOY_INV_Y 0
// Zona morta intorno al centro (su scala ADC 12 bit: 0..4095)
#define JOY_SOGLIA 650

// Dimensioni della griglia di gioco e numero di fantasmi
#define MW 19
#define MH 21
#define NFANT 4

// Dimensione minima del terminale per avviare il gioco
#define RIGHE_MIN 24
#define COLONNE_MIN 40

// Coordinate chiave del labirinto (spawn Pac-Man, uscita e centro casa fantasmi)
#define PAC_X0 9
#define PAC_Y0 15
#define USCITA_X 9
#define USCITA_Y 7
#define CASA_X 9
#define CASA_Y 9

// Flag degli eventi restituiti dalla lettura input
#define IN_QUIT 1
#define IN_PAUSA 2

// Codice ANSI 256-colori per il nero puro RGB(0,0,0)
#define BG_NERO "48;5;16"

/*
 * Mappa base del labirinto:
 * '#' = muro, '.' = puntino, 'o' = super-pillola,
 * '-' = porta casa fantasmi, 'G' = interno casa, 'P' = partenza Pac-Man
 */
static const char *const mappa_base[MH] = {
	"###################",
	"#........#........#",
	"#o##.###.#.###.##o#",
	"#.................#",
	"#.##.#.#####.#.##.#",
	"#....#...#...#....#",
	"####.###.#.###.####",
	"####.#.......#.####",
	"####.#.##-##.#.####",
	".......#GGG#.......",
	"####.#.#####.#.####",
	"####.#.......#.####",
	"####.#.#####.#.####",
	"#........#........#",
	"#.##.###.#.###.##.#",
	"#o.#.....P.....#.o#",
	"##.#.#.#####.#.#.##",
	"#....#...#...#....#",
	"#.######.#.######.#",
	"#.................#",
	"###################",
};

// Stati possibili della macchina a stati di ciascun fantasma
enum { ST_CASA, ST_ESCE, ST_NORMALE, ST_OCCHI };

struct fantasma {
	int x, y;
	int dx, dy;
	int stato;
	bool spaventato;
	bool inverti;
	int64_t prossimo;
	int64_t rilascio;
};

// Stato globale dell'intera partita
struct gioco {
	const struct shell *sh;
	int righe, colonne, ox, oy;
	int scala;
	bool hud_laterale;
	char mappa[MH][MW + 1];
	int dist_casa[MH][MW];
	char buf[1536];

	int px, py, dx, dy, ndx, ndy;
	bool bocca;
	struct fantasma f[NFANT];

	int punti, record, vite, livello, puntini_rimasti, combo;
	bool extra_vita;

	int64_t pausa_tot;
	int64_t pac_next;
	int per_pac, per_gh, fright_ms;
	int64_t fright_fine;
	int64_t fase_fine;
	int fase_idx;
	bool scatter;
	int64_t t_lamp;
	bool lamp;
	int64_t msg_fine;

	int esc;
	int btn_prec;

	bool joy_ok;
	int joy_cx, joy_cy;
	int joy_btn_prec;
};

static struct gioco G;

// Vettori direzione ordinati: su, sinistra, giu', destra
static const int DX[4] = {0, -1, 0, 1};
static const int DY[4] = {-1, 0, 1, 0};

// Colori ANSI dei quattro fantasmi
static const char *const colori_f[NFANT] = {
	"1;91;" BG_NERO,        // Blinky: rosso
	"1;95;" BG_NERO,        // Pinky: rosa
	"1;96;" BG_NERO,        // Inky: ciano
	"1;38;5;208;" BG_NERO,  // Clyde: arancione
};

// Durata in ms delle fasi alternate Scatter / Chase (0 = infinito)
static const int fasi[] = {7000, 20000, 7000, 20000, 5000, 0};

// Scorciatoia per inviare stringhe formattate sulla shell Zephyr
#define OUT(...) shell_fprintf(G.sh, SHELL_NORMAL, __VA_ARGS__)

static uint32_t rng = 2463534242u;

// Generatore pseudo-casuale Xorshift a 32 bit
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static int iabs(int v)
{
	return (v < 0) ? -v : v;
}

// Restituisce il tempo di gioco effettivo sottraendo i ms trascorsi in pausa
static int64_t ora(void)
{
	return k_uptime_get() - G.pausa_tot;
}

// Sospende l'esecuzione senza far avanzare i timer di gioco
static void attesa(int ms)
{
	k_msleep(ms);
	G.pausa_tot += ms;
}

// Gestisce l'effetto pac-man nel tunnel orizzontale
static int avvolgi(int x)
{
	if (x < 0)
	{
		return x + MW;
	}
	if (x >= MW)
	{
		return x - MW;
	}
	return x;
}

// Scarta eventuali byte residui rimasti nel buffer RX della UART
static void svuota_input(void)
{
	char c;
	size_t letti;

	do
	{
		letti = 0;
		G.sh->iface->api->read(G.sh->iface, &c, 1, &letti);
	}
	while (letti > 0);
}

/*
 * Interroga il terminale tramite sequenza ANSI DSR (\x1b[6n):
 * sposta il cursore all'angolo in basso a destra (999,999) e legge
 * la risposta "\x1b[righe;colonneR" per adattare la scala grafica.
 */
static void rileva_dimensioni_terminale(int *righe, int *colonne)
{
	char buf[32];
	size_t idx = 0;
	char c = 0;
	size_t letti = 0;
	int r = 0;
	int col = 0;
	int fase = 0;
	bool finito = false;

	svuota_input();
	OUT("\x1b[999;999H\x1b[6n");

	for (int t = 0; t < 50 && !finito; t++)
	{
		k_msleep(10);
		while (1)
		{
			letti = 0;
			G.sh->iface->api->read(G.sh->iface, &c, 1, &letti);
			if (letti == 0)
			{
				break;
			}
			if (idx < sizeof(buf) - 1)
			{
				buf[idx++] = c;
				buf[idx] = '\0';
			}
			if (c == 'R')
			{
				finito = true;
				break;
			}
		}
	}

	// Estrae righe e colonne dalla risposta del terminale
	for (size_t i = 0; i < idx; i++)
	{
		if (buf[i] == '[')
		{
			fase = 1;
		}
		else if (fase == 1 && buf[i] >= '0' && buf[i] <= '9')
		{
			r = r * 10 + (buf[i] - '0');
		}
		else if (fase == 1 && buf[i] == ';')
		{
			fase = 2;
		}
		else if (fase == 2 && buf[i] >= '0' && buf[i] <= '9')
		{
			col = col * 10 + (buf[i] - '0');
		}
		else if (fase == 2 && buf[i] == 'R')
		{
			break;
		}
	}

	// Ripiega su 80x24 se il terminale non supporta la query ANSI
	if (finito && r >= 5 && r <= 300 && col >= 20 && col <= 600)
	{
		*righe = r;
		*colonne = col;
	}
	else
	{
		*righe = 24;
		*colonne = 80;
	}
}

// Esegue una singola conversione ADC su un canale specifico
static int leggi_canale_adc(const struct adc_dt_spec *spec, int32_t *val)
{
	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};

	adc_sequence_init_dt(spec, &seq);
	if (adc_read_dt(spec, &seq) < 0)
	{
		return -1;
	}
	*val = raw;
	return 0;
}

/*
 * Media 8 campionamenti a riposo per ricavare il punto neutro (centro)
 * del joystick ed evitare derive dovute alle tolleranze dei potenziometri.
 */
static void calibra_joystick(void)
{
	int32_t sx = 0;
	int32_t sy = 0;
	int32_t vx;
	int32_t vy;
	int ok = 0;

	if (!G.joy_ok)
	{
		return;
	}

	for (int i = 0; i < 8; i++)
	{
		if (leggi_canale_adc(&joy_x, &vx) == 0 &&
		    leggi_canale_adc(&joy_y, &vy) == 0)
		{
			sx += vx;
			sy += vy;
			ok++;
		}
		k_msleep(2);
	}

	// Scarta valori fuori range nel caso la levetta fosse premuta all'avvio
	if (ok > 0)
	{
		int cx = sx / ok;
		int cy = sy / ok;

		G.joy_cx = (cx >= 1200 && cx <= 2900) ? cx : 2048;
		G.joy_cy = (cy >= 1200 && cy <= 2900) ? cy : 2048;
	}
	else
	{
		G.joy_cx = 2048;
		G.joy_cy = 2048;
	}
}

// Copia il labirinto nella matrice attiva e conta i puntini da mangiare
static void carica_mappa(void)
{
	G.puntini_rimasti = 0;
	for (int y = 0; y < MH; y++)
	{
		for (int x = 0; x < MW; x++)
		{
			char c = mappa_base[y][x];
			if (c == 'P')
			{
				c = ' ';
			}
			if (c == '.' || c == 'o')
			{
				G.puntini_rimasti++;
			}
			G.mappa[y][x] = c;
		}
		G.mappa[y][MW] = '\0';
	}
}

/*
 * Precalcola tramite visita in ampiezza (BFS) la distanza minima
 * di ogni casella dalla casa centrale, usata dagli occhi per rientrare.
 */
static void calcola_dist_casa(void)
{
	static int coda[MH * MW];
	int qh = 0;
	int qt = 0;

	for (int y = 0; y < MH; y++)
	{
		for (int x = 0; x < MW; x++)
		{
			G.dist_casa[y][x] = -1;
		}
	}

	G.dist_casa[CASA_Y][CASA_X] = 0;
	coda[qt++] = CASA_Y * MW + CASA_X;

	while (qh < qt)
	{
		int c = coda[qh++];
		int cx = c % MW;
		int cy = c / MW;

		for (int k = 0; k < 4; k++)
		{
			int nx = avvolgi(cx + DX[k]);
			int ny = cy + DY[k];

			if (ny < 0 || ny >= MH || mappa_base[ny][nx] == '#' || G.dist_casa[ny][nx] >= 0)
			{
				continue;
			}
			G.dist_casa[ny][nx] = G.dist_casa[cy][cx] + 1;
			coda[qt++] = ny * MW + nx;
		}
	}
}

// Verifica se Pac-Man puo' entrare nella cella (x, y)
static bool libera_pac(int x, int y)
{
	if (y < 0 || y >= MH)
	{
		return false;
	}
	char t = G.mappa[y][avvolgi(x)];
	return t != '#' && t != '-' && t != 'G';
}

// Verifica se un fantasma puo' attraversare la cella (x, y)
static bool libera_gh(const struct fantasma *f, int x, int y)
{
	if (y < 0 || y >= MH)
	{
		return false;
	}
	char t = G.mappa[y][avvolgi(x)];
	if (t == '#')
	{
		return false;
	}
	// La porta della casa e' attraversabile solo in uscita o da occhi
	if (t == '-' || t == 'G')
	{
		return f->stato != ST_NORMALE;
	}
	return true;
}

static bool muro_a(int x, int y)
{
	if (x < 0 || x >= MW || y < 0 || y >= MH)
	{
		return false;
	}
	return mappa_base[y][x] == '#';
}

/*
 * Calcola il carattere ASCII ('+', '-', '|', ' ') per il sub-pixel (cx, sub)
 * di un blocco muro osservando i blocchi adiacenti.
 */
static char muro_char(int x, int y, int cx, int sub)
{
	int cw = 2 * G.scala;
	int ch = G.scala;
	bool top = (sub == 0);
	bool bot = (sub == ch - 1);
	bool left = (cx == 0);
	bool right = (cx == cw - 1);

	bool up = muro_a(x, y - 1);
	bool dn = muro_a(x, y + 1);
	bool lt = muro_a(x - 1, y);
	bool rt = muro_a(x + 1, y);

	if (top && left)
	{
		if (!up && !lt)
		{
			return '+';
		}
		if (!up)
		{
			return '-';
		}
		if (!lt)
		{
			return '|';
		}
		return muro_a(x - 1, y - 1) ? ' ' : '+';
	}
	if (top && right)
	{
		if (!up && !rt)
		{
			return '+';
		}
		if (!up)
		{
			return '-';
		}
		if (!rt)
		{
			return '|';
		}
		return muro_a(x + 1, y - 1) ? ' ' : '+';
	}
	if (bot && left)
	{
		if (!dn && !lt)
		{
			return '+';
		}
		if (!dn)
		{
			return '-';
		}
		if (!lt)
		{
			return '|';
		}
		return muro_a(x - 1, y + 1) ? ' ' : '+';
	}
	if (bot && right)
	{
		if (!dn && !rt)
		{
			return '+';
		}
		if (!dn)
		{
			return '-';
		}
		if (!rt)
		{
			return '|';
		}
		return muro_a(x + 1, y + 1) ? ' ' : '+';
	}
	if (top)
	{
		return up ? ' ' : '-';
	}
	if (bot)
	{
		return dn ? ' ' : '-';
	}
	if (left)
	{
		return lt ? ' ' : '|';
	}
	if (right)
	{
		return rt ? ' ' : '|';
	}
	return ' ';
}

// Sprite di Pac-Man per i 4 fattori di scala (1x..4x) e orientamento bocca
static const char *sprite_pac_1x(void)
{
	if (!G.bocca)
	{
		return "()";
	}
	if (G.dx > 0)
	{
		return "(<";
	}
	if (G.dx < 0)
	{
		return ">)";
	}
	if (G.dy < 0)
	{
		return "\\/";
	}
	return "/\\";
}

static const char *sprite_pac_2x(int sub)
{
	if (!G.bocca)
	{
		return (sub == 0) ? "/--\\" : "\\--/";
	}
	if (G.dx > 0)
	{
		return (sub == 0) ? "/--<" : "\\--<";
	}
	if (G.dx < 0)
	{
		return (sub == 0) ? ">--\\" : ">--/";
	}
	if (G.dy < 0)
	{
		return (sub == 0) ? "|\\/|" : "\\__/";
	}
	return (sub == 0) ? "/--\\" : "|/\\|";
}

static const char *sprite_pac_3x(int sub)
{
	if (!G.bocca)
	{
		static const char *const s[3] = {" .--. ", "(    )", " '--' "};
		return s[sub];
	}
	if (G.dx > 0)
	{
		static const char *const s[3] = {" .--' ", "(  <  ", " '--. "};
		return s[sub];
	}
	if (G.dx < 0)
	{
		static const char *const s[3] = {" '--. ", "  >  )", " .--' "};
		return s[sub];
	}
	if (G.dy < 0)
	{
		static const char *const s[3] = {"| \\/ |", "(    )", " '--' "};
		return s[sub];
	}
	static const char *const s[3] = {" .--. ", "(    )", "| /\\ |"};
	return s[sub];
}

static const char *sprite_pac_4x(int sub)
{
	if (!G.bocca)
	{
		static const char *const s[4] = {" .----. ", "/      \\", "\\      /", " '----' "};
		return s[sub];
	}
	if (G.dx > 0)
	{
		static const char *const s[4] = {" .----' ", "/   <   ", "\\   <   ", " '----. "};
		return s[sub];
	}
	if (G.dx < 0)
	{
		static const char *const s[4] = {" '----. ", "   >   \\", "   >   /", " .----' "};
		return s[sub];
	}
	if (G.dy < 0)
	{
		static const char *const s[4] = {"| \\  / |", "|  \\/  |", "\\      /", " '----' "};
		return s[sub];
	}
	static const char *const s[4] = {" .----. ", "/      \\", "|  /\\  |", "| /  \\ |"};
	return s[sub];
}

/*
 * Formatta una singola sub-riga verticale della cella (x, y) con i codici
 * colore ANSI, rispettando la priorita' grafica: Pac-Man > Fantasma > Mappa.
 */
static void cella_subriga(int x, int y, int sub, char *out, size_t n)
{
	const char *col = BG_NERO;
	char spazio[12];
	char muro_buf[12];
	const char *txt;
	int cw = 2 * G.scala;
	int gi = -1;

	memset(spazio, ' ', cw);
	spazio[cw] = '\0';
	txt = spazio;

	// Cerca eventuale fantasma presente nella cella
	for (int i = 0; i < NFANT; i++)
	{
		if (G.f[i].x == x && G.f[i].y == y)
		{
			gi = i;
			break;
		}
	}

	if (G.px == x && G.py == y)
	{
		col = "1;93;" BG_NERO;
		if (G.scala == 4)
		{
			txt = sprite_pac_4x(sub);
		}
		else if (G.scala == 3)
		{
			txt = sprite_pac_3x(sub);
		}
		else if (G.scala == 2)
		{
			txt = sprite_pac_2x(sub);
		}
		else
		{
			txt = sprite_pac_1x();
		}
	}
	else if (gi >= 0)
	{
		const struct fantasma *f = &G.f[gi];

		if (f->stato == ST_OCCHI)
		{
			col = "1;97;" BG_NERO;
			if (G.scala == 4)
			{
				txt = (sub == 1) ? "  o  o  " : spazio;
			}
			else if (G.scala == 3)
			{
				txt = (sub == 1) ? " o  o " : spazio;
			}
			else if (G.scala == 2)
			{
				txt = (sub == 0) ? " oo " : spazio;
			}
			else
			{
				txt = "oo";
			}
		}
		else if (f->spaventato)
		{
			// Lampeggia bianco negli ultimi 2 secondi di vulnerabilita'
			bool lampeggia = G.fright_fine && (G.fright_fine - ora()) < 2000 && G.lamp;
			col = lampeggia ? ("1;97;" BG_NERO) : ("1;94;" BG_NERO);
			if (G.scala == 4)
			{
				static const char *const s[4] = {
					" .----. ", "/ ~  ~ \\", "| ---- |", "|/\\/\\/\\|"
				};
				txt = s[sub];
			}
			else if (G.scala == 3)
			{
				static const char *const s[3] = {" .--. ", "| ~~ |", "|/\\/\\|"};
				txt = s[sub];
			}
			else if (G.scala == 2)
			{
				txt = (sub == 0) ? "/\"\"\\" : "/~~\\";
			}
			else
			{
				txt = "MM";
			}
		}
		else
		{
			col = colori_f[gi];
			if (G.scala == 4)
			{
				static const char *const s[4] = {
					" .----. ", "/ o  o \\", "|      |", "|/\\/\\/\\|"
				};
				txt = s[sub];
			}
			else if (G.scala == 3)
			{
				static const char *const s[3] = {" .--. ", "| oo |", "|/\\/\\|"};
				txt = s[sub];
			}
			else if (G.scala == 2)
			{
				txt = (sub == 0) ? "/oo\\" : "|^^|";
			}
			else
			{
				txt = "MM";
			}
		}
	}
	else
	{
		switch (G.mappa[y][x])
		{
		case '#':
			if (G.scala == 1)
			{
				col = "44";
			}
			else
			{
				col = "1;94;" BG_NERO;
				for (int cx = 0; cx < cw; cx++)
				{
					muro_buf[cx] = muro_char(x, y, cx, sub);
				}
				muro_buf[cw] = '\0';
				txt = muro_buf;
			}
			break;
		case '-':
			col = "1;95;" BG_NERO;
			if (G.scala == 4)
			{
				txt = (sub == 1) ? "========" : spazio;
			}
			else if (G.scala == 3)
			{
				txt = (sub == 1) ? "======" : spazio;
			}
			else if (G.scala == 2)
			{
				txt = (sub == 0) ? "====" : spazio;
			}
			else
			{
				txt = "--";
			}
			break;
		case '.':
			col = "37;" BG_NERO;
			if (G.scala == 4)
			{
				txt = (sub == 1) ? "   .    " : spazio;
			}
			else if (G.scala == 3)
			{
				txt = (sub == 1) ? "  .   " : spazio;
			}
			else if (G.scala == 2)
			{
				txt = (sub == 0) ? " .  " : spazio;
			}
			else
			{
				txt = " .";
			}
			break;
		case 'o':
			col = "1;97;" BG_NERO;
			if (!G.lamp)
			{
				break;
			}
			if (G.scala == 4)
			{
				static const char *const s[4] = {
					"  .--.  ", " /    \\ ", " \\    / ", "  '--'  "
				};
				txt = s[sub];
			}
			else if (G.scala == 3)
			{
				static const char *const s[3] = {" .--. ", "(    )", " '--' "};
				txt = s[sub];
			}
			else if (G.scala == 2)
			{
				txt = (sub == 0) ? " /\\ " : " \\/ ";
			}
			else
			{
				txt = " O";
			}
			break;
		default:
			break;
		}
	}

	snprintf(out, n, "\x1b[%sm%s\x1b[0;%sm", col, txt, BG_NERO);
}

// Aggiorna una singola cella (rendering differenziale senza ridisegnare tutto)
static void disegna_cella(int x, int y)
{
	char s[64];
	int cw = 2 * G.scala;

	for (int sub = 0; sub < G.scala; sub++)
	{
		cella_subriga(x, y, sub, s, sizeof(s));
		OUT("\x1b[%d;%dH%s", G.oy + y * G.scala + sub, G.ox + cw * x, s);
	}
}

// Disegna un'intera riga della mappa accumulandola in G.buf per velocizzare la UART
static void disegna_riga(int y)
{
	char s[64];

	for (int sub = 0; sub < G.scala; sub++)
	{
		size_t pos = 0;

		for (int x = 0; x < MW; x++)
		{
			cella_subriga(x, y, sub, s, sizeof(s));
			size_t l = strlen(s);
			if (pos + l < sizeof(G.buf) - 1)
			{
				memcpy(G.buf + pos, s, l);
				pos += l;
			}
		}
		G.buf[pos] = '\0';
		OUT("\x1b[%d;%dH%s", G.oy + y * G.scala + sub, G.ox, G.buf);
	}
}

// Aggiorna punteggio, record e livello (in alto o nella colonna laterale)
static void disegna_header(void)
{
	if (G.punti > G.record)
	{
		G.record = G.punti;
	}

	if (G.hud_laterale)
	{
		int sx = G.ox + MW * 2 * G.scala + 4;

		OUT("\x1b[%d;%dH\x1b[1;91;%sm1UP\x1b[0;%sm\x1b[K", G.oy + 1, sx, BG_NERO, BG_NERO);
		OUT("\x1b[%d;%dH\x1b[1;97;%sm%06d\x1b[0;%sm\x1b[K", G.oy + 2, sx, BG_NERO, G.punti, BG_NERO);
		OUT("\x1b[%d;%dH\x1b[1;91;%smHIGH SCORE\x1b[0;%sm\x1b[K", G.oy + 4, sx, BG_NERO, BG_NERO);
		OUT("\x1b[%d;%dH\x1b[1;96;%sm%06d\x1b[0;%sm\x1b[K", G.oy + 5, sx, BG_NERO, G.record, BG_NERO);
		OUT("\x1b[%d;%dH\x1b[1;93;%smLIVELLO %d\x1b[0;%sm\x1b[K",
		    G.oy + 7, sx, BG_NERO, G.livello, BG_NERO);
		return;
	}

	OUT("\x1b[%d;%dH\x1b[0;%sm\x1b[2K\x1b[1;91;%sm1UP\x1b[0;%sm \x1b[1;97;%sm%06d\x1b[0;%sm  "
	    "\x1b[1;91;%smHIGH SCORE\x1b[0;%sm \x1b[1;96;%sm%06d\x1b[0;%sm  "
	    "\x1b[1;93;%smLIV %d\x1b[0;%sm",
	    G.oy - 1, G.ox, BG_NERO, BG_NERO, BG_NERO, BG_NERO, G.punti, BG_NERO,
	    BG_NERO, BG_NERO, BG_NERO, G.record, BG_NERO, BG_NERO, G.livello, BG_NERO);
}

// Disegna le vite residue e la legenda dei comandi
static void disegna_vite(void)
{
	int map_h = MH * G.scala;

	if (G.hud_laterale)
	{
		int sx = G.ox + MW * 2 * G.scala + 4;

		OUT("\x1b[%d;%dH\x1b[1;97;%smVITE\x1b[0;%sm\x1b[K", G.oy + 9, sx, BG_NERO, BG_NERO);
		OUT("\x1b[%d;%dH\x1b[0;%sm", G.oy + 10, sx, BG_NERO);
		for (int i = 0; i < G.vite; i++)
		{
			OUT("\x1b[1;93;%sm(<\x1b[0;%sm ", BG_NERO, BG_NERO);
		}
		OUT("\x1b[K");
		OUT("\x1b[%d;%dH\x1b[2;%smJOY / WASD\x1b[0;%sm\x1b[K", G.oy + 15, sx, BG_NERO, BG_NERO);
		OUT("\x1b[%d;%dH\x1b[2;%smSW / P = PAUSA\x1b[0;%sm\x1b[K", G.oy + 16, sx, BG_NERO, BG_NERO);
		OUT("\x1b[%d;%dH\x1b[2;%smQ = ESCI\x1b[0;%sm\x1b[K", G.oy + 17, sx, BG_NERO, BG_NERO);
		return;
	}

	OUT("\x1b[%d;%dH\x1b[0;%sm\x1b[2KVITE ", G.oy + map_h + 1, G.ox, BG_NERO);
	for (int i = 0; i < G.vite; i++)
	{
		OUT("\x1b[1;93;%sm(<\x1b[0;%sm ", BG_NERO, BG_NERO);
	}
	OUT(" \x1b[2;%smJOY/WASD P=pausa Q=esci\x1b[0;%sm", BG_NERO, BG_NERO);
}

static void messaggio(const char *colore, const char *testo)
{
	if (G.hud_laterale)
	{
		int sx = G.ox + MW * 2 * G.scala + 4;
		OUT("\x1b[%d;%dH\x1b[%s;%sm%s\x1b[0;%sm\x1b[K",
		    G.oy + 12, sx, colore, BG_NERO, testo, BG_NERO);
		return;
	}

	int map_w = MW * 2 * G.scala;
	int map_h = MH * G.scala;
	int pad = (map_w - (int)strlen(testo)) / 2;

	if (pad < 0)
	{
		pad = 0;
	}
	OUT("\x1b[%d;%dH\x1b[0;%sm\x1b[2K\x1b[%s;%sm%s\x1b[0;%sm",
	    G.oy + map_h, G.ox + pad, BG_NERO, colore, BG_NERO, testo, BG_NERO);
}

static void cancella_messaggio(void)
{
	if (G.hud_laterale)
	{
		int sx = G.ox + MW * 2 * G.scala + 4;
		OUT("\x1b[%d;%dH\x1b[0;%sm\x1b[K", G.oy + 12, sx, BG_NERO);
		return;
	}
	OUT("\x1b[%d;1H\x1b[0;%sm\x1b[2K", G.oy + MH * G.scala, BG_NERO);
}

// Pulisce lo schermo, nasconde il cursore e ridisegna l'intera schermata
static void disegna_tutto(void)
{
	OUT("\x1b]11;#000000\x07\x1b[0;%sm\x1b[2J\x1b[?25l", BG_NERO);
	disegna_header();
	for (int y = 0; y < MH; y++)
	{
		disegna_riga(y);
	}
	disegna_vite();
}

// Aumenta la velocita' e riduce il tempo di paura al salire dei livelli
static void imposta_livello(void)
{
	int l = G.livello - 1;

	G.per_pac = 150 - 6 * l;
	if (G.per_pac < 90)
	{
		G.per_pac = 90;
	}
	G.per_gh = 175 - 8 * l;
	if (G.per_gh < 100)
	{
		G.per_gh = 100;
	}
	G.fright_ms = 7000 - 1000 * l;
	if (G.fright_ms < 2000)
	{
		G.fright_ms = 2000;
	}
}

// Riporta Pac-Man e i fantasmi nelle rispettive posizioni iniziali
static void posiziona_entita(void)
{
	static const int sx[NFANT] = {USCITA_X, 9, 8, 10};
	static const int sy[NFANT] = {USCITA_Y, 9, 9, 9};

	G.px = PAC_X0;
	G.py = PAC_Y0;
	G.dx = -1;
	G.dy = 0;
	G.ndx = 0;
	G.ndy = 0;
	G.bocca = false;

	for (int i = 0; i < NFANT; i++)
	{
		struct fantasma *f = &G.f[i];

		f->x = sx[i];
		f->y = sy[i];
		f->dx = -1;
		f->dy = 0;
		// Blinky parte subito fuori, gli altri tre restano in casa
		f->stato = (i == 0) ? ST_NORMALE : ST_CASA;
		f->spaventato = false;
		f->inverti = false;
	}

	G.fright_fine = 0;
	G.combo = 0;
	gpio_pin_set_dt(&led_verde, 0);
}

static void avvia_timer(void)
{
	static const int rel[NFANT] = {0, 1500, 4500, 8000};
	int64_t now = ora();

	G.pac_next = now + G.per_pac;
	for (int i = 0; i < NFANT; i++)
	{
		G.f[i].prossimo = now + G.per_gh;
		G.f[i].rilascio = now + rel[i];
	}
	G.fase_idx = 0;
	G.scatter = true;
	G.fase_fine = now + fasi[0];
	G.t_lamp = now + 250;
	G.msg_fine = 0;
}

/*
 * Calcola la casella bersaglio (tx, ty) del fantasma i in modalita' Chase:
 * 0 (Blinky) insegue direttamente Pac-Man;
 * 1 (Pinky) punta 4 caselle davanti a Pac-Man;
 * 2 (Inky) triangola la posizione di Pac-Man rispetto a Blinky;
 * 3 (Clyde) insegue se distante piu' di 8 caselle, altrimenti torna all'angolo.
 */
static void bersaglio(int i, int *tx, int *ty)
{
	static const int cx[NFANT] = {MW - 2, 1, MW - 2, 1};
	static const int cy[NFANT] = {0, 0, MH - 1, MH - 1};

	if (G.scatter)
	{
		*tx = cx[i];
		*ty = cy[i];
		return;
	}

	switch (i)
	{
	case 0:
		*tx = G.px;
		*ty = G.py;
		break;
	case 1:
		*tx = G.px + 4 * G.dx;
		*ty = G.py + 4 * G.dy;
		break;
	case 2:
	{
		int ax = G.px + 2 * G.dx;
		int ay = G.py + 2 * G.dy;
		*tx = 2 * ax - G.f[0].x;
		*ty = 2 * ay - G.f[0].y;
		break;
	}
	default:
	{
		int ddx = G.f[3].x - G.px;
		int ddy = G.f[3].y - G.py;
		if (ddx * ddx + ddy * ddy > 64)
		{
			*tx = G.px;
			*ty = G.py;
		}
		else
		{
			*tx = cx[3];
			*ty = cy[3];
		}
		break;
	}
	}
}

// Restituisce l'intervallo in ms tra uno spostamento e il successivo del fantasma
static int periodo_fantasma(int i)
{
	const struct fantasma *f = &G.f[i];
	int p = G.per_gh;

	if (f->stato == ST_OCCHI)
	{
		return 45;
	}
	if (f->spaventato)
	{
		return p * 8 / 5;
	}
	// Rallenta i fantasmi nel tunnel laterale come nell'arcade originale
	if (f->stato == ST_NORMALE && f->y == 9 && (f->x < 4 || f->x > MW - 5))
	{
		return p * 2;
	}
	return p;
}

// Aggiorna posizione e direzione del fantasma i in base al suo stato
static void muovi_fantasma(int i)
{
	struct fantasma *f = &G.f[i];
	int vx = f->x;
	int vy = f->y;

	switch (f->stato)
	{
	case ST_CASA:
		return;

	case ST_ESCE:
		if (f->x != CASA_X)
		{
			f->x += (f->x < CASA_X) ? 1 : -1;
		}
		else if (f->y > USCITA_Y)
		{
			f->y--;
		}
		if (f->x == USCITA_X && f->y == USCITA_Y)
		{
			f->stato = ST_NORMALE;
			f->dx = (rnd() & 1) ? 1 : -1;
			f->dy = 0;
		}
		break;

	case ST_OCCHI:
	{
		// Segue il gradiente decrescente della mappa BFS fino alla base
		int best = G.dist_casa[f->y][f->x];
		int bx = f->x;
		int by = f->y;

		for (int k = 0; k < 4; k++)
		{
			int nx = avvolgi(f->x + DX[k]);
			int ny = f->y + DY[k];

			if (ny < 0 || ny >= MH)
			{
				continue;
			}
			int d = G.dist_casa[ny][nx];
			if (d >= 0 && d < best)
			{
				best = d;
				bx = nx;
				by = ny;
			}
		}
		f->x = bx;
		f->y = by;
		if (best == 0)
		{
			f->stato = ST_ESCE;
			f->spaventato = false;
		}
		break;
	}

	default:
	{
		bool scelto = false;

		if (f->inverti)
		{
			f->inverti = false;
			if (libera_gh(f, f->x - f->dx, f->y - f->dy))
			{
				f->dx = -f->dx;
				f->dy = -f->dy;
				scelto = true;
			}
		}

		if (!scelto)
		{
			int tx;
			int ty;
			int cand[4];
			int nc = 0;
			int bk = -1;
			int best = 0x7fffffff;

			bersaglio(i, &tx, &ty);

			// Valuta le 3 direzioni consentite (esclude l'inversione a U)
			for (int k = 0; k < 4; k++)
			{
				if (DX[k] == -f->dx && DY[k] == -f->dy)
				{
					continue;
				}
				int nx = f->x + DX[k];
				int ny = f->y + DY[k];

				if (!libera_gh(f, nx, ny))
				{
					continue;
				}
				cand[nc++] = k;
				int d = (nx - tx) * (nx - tx) + (ny - ty) * (ny - ty);
				if (d < best)
				{
					best = d;
					bk = k;
				}
			}

			// Quando sono spaventati scelgono una svolta casuale agli incroci
			if (f->spaventato && nc > 0)
			{
				bk = cand[rnd() % nc];
			}

			// Torna indietro solo se si trova in un vicolo cieco
			if (bk < 0)
			{
				for (int k = 0; k < 4; k++)
				{
					if (DX[k] == -f->dx && DY[k] == -f->dy &&
					    libera_gh(f, f->x + DX[k], f->y + DY[k]))
					{
						bk = k;
					}
				}
			}

			if (bk >= 0)
			{
				f->dx = DX[bk];
				f->dy = DY[bk];
			}
		}

		if (libera_gh(f, f->x + f->dx, f->y + f->dy))
		{
			f->x = avvolgi(f->x + f->dx);
			f->y += f->dy;
		}
		break;
	}
	}

	if (f->x != vx || f->y != vy)
	{
		disegna_cella(vx, vy);
		disegna_cella(f->x, f->y);
	}
}

// Attiva la modalita' vulnerabile dei fantasmi dopo aver mangiato una super-pillola
static void attiva_paura(void)
{
	G.fright_fine = ora() + G.fright_ms;
	G.combo = 0;
	if (G.fase_fine)
	{
		G.fase_fine += G.fright_ms;
	}
	for (int i = 0; i < NFANT; i++)
	{
		struct fantasma *f = &G.f[i];

		if (f->stato == ST_OCCHI)
		{
			continue;
		}
		if (!f->spaventato && f->stato == ST_NORMALE)
		{
			f->inverti = true;
		}
		f->spaventato = true;
		disegna_cella(f->x, f->y);
	}
	gpio_pin_set_dt(&led_verde, 1);
}

// Gestisce la raccolta di puntini, super-pillole e vita extra a 10000 punti
static void mangia(void)
{
	char t = G.mappa[G.py][G.px];

	if (t == '.')
	{
		G.punti += 10;
	}
	else if (t == 'o')
	{
		G.punti += 50;
	}
	else
	{
		return;
	}

	G.mappa[G.py][G.px] = ' ';
	G.puntini_rimasti--;

	if (t == 'o')
	{
		attiva_paura();
	}

	if (!G.extra_vita && G.punti >= 10000)
	{
		G.extra_vita = true;
		G.vite++;
		disegna_vite();
	}
	disegna_header();
}

// Controlla le collisioni tra Pac-Man e i fantasmi (restituisce true se Pac-Man muore)
static bool controlla_collisioni(void)
{
	for (int i = 0; i < NFANT; i++)
	{
		struct fantasma *f = &G.f[i];

		if (f->x != G.px || f->y != G.py || f->stato == ST_OCCHI)
		{
			continue;
		}

		if (f->spaventato)
		{
			char t[24];

			// Bonus progressivo: 200, 400, 800, 1600 punti
			G.combo++;
			int p = 100 << G.combo;
			G.punti += p;
			f->spaventato = false;
			f->stato = ST_OCCHI;
			snprintf(t, sizeof(t), "+%d", p);
			messaggio("1;96", t);
			G.msg_fine = ora() + 1000;
			disegna_header();
		}
		else
		{
			return true;
		}
	}
	return false;
}

// Muove Pac-Man applicando il pre-turn (svolta memorizzata in ndx/ndy)
static bool muovi_pac(void)
{
	int vx = G.px;
	int vy = G.py;
	bool mosso = false;

	if ((G.ndx || G.ndy) && libera_pac(G.px + G.ndx, G.py + G.ndy))
	{
		G.dx = G.ndx;
		G.dy = G.ndy;
	}

	if (libera_pac(G.px + G.dx, G.py + G.dy))
	{
		G.px = avvolgi(G.px + G.dx);
		G.py += G.dy;
		mosso = true;
	}

	if (mosso)
	{
		G.bocca = !G.bocca;
		disegna_cella(vx, vy);
		mangia();
		disegna_cella(G.px, G.py);
	}

	return controlla_collisioni();
}

static void imposta_dir(int dx, int dy)
{
	G.ndx = dx;
	G.ndy = dy;
}

// Legge gli assi analogici X/Y del joystick e imposta la direzione prevalente
static void leggi_joystick(void)
{
	int32_t vx;
	int32_t vy;

	if (!G.joy_ok)
	{
		return;
	}
	if (leggi_canale_adc(&joy_x, &vx) < 0 || leggi_canale_adc(&joy_y, &vy) < 0)
	{
		return;
	}

	int dx = vx - G.joy_cx;
	int dy = vy - G.joy_cy;

	if (JOY_INV_X)
	{
		dx = -dx;
	}
	if (JOY_INV_Y)
	{
		dy = -dy;
	}

	// Ignora i piccoli spostamenti dentro la zona morta centrale
	if (iabs(dx) < JOY_SOGLIA && iabs(dy) < JOY_SOGLIA)
	{
		return;
	}

	if (iabs(dx) >= iabs(dy))
	{
		imposta_dir((dx > 0) ? 1 : -1, 0);
	}
	else
	{
		imposta_dir(0, (dy > 0) ? 1 : -1);
	}
}

// Legge sia il joystick che eventuali tasti o sequenze frecce ESC [ A/B/C/D da UART
static int leggi_input(void)
{
	int ev = 0;
	char c;
	size_t letti;

	leggi_joystick();

	while (1)
	{
		letti = 0;
		G.sh->iface->api->read(G.sh->iface, &c, 1, &letti);
		if (letti == 0)
		{
			break;
		}

		if (c == 27)
		{
			G.esc = 1;
			continue;
		}
		if (G.esc == 1)
		{
			if (c == '[' || c == 'O')
			{
				G.esc = 2;
				continue;
			}
			G.esc = 0;
		}
		else if (G.esc == 2)
		{
			G.esc = 0;
			switch (c)
			{
			case 'A':
				imposta_dir(0, -1);
				break;
			case 'B':
				imposta_dir(0, 1);
				break;
			case 'C':
				imposta_dir(1, 0);
				break;
			case 'D':
				imposta_dir(-1, 0);
				break;
			default:
				break;
			}
			continue;
		}

		switch (c)
		{
		case 'w':
		case 'W':
			imposta_dir(0, -1);
			break;
		case 's':
		case 'S':
			imposta_dir(0, 1);
			break;
		case 'a':
		case 'A':
			imposta_dir(-1, 0);
			break;
		case 'd':
		case 'D':
			imposta_dir(1, 0);
			break;
		case 'q':
		case 'Q':
			ev |= IN_QUIT;
			break;
		case 'p':
		case 'P':
			ev |= IN_PAUSA;
			break;
		default:
			break;
		}
	}
	return ev;
}

// Rileva il fronte di salita del tasto B1 sulla scheda o del pulsante SW del joystick
static bool pulsante_premuto(void)
{
	int b = gpio_pin_get_dt(&pulsante);
	bool fronte = (b > 0 && G.btn_prec == 0);

	G.btn_prec = (b > 0) ? 1 : 0;

	if (G.joy_ok)
	{
		int jb = gpio_pin_get_dt(&joy_sw);
		if (jb > 0 && G.joy_btn_prec == 0)
		{
			fronte = true;
		}
		G.joy_btn_prec = (jb > 0) ? 1 : 0;
	}

	return fronte;
}

// Congela il gioco finche' non viene ripremuto P/SW/B1 oppure Q per uscire
static bool pausa(void)
{
	int64_t inizio = k_uptime_get();
	bool esci = false;

	messaggio("1;93", "PAUSA");
	while (1)
	{
		k_msleep(30);
		int ev = leggi_input();
		if (pulsante_premuto())
		{
			ev |= IN_PAUSA;
		}
		if (ev & IN_QUIT)
		{
			esci = true;
			break;
		}
		if (ev & IN_PAUSA)
		{
			break;
		}
	}
	G.pausa_tot += k_uptime_get() - inizio;
	cancella_messaggio();
	return esci;
}

static void animazione_morte(void)
{
	static const char *const fr[] = {"(<", "\\/", "()", "--", "  "};
	char vuoto[12];
	int cw = 2 * G.scala;
	int pad = (cw - 2) / 2;

	memset(vuoto, ' ', cw);
	vuoto[cw] = '\0';

	gpio_pin_set_dt(&led_verde, 0);
	gpio_pin_set_dt(&led_rosso, 1);
	for (int i = 0; i < 5; i++)
	{
		for (int sub = 0; sub < G.scala; sub++)
		{
			OUT("\x1b[%d;%dH\x1b[%sm%s\x1b[0;%sm",
			    G.oy + G.py * G.scala + sub, G.ox + cw * G.px, BG_NERO, vuoto, BG_NERO);
		}
		OUT("\x1b[%d;%dH\x1b[1;93;%sm%s\x1b[0;%sm",
		    G.oy + G.py * G.scala + (G.scala / 2), G.ox + cw * G.px + pad,
		    BG_NERO, fr[i], BG_NERO);
		attesa(220);
	}
}

static void riposiziona_dopo_morte(void)
{
	int vx[NFANT + 1];
	int vy[NFANT + 1];

	vx[0] = G.px;
	vy[0] = G.py;
	for (int i = 0; i < NFANT; i++)
	{
		vx[i + 1] = G.f[i].x;
		vy[i + 1] = G.f[i].y;
	}

	posiziona_entita();

	for (int k = 0; k <= NFANT; k++)
	{
		disegna_cella(vx[k], vy[k]);
	}
	disegna_cella(G.px, G.py);
	for (int i = 0; i < NFANT; i++)
	{
		disegna_cella(G.f[i].x, G.f[i].y);
	}
}

static void pronto(void)
{
	messaggio("1;93", "PRONTO!");
	attesa(1500);
	cancella_messaggio();
	svuota_input();
	G.esc = 0;
	avvia_timer();
}

static int cmd_pacman(const struct shell *sh, size_t argc, char **argv)
{
	int righe = 24;
	int colonne = 80;
	int scala_forzata = 0;
	bool game_over = false;

	// Consente di forzare manualmente la scala passando "pacman 1..4"
	if (argc >= 2 && argv[1][0] >= '1' && argv[1][0] <= '4' && argv[1][1] == '\0')
	{
		scala_forzata = argv[1][0] - '0';
	}

	G.sh = sh;
	G.pausa_tot = 0;
	rng ^= k_cycle_get_32();
	if (rng == 0)
	{
		rng = 1;
	}

	rileva_dimensioni_terminale(&righe, &colonne);

	// Selezione automatica della scala grafica massima compatibile con la finestra
	if (scala_forzata > 0)
	{
		G.scala = scala_forzata;
	}
	else
	{
		if (righe < RIGHE_MIN || colonne < COLONNE_MIN)
		{
			shell_fprintf(sh, SHELL_NORMAL,
				      "\x1b[0m\r\nTerminale troppo piccolo (%dx%d): servono almeno %dx%d.\r\n"
				      "Ingrandisci la finestra e riprova.\r\n",
				      colonne, righe, COLONNE_MIN, RIGHE_MIN);
			return 0;
		}

		G.scala = 1;
		for (int s = 4; s >= 2; s--)
		{
			int mw = MW * 2 * s;
			int mh = MH * s;

			if ((colonne >= mw && righe >= mh + 3) ||
			    (colonne >= mw + 24 && righe >= mh))
			{
				G.scala = s;
				break;
			}
		}
	}

	int map_w = MW * 2 * G.scala;
	int map_h = MH * G.scala;

	// Posiziona l'HUD a destra della mappa se c'e' abbastanza larghezza
	G.hud_laterale = (G.scala >= 2 && colonne >= map_w + 24 && righe < map_h + 3);
	if (!G.hud_laterale && G.scala >= 3 && colonne >= map_w + 28)
	{
		G.hud_laterale = true;
	}

	if (righe < map_h + (G.hud_laterale ? 0 : 3))
	{
		righe = map_h + (G.hud_laterale ? 0 : 3);
	}
	if (colonne < map_w + (G.hud_laterale ? 24 : 2))
	{
		colonne = map_w + (G.hud_laterale ? 24 : 2);
	}

	// Centra l'area di gioco nella finestra del terminale
	G.righe = righe;
	G.colonne = colonne;
	if (G.hud_laterale)
	{
		G.ox = (colonne - (map_w + 22)) / 2 + 1;
		G.oy = (righe - map_h) / 2 + 1;
	}
	else
	{
		G.ox = (colonne - map_w) / 2 + 1;
		G.oy = (righe - (map_h + 3)) / 2 + 2;
	}
	if (G.ox < 1)
	{
		G.ox = 1;
	}
	if (G.oy < (G.hud_laterale ? 1 : 2))
	{
		G.oy = G.hud_laterale ? 1 : 2;
	}

	G.punti = 0;
	G.vite = 3;
	G.livello = 1;
	G.extra_vita = false;
	G.lamp = true;
	G.esc = 0;
	G.btn_prec = (gpio_pin_get_dt(&pulsante) > 0) ? 1 : 0;
	G.joy_btn_prec = (G.joy_ok && gpio_pin_get_dt(&joy_sw) > 0) ? 1 : 0;

	calibra_joystick();
	calcola_dist_casa();
	imposta_livello();
	carica_mappa();
	posiziona_entita();

	gpio_pin_set_dt(&led_blu, 1);
	gpio_pin_set_dt(&led_rosso, 0);

	disegna_tutto();
	pronto();

	// Ciclo principale non bloccante temporizzato tramite k_uptime_get()
	while (1)
	{
		int ev = leggi_input();

		if (pulsante_premuto())
		{
			ev |= IN_PAUSA;
		}
		if (ev & IN_QUIT)
		{
			break;
		}
		if ((ev & IN_PAUSA) && pausa())
		{
			break;
		}

		int64_t now = ora();
		bool morto = false;

		if (now >= G.pac_next)
		{
			G.pac_next = now + G.per_pac;
			morto = muovi_pac();
		}

		for (int i = 0; i < NFANT && !morto; i++)
		{
			struct fantasma *f = &G.f[i];

			if (f->stato == ST_CASA)
			{
				if (now >= f->rilascio)
				{
					f->stato = ST_ESCE;
				}
				continue;
			}
			if (now >= f->prossimo)
			{
				muovi_fantasma(i);
				f->prossimo = now + periodo_fantasma(i);
				if (controlla_collisioni())
				{
					morto = true;
				}
			}
		}

		if (morto)
		{
			animazione_morte();
			G.vite--;
			disegna_vite();
			if (G.vite <= 0)
			{
				game_over = true;
				break;
			}
			riposiziona_dopo_morte();
			gpio_pin_set_dt(&led_rosso, 0);
			pronto();
			continue;
		}

		// Fine effetto super-pillola
		if (G.fright_fine && now >= G.fright_fine)
		{
			G.fright_fine = 0;
			for (int i = 0; i < NFANT; i++)
			{
				if (G.f[i].spaventato)
				{
					G.f[i].spaventato = false;
					disegna_cella(G.f[i].x, G.f[i].y);
				}
			}
			gpio_pin_set_dt(&led_verde, 0);
		}

		// Cambio fase IA tra Scatter (dispersione) e Chase (inseguimento)
		if (G.fase_fine && now >= G.fase_fine)
		{
			if (G.fase_idx < 5)
			{
				G.fase_idx++;
			}
			G.scatter = (G.fase_idx % 2 == 0);
			G.fase_fine = fasi[G.fase_idx] ? now + fasi[G.fase_idx] : 0;
			for (int i = 0; i < NFANT; i++)
			{
				if (G.f[i].stato == ST_NORMALE)
				{
					G.f[i].inverti = true;
				}
			}
		}

		// Lampeggio super-pillole e fantasmi blu ogni 250 ms
		if (now >= G.t_lamp)
		{
			G.t_lamp = now + 250;
			G.lamp = !G.lamp;
			for (int y = 0; y < MH; y++)
			{
				for (int x = 0; x < MW; x++)
				{
					if (G.mappa[y][x] == 'o')
					{
						disegna_cella(x, y);
					}
				}
			}
			if (G.fright_fine)
			{
				for (int i = 0; i < NFANT; i++)
				{
					if (G.f[i].spaventato)
					{
						disegna_cella(G.f[i].x, G.f[i].y);
					}
				}
			}
		}

		if (G.msg_fine && now >= G.msg_fine)
		{
			G.msg_fine = 0;
			cancella_messaggio();
		}

		// Passaggio al livello successivo una volta raccolti tutti i puntini
		if (G.puntini_rimasti <= 0)
		{
			messaggio("1;92", "LIVELLO COMPLETATO!");
			gpio_pin_set_dt(&led_verde, 1);
			attesa(2000);
			gpio_pin_set_dt(&led_verde, 0);

			G.livello++;
			imposta_livello();
			carica_mappa();
			posiziona_entita();
			disegna_tutto();
			pronto();
			continue;
		}

		k_msleep(5);
	}

	gpio_pin_set_dt(&led_blu, 0);
	gpio_pin_set_dt(&led_verde, 0);

	if (game_over)
	{
		gpio_pin_set_dt(&led_rosso, 1);
		messaggio("1;91", "GAME OVER");
		attesa(2500);
	}
	else
	{
		gpio_pin_set_dt(&led_rosso, 0);
	}

	// Ripristina i colori standard del terminale, mostra il cursore e stampa il riepilogo
	svuota_input();
	OUT("\x1b]111\x07\x1b[0m\x1b[?25h\x1b[%d;1H\r\n%s Punteggio finale: %d  (record: %d)\r\n",
	    G.righe, game_over ? "GAME OVER." : "Partita interrotta.", G.punti, G.record);

	return 0;
}

// Registra il comando "pacman" nella shell Zephyr
SHELL_CMD_REGISTER(pacman, NULL, "Avvia Pac-Man a colori", cmd_pacman);

int main(void)
{
	if (!gpio_is_ready_dt(&led_verde) || !gpio_is_ready_dt(&led_blu) ||
	    !gpio_is_ready_dt(&led_rosso) || !gpio_is_ready_dt(&pulsante))
	{
		return 0;
	}

	gpio_pin_configure_dt(&led_verde, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_blu, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_rosso, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&pulsante, GPIO_INPUT);

	// Inizializza ADC e pulsante del joystick; se assenti il gioco resta giocabile da tastiera
	if (adc_is_ready_dt(&joy_x) && adc_is_ready_dt(&joy_y) &&
	    gpio_is_ready_dt(&joy_sw) &&
	    adc_channel_setup_dt(&joy_x) == 0 &&
	    adc_channel_setup_dt(&joy_y) == 0 &&
	    gpio_pin_configure_dt(&joy_sw, GPIO_INPUT) == 0)
	{
		G.joy_ok = true;
	}

	return 0;
}