/* main.c -- the PicoCalc picture viewer.
 *
 * Pick a picture from the SD card; it is shown fitted to the screen.
 * Left/Right (or PgUp/PgDn, Space, Backspace) step through the folder's
 * pictures, ? shows the keys, Home/End go to the first/last, Tab jumps ten ahead, r rotates
 * the picture 90 degrees clockwise (pictures with a camera orientation tag stand
 * up by themselves), i shows the name and size, + and - change the brightness, s
 * starts a slide show (any key stops it), Esc goes back to the list, and Esc
 * in the list returns to the shell.
 *
 * Settings, all optional, in /viewer/viewer.cfg, one per line:
 *   delay = 10        seconds each picture stays in a slide show
 *   order = shuffle   or "name", the order of the list
 *   folder = /pictures/Christmas   where the viewer starts (and autostart's
 *                     slide show); /pictures if not given
 *   autostart = yes   start a slide show of that folder at once (picture frame)
 *   speed = normal    run at the RP2350's rated 150 MHz instead of 250
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "drivers/lcd.h"
#include "drivers/keyboard.h"
#include "drivers/southbridge.h"
#include "drivers/fat32.h"
#include "drivers/sdcard.h"
#include "picocalc_shell.h"
#include "format.h"
#include "canvas.h"
#include "picker.h"
#include "banner.h"

volatile bool user_interrupt;		/* the keyboard driver sets it on Brk */

#define W 320
#define H 320

/* ------------------------------------------------------------ files */

/* Reads go through a 16 KB buffer: the FAT32 driver walks the file's
   cluster chain from the start on every fat32_read(), so a few big reads
   are much faster than many small ones. */
#define SRC_BUF 16384
static uint8_t src_buf[SRC_BUF];

struct img_src {
	fat32_file_t f;
	uint32_t pos, size;		/* where the reader is; the file's size */
	uint32_t buf_at, buf_len;	/* what src_buf holds */
};

static bool fill(img_src_t *s)
{
	uint32_t at = s->pos;
	if (fat32_seek(&s->f, at) != FAT32_OK) return false;
	size_t got = 0;
	if (fat32_read(&s->f, src_buf, SRC_BUF, &got) != FAT32_OK) return false;
	s->buf_at = at;
	s->buf_len = got;
	return got > 0;
}

size_t src_read(img_src_t *s, void *buf, size_t n)
{
	size_t done = 0;
	while (done < n && s->pos < s->size) {
		if (s->pos < s->buf_at || s->pos >= s->buf_at + s->buf_len)
			if (!fill(s)) break;
		size_t k = s->buf_at + s->buf_len - s->pos;
		if (k > n - done) k = n - done;
		memcpy((uint8_t *)buf + done, src_buf + (s->pos - s->buf_at), k);
		done += k;
		s->pos += k;
	}
	return done;
}

bool src_seek(img_src_t *s, uint32_t pos)
{
	if (pos > s->size) return false;
	s->pos = pos;
	return true;
}

uint32_t src_tell(img_src_t *s) { return s->pos; }
uint32_t src_size(img_src_t *s) { return s->size; }

/* ------------------------------------------------------------ screen */

static uint16_t fb[W * H];
static canvas_t canvas;

/* a line of text: the bottom row in the small font (the info line is
   long), the rest in the large one */
static void text_line(int row, uint16_t fg, const char *s)
{
	char b[80];
	lcd_set_font(row == 31 ? &font_5x10 : &font_8x10);
	int cols = lcd_get_columns();
	snprintf(b, sizeof b, "%-*.*s", cols, cols, s);
	lcd_set_reverse(false);
	lcd_set_foreground(fg);
	lcd_set_background(0x0000);
	lcd_putstr(0, row, b);
}

/* brightness: 0 is the picture as it is; each step lifts the darker tones
   (a gamma curve), so highlights don't burn out */
static int bright;
#define BRIGHT_MAX 5
static uint8_t lut5[32], lut6[64];

static void set_bright(int b)
{
	bright = b < 0 ? 0 : b > BRIGHT_MAX ? BRIGHT_MAX : b;
	float g = 1.0f - 0.1f * bright;
	for (int i = 0; i < 32; i++) lut5[i] = (uint8_t)(31.0f * powf(i / 31.0f, g) + 0.5f);
	for (int i = 0; i < 64; i++) lut6[i] = (uint8_t)(63.0f * powf(i / 63.0f, g) + 0.5f);
}

/* copy rows y0..y1-1 of fb to the screen, through the brightness curve */
static void blit_rows(int y0, int y1)
{
	if (!bright) {
		lcd_blit(fb + y0 * W, 0, y0, W, y1 - y0);
		return;
	}
	static uint16_t line[W * 8];
	for (int y = y0; y < y1; y += 8) {
		int n = y1 - y < 8 ? y1 - y : 8;
		const uint16_t *p = fb + y * W;
		for (int i = 0; i < n * W; i++) {
			uint16_t v = p[i];
			line[i] = (uint16_t)(lut5[v >> 11] << 11 | lut6[(v >> 5) & 63] << 5 | lut5[v & 31]);
		}
		lcd_blit(line, 0, y, W, n);
	}
}

static void show_fb(void) { blit_rows(0, H); }

/* put the picture back where text was drawn over it */
static void restore_row(int row)
{
	blit_rows(row * GLYPH_HEIGHT, (row + 1) * GLYPH_HEIGHT);
}

/* the keys, in a box over the picture; any key puts the picture back */
static void show_help(void)
{
	static const char *const lines[] = {
		"",
		"  Right, Space   next picture",
		"  Left           previous picture",
		"  Home / End     first / last",
		"  Tab            ten ahead",
		"  r              rotate 90 degrees",
		"  + / -          brighter / darker",
		"  i              name, size, time",
		"  s              slide show",
		"    Left / Right   back / skip",
		"  Esc            back to the list",
		"  ? or h         this help",
		"",
		"  (any key to go on)",
		"",
	};
	int n = sizeof lines / sizeof lines[0], top = (32 - n) / 2;
	lcd_set_font(&font_8x10);
	lcd_solid_rectangle(0x0010, 16, top * 10 - 4, 288, n * 10 + 8);
	lcd_set_background(0x0010);
	for (int i = 0; i < n; i++) {
		lcd_set_foreground(i == n - 2 ? 0x8410 : 0xFFFF);
		lcd_putstr(2, top + i, lines[i]);
	}
	lcd_set_background(0x0000);
}

static void drain_keys(void)
{
	while (keyboard_key_available()) keyboard_get_key();
}

static bool key_waiting(void) { return keyboard_key_available(); }

static bool is_picture(const char *name) { return format_for_name(name) != NULL; }

/* the settings (read_settings() below fills them in) */
static struct {
	int delay;		/* seconds per picture in a slide show */
	bool shuffle;
	bool autostart;
	bool fast;		/* 250 MHz */
	char folder[PICKER_PATH];	/* where to start; "" for /pictures */
} cfg = { 10, false, false, true, "" };

static void to_menu(void)
{
	lcd_clear_screen();
	/* Leave the chip as it was found: the clock goes back to 150 MHz with
	   the restart anyway, but the core voltage is kept by the always-on
	   power section, which a restart may not reset. */
	if (cfg.fast && picocalc_shell_card_present()) {
		set_sys_clock_khz(150000, true);
		vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
	}
	if (!picocalc_shell_exit()) {
		text_line(15, 0xFFFF, "No SD card: switch off to leave.");
		for (;;) tight_loop_contents();
	}
}

/* ------------------------------------------------------------ settings */

static bool word_is(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if ((*a | 0x20) != (*b | 0x20)) return false;
	return !*a && !*b;
}

static void trim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
	char *b = s;
	while (*b == ' ' || *b == '\t') b++;
	memmove(s, b, strlen(b) + 1);
}

/* /viewer/viewer.cfg: "name = value" lines; # starts a comment.  Anything
   not understood is passed over, so a mistake never stops the viewer. */
static void read_settings(void)
{
	fat32_file_t f;
	if (fat32_open(&f, "/viewer/viewer.cfg") != FAT32_OK) return;
	static char text[1024];
	size_t got = 0;
	if (fat32_read(&f, text, sizeof text - 1, &got) != FAT32_OK) got = 0;
	fat32_close(&f);
	text[got] = 0;
	for (char *line = text, *next; line; line = next) {
		next = strchr(line, '\n');
		if (next) *next++ = 0;
		char *hash = strchr(line, '#');
		if (hash) *hash = 0;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *key = line, *val = eq + 1;
		trim(key);
		trim(val);
		bool yes = word_is(val, "yes") || word_is(val, "on") || word_is(val, "true");
		if (word_is(key, "delay")) {
			int d = atoi(val);
			if (d >= 1 && d <= 86400) cfg.delay = d;
		} else if (word_is(key, "order")) cfg.shuffle = word_is(val, "shuffle");
		else if (word_is(key, "autostart")) cfg.autostart = yes;
		else if (word_is(key, "speed")) cfg.fast = !word_is(val, "normal");
		else if (word_is(key, "folder") && *val && strlen(val) + 2 < sizeof cfg.folder) {
			snprintf(cfg.folder, sizeof cfg.folder, "%s%s", *val == '/' ? "" : "/", val);
			size_t n = strlen(cfg.folder);
			while (n > 1 && cfg.folder[n - 1] == '/') cfg.folder[--n] = 0;
		}
	}
}

/* ------------------------------------------------------------ viewing */

/* decode one picture into a canvas; NULL or a message */
static const char *load_into(canvas_t *cv, const char *path)
{
	img_src_t src = { 0 };
	if (fat32_open(&src.f, path) != FAT32_OK) return "Can't open this file";
	src.size = fat32_size(&src.f);
	uint8_t head[16];
	size_t n = src_read(&src, head, sizeof head);
	const image_format_t *fmt = format_for_head(head, n);
	if (!fmt) fmt = format_for_name(path);
	const char *err = NULL;
	if (!fmt) err = "Not a picture this viewer knows";
	else if (!src_seek(&src, 0)) err = "Can't read this file";
	else {
		canvas_clear(cv);
		err = fmt->decode(&src, &cv->sink);
		/* stand it up, as the camera's orientation tag asks */
		if (!err) for (int r = cv->rotate; r > 0; r -= 90) canvas_turn(cv);
	}
	fat32_close(&src.f);
	return err;
}

/* The full-screen picture is kept: opening the same one again, or its
   thumbnail in the list, needs no reading or decoding. */
static char fb_path[PICKER_PATH];	/* what fb holds, if anything */
static int fb_turned;			/* and how far it has been turned */

static const char *load(const char *path)
{
	if (fb_path[0] && strcmp(path, fb_path) == 0) return NULL;
	fb_path[0] = 0;
	const char *err = load_into(&canvas, path);
	if (!err && !canvas.cancelled) {
		snprintf(fb_path, sizeof fb_path, "%s", path);
		fb_turned = canvas.rotate;
	}
	return err;
}

/* the list's preview panel: the last few thumbnails are kept */
#define THUMB 116
#define THUMBS 3
static struct {
	char path[PICKER_PATH];
	uint16_t px[THUMB * THUMB];
	int full_w, full_h;
	uint32_t used;
} thumbs[THUMBS];
static uint32_t thumb_clock;
static canvas_t thumb;

/* shrink the full-screen picture to a thumbnail, averaging each block */
static void shrink_fb(uint16_t *out)
{
	for (int ty = 0; ty < THUMB; ty++) {
		int y0 = ty * H / THUMB, y1 = (ty + 1) * H / THUMB;
		for (int tx = 0; tx < THUMB; tx++) {
			int x0 = tx * W / THUMB, x1 = (tx + 1) * W / THUMB;
			unsigned r = 0, g = 0, b = 0, n = 0;
			for (int y = y0; y < y1; y++)
				for (int x = x0; x < x1; x++) {
					uint16_t v = fb[y * W + x];
					r += v >> 11; g += (v >> 5) & 63; b += v & 31; n++;
				}
			out[ty * THUMB + tx] = (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n));
		}
	}
}

static void preview(const char *path, int x, int y, int size)
{
	(void)size;
	const char *err = NULL;
	int slot = -1;
	for (int k = 0; k < THUMBS; k++)
		if (strcmp(thumbs[k].path, path) == 0) slot = k;
	if (slot < 0) {
		/* reuse the least recently used slot */
		slot = 0;
		for (int k = 1; k < THUMBS; k++)
			if (thumbs[k].used < thumbs[slot].used) slot = k;
		thumbs[slot].path[0] = 0;
		if (fb_path[0] && strcmp(path, fb_path) == 0) {
			shrink_fb(thumbs[slot].px);
			thumbs[slot].full_w = canvas.full_w;
			thumbs[slot].full_h = canvas.full_h;
		} else {
			canvas_init(&thumb, thumbs[slot].px, THUMB, THUMB, key_waiting);
			err = load_into(&thumb, path);
			if (keyboard_key_available()) return;	/* moved on: leave the panel blank */
			thumbs[slot].full_w = thumb.full_w;
			thumbs[slot].full_h = thumb.full_h;
		}
		if (!err) snprintf(thumbs[slot].path, sizeof thumbs[slot].path, "%s", path);
	}
	thumbs[slot].used = ++thumb_clock;
	if (err) {
		lcd_set_font(&font_5x10);
		lcd_set_foreground(0xF800);
		lcd_set_background(0x10A2);
		lcd_putstr(x / 5 + 1, y / 10 + 5, "Can't show it");
		lcd_set_background(0x0000);
		return;
	}
	lcd_blit(thumbs[slot].px, x, y, THUMB, THUMB);
	/* the full size, on the panel's last line */
	const image_format_t *fmt = format_for_name(path);
	char b[32];
	snprintf(b, sizeof b, "%s %d x %d", fmt ? fmt->name : "", thumbs[slot].full_w, thumbs[slot].full_h);
	lcd_set_font(&font_5x10);
	lcd_set_foreground(0x8410);
	lcd_putstr(40, 28, b);
}

/* the slide show's order: the list's, or shuffled, every picture once
   before any comes round again */
static uint16_t order[PICKER_MAX_FILES];
static int order_at;
static uint32_t rng;

static uint32_t rnd(void)
{
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return rng;
}

/* a new shuffle, with first at the front (if it's a picture's index) */
static void shuffle(int n, int first)
{
	for (int k = 0; k < n; k++) order[k] = (uint16_t)k;
	for (int k = n - 1; k > 0; k--) {
		int j = (int)(rnd() % (uint32_t)(k + 1));
		uint16_t t = order[k]; order[k] = order[j]; order[j] = t;
	}
	for (int k = 0; k < n; k++)
		if (order[k] == first) { order[k] = order[0]; order[0] = (uint16_t)first; break; }
	order_at = 0;
}

/* the pictures a shuffled show has shown, so Left can go back over them */
#define HIST 64
static uint16_t hist[HIST];
static int hist_n, hist_at;		/* how many; which one is up */

static void hist_push(int i)
{
	if (hist_n == HIST) {
		memmove(hist, hist + 1, (HIST - 1) * sizeof hist[0]);
		hist_n--;
	}
	hist[hist_n++] = (uint16_t)i;
	hist_at = hist_n - 1;
}

static void show_start(int n, int i)
{
	rng = time_us_32() | 1;
	if (cfg.shuffle) shuffle(n, i);
	hist_n = 0;
	hist_push(i);
}

static int show_next(int n, int i)
{
	if (!cfg.shuffle) return (i + 1) % n;
	if (++order_at >= n) {		/* round again, in a new order */
		shuffle(n, -1);
		if (n > 1 && order[0] == i) {	/* but not the same one twice running */
			order[0] = order[1];
			order[1] = (uint16_t)i;
		}
	}
	return order[order_at];
}

/* the slide show's next picture: forward through what Left went back
   over, then on to new ones */
static int show_forward(int n, int i)
{
	if (!cfg.shuffle) return (i + 1) % n;
	if (hist_at < hist_n - 1) return hist[++hist_at];
	i = show_next(n, i);
	hist_push(i);
	return i;
}

/* the one before; -1 if a shuffled show has nothing earlier to go back to */
static int show_back(int n, int i)
{
	if (!cfg.shuffle) return (i + n - 1) % n;
	return hist_at > 0 ? hist[--hist_at] : -1;
}

/* wait up to ms for a key; true if one came */
static bool key_within(uint32_t ms)
{
	absolute_time_t until = make_timeout_time_ms(ms);
	while (!keyboard_key_available())
		if (absolute_time_diff_us(get_absolute_time(), until) <= 0) return false;
		else sleep_ms(10);
	return true;
}

/* show the folder's pictures, starting at index i, as a slide show if
   show; returns the index of the last one shown, on Esc */
static int view(const char *dir, int i, bool show)
{
	int n = picker_file_count();
	char path[PICKER_PATH], info[80];
	bool show_info = false;
	int failed = 0;			/* pictures in a row the slide show couldn't show */
	bool back = false;		/* the show is going backwards (Left) */
	if (show) show_start(n, i);
	bool notice = show;		/* say how to stop it, at first */
	while (n > 0) {
		const char *name = picker_file_name(i);
		snprintf(path, sizeof path, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
		/* in a slide show the last picture stays up while the next loads,
		   and a key pressed meanwhile waits until it is shown */
		if (!show) text_line(31, 0xFFE0, "Loading...");
		canvas.cancel = show ? NULL : key_waiting;
		uint32_t t0 = to_ms_since_boot(get_absolute_time());
		const char *err = load(path);
		uint32_t ms = to_ms_since_boot(get_absolute_time()) - t0;
		canvas.cancel = key_waiting;
		int turned = fb_turned;		/* degrees, from the file and from r */
		if (ms < 100) ms = 100;		/* "0.0s" reads oddly */
		/* A key stopped the load: the screen still has the last picture.  A
		   key that moves on (or Esc) does so now; any other key starts the
		   load again, as if it hadn't been pressed. */
		int pending = -1;
		if (!err && canvas.cancelled) {
			uint8_t k = (uint8_t)keyboard_get_key();
			if (k == KEY_ESC || k == KEY_RETURN || k == KEY_ENTER || k == KEY_RIGHT ||
			    k == KEY_PAGE_DOWN || k == ' ' || k == KEY_LEFT || k == KEY_PAGE_UP ||
			    k == KEY_BACKSPACE || k == KEY_HOME || k == KEY_END || k == KEY_TAB)
				pending = k;
			if ((k == KEY_HOME && i == 0) || (k == KEY_END && i == n - 1) || (k == KEY_TAB && n == 1))
				pending = -1;		/* it's already here: load it again */
			if (pending < 0) continue;
		}
		if (show && err) {		/* pass it by, the way the show was going */
			if (++failed < n) {
				if (cfg.shuffle && hist_at == hist_n - 1) hist_at = --hist_n - 1;	/* forget it */
				int j = back ? show_back(n, i) : -1;
				i = j >= 0 ? j : show_forward(n, i);
				continue;
			}
			show = false;		/* nothing here it can show */
		}
		failed = 0;
		if (pending < 0) show_fb();
		if (err) {
			lcd_clear_screen();
			text_line(14, 0xF800, err);
			text_line(15, 0xFFFF, name);
			drain_keys();		/* keys pressed while it loaded */
		}
		for (;;) {
			int len = snprintf(info, sizeof info, "%d/%d %s %dx%d %lu.%lus", i + 1, n, name,
			                   canvas.full_w, canvas.full_h, (unsigned long)(ms / 1000),
			                   (unsigned long)(ms % 1000 / 100));
			if (turned && len < (int)sizeof info)
				len += snprintf(info + len, sizeof info - len, " rotated %d", turned);
			if (bright && len < (int)sizeof info)
				snprintf(info + len, sizeof info - len, " bright +%d", bright);
			if (show_info && !err) text_line(31, 0xFFFF, info);
			if (show) {
				uint32_t wait = (uint32_t)cfg.delay * 1000;
				bool key;
				if (notice) {		/* for the first 3 seconds */
					char b[64];
					snprintf(b, sizeof b, "Slide show, %d s each. Left/Right: back/skip. Other keys stop.", cfg.delay);
					text_line(31, 0xFFE0, b);
					uint32_t first = wait < 3000 ? wait : 3000;
					key = key_within(first);
					if (show_info) text_line(31, 0xFFFF, info);
					else restore_row(31);
					notice = false;
					if (!key) key = key_within(wait - first);
				} else key = key_within(wait);
				back = false;
				if (!key) { i = show_forward(n, i); break; }
				uint8_t k = (uint8_t)keyboard_get_key();
				if (k == KEY_RIGHT) { i = show_forward(n, i); break; }
				if (k == KEY_LEFT) {
					int j = show_back(n, i);
					if (j < 0) continue;	/* back at the show's first picture */
					i = j;
					back = true;
					break;
				}
				/* any other key: the show stops, and that key does nothing more */
				show = false;
				text_line(31, 0xFFE0, "Slide show stopped");
				sleep_ms(800);
				if (show_info) text_line(31, 0xFFFF, info);
				else restore_row(31);
				continue;
			}
			uint8_t k = pending >= 0 ? (uint8_t)pending : (uint8_t)keyboard_get_key();
			pending = -1;
			if ((k == 's' || k == 'S') && n > 0) {
				show = notice = true;
				show_start(n, i);
				continue;
			}
			if (k == KEY_ESC || k == KEY_RETURN || k == KEY_ENTER) return i;
			if (k == KEY_RIGHT || k == KEY_PAGE_DOWN || k == ' ') {
				i = (i + 1) % n;
				break;
			}
			if (k == KEY_LEFT || k == KEY_PAGE_UP || k == KEY_BACKSPACE) {
				i = (i + n - 1) % n;
				break;
			}
			if (k == KEY_HOME && i != 0) { i = 0; break; }
			if (k == KEY_END && i != n - 1) { i = n - 1; break; }
			if (k == KEY_TAB && n > 1) {	/* ten ahead, or to the last one */
				i = i == n - 1 ? 0 : (i + 10 < n ? i + 10 : n - 1);
				break;
			}
			if ((k == 'i' || k == 'I') && !err) {
				show_info = !show_info;
				if (!show_info) restore_row(31);
			}
			if (k == '?' || k == 'h' || k == 'H') {
				show_help();
				keyboard_get_key();
				if (err) {
					lcd_clear_screen();
					text_line(14, 0xF800, err);
					text_line(15, 0xFFFF, name);
				} else show_fb();
				continue;
			}
			if ((k == 'r' || k == 'R') && !err) {	/* a quarter turn clockwise */
				canvas_turn(&canvas);
				turned = (turned + 90) % 360;
				fb_turned = turned;
				/* its thumbnail no longer matches */
				for (int k = 0; k < THUMBS; k++)
					if (strcmp(thumbs[k].path, path) == 0) thumbs[k].path[0] = 0;
				show_fb();
			}
			if ((k == '+' || k == '=' || k == '-') && !err) {
				set_bright(bright + (k == '-' ? -1 : 1));
				show_fb();
			}
		}
	}
	return i;
}

int main(void)
{
	/* The settings first, at the usual 150 MHz, so speed = normal can
	   keep it there. */
	fat32_init();
	read_settings();
	if (cfg.fast) {
		/* 250 MHz: decoding a big JPEG is mostly arithmetic.  A little
		   more core voltage first, as is usual for this speed. */
		vreg_set_voltage(VREG_VOLTAGE_1_15);
		sleep_ms(2);
		set_sys_clock_khz(250000, true);
		/* the SD card's SPI clock was set for 150 MHz: set it again */
		spi_set_baudrate(SD_SPI, SD_BAUDRATE);
	}
	sb_init();
	lcd_init();
	lcd_set_font(&font_8x10);
	lcd_enable_cursor(false);
	keyboard_init();
	keyboard_set_background_poll(true);
	canvas_init(&canvas, fb, W, H, key_waiting);
	set_bright(0);
	static const char *const list_keys[] = {
		"  s              slide show of",
		"                 the folder",
		NULL,
	};
	static const picker_style_t style = {
		.title = "Pictures", .banner = banner, .banner_w = BANNER_W, .banner_h = BANNER_H,
		.filter = is_picture, .noun = "picture", .nouns = "pictures", .preview = preview,
		.keys = "sS", .keys_help = list_keys,
	};

	/* start in the folder the settings name, else /pictures, else the top */
	char dir[PICKER_PATH] = "/pictures";
	fat32_file_t d;
	if (cfg.folder[0] && fat32_open(&d, cfg.folder) == FAT32_OK) {
		fat32_close(&d);
		strcpy(dir, cfg.folder);
	} else if (fat32_open(&d, dir) == FAT32_OK) fat32_close(&d);
	else strcpy(dir, "/");

	char path[PICKER_PATH];
	const char *last = NULL;
	char lastname[FAT32_MAX_FILENAME_LEN + 1];
	/* picture frame: straight into a slide show */
	if (cfg.autostart && picker_load(dir, is_picture) && picker_file_count() > 0) {
		lcd_clear_screen();
		int n = picker_file_count();
		int i = view(dir, cfg.shuffle ? (int)(time_us_32() % (uint32_t)n) : 0, true);
		snprintf(lastname, sizeof lastname, "%s", picker_file_name(i));
		last = lastname;
	}
	for (;;) {
		int got = picker_run(&style, dir, last, path);
		if (!got) { to_menu(); continue; }
		bool show = got != PICKER_CHOSEN;
		int i;
		if (show && picker_on_folder()) {
			/* s on a folder: a slide show of what's in it, from the top */
			if (!picker_load(path, is_picture) || picker_file_count() == 0) {
				picker_load(dir, is_picture);
				text_line(31, 0xFFE0, "No pictures in that folder");
				sleep_ms(1500);
				last = NULL;
				if (strcmp(path, dir) != 0) {
					snprintf(lastname, sizeof lastname, "%s", strrchr(path, '/') + 1);
					last = lastname;
				}
				continue;
			}
			snprintf(dir, PICKER_PATH, "%s", path);
			i = cfg.shuffle ? (int)(time_us_32() % (uint32_t)picker_file_count()) : 0;
		} else {
			snprintf(lastname, sizeof lastname, "%s", strrchr(path, '/') + 1);
			i = picker_file_index(lastname);
			if (i < 0) continue;
		}
		lcd_clear_screen();
		i = view(dir, i, show);
		/* come back to the list on the picture last shown */
		snprintf(lastname, sizeof lastname, "%s", picker_file_name(i));
		last = lastname;
	}
}
