/* SPDX-License-Identifier: GPL-3.0-or-later
 * remind - the interval reminder (Galtzagorriak MINIX, spec 7b.1, F.4).
 *
 * Usage:	remind		the app (libfuri GUI)
 *		remind -s	today's count and the last 7 days, as text
 *
 * Any number of reminders, each one `interval` timer in /var/timers
 * (libapp, category reminder, id remind-N): a text, every N minutes in a
 * window (09:00-17:00) on chosen weekdays, sound, vibro and LED, snooze.
 * Nothing of it is in RAM while it is closed: timed fires the timer and
 * alert shows it; OK on the alert ("done") appends "TIME remind-N" to
 * APP_DATA/remind/log (the timer's Log: field, written by alert as the
 * user remind).  The app shows today's count and a 7-day bar chart.
 *
 * Screens: main (count, chart; OK: the reminders, Back: quit), the list
 * (OK: edit, the first line adds one), the form (Up/Down: field,
 * Left/Right: change, OK: toggle, Save, Delete; Back: cancel).
 *
 * The launcher starts it as root (/etc/apps "Remind:root:..."); once the
 * GUI has the screen and buttons it becomes the user remind of group
 * timers (nat_become(), until the launcher gives apps their own uid,
 * spec A.3).
 */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/canvas.h>
#include "src/gui/gui_i.h"	/* the title bar text */
#include "app.h"
#include "nat.h"

#define APP	"remind"
#define PREFIX	"remind-"

typedef struct {
	char id[APP_ID_MAX + 1];
	char text[17];		/* as much as the list shows */
	unsigned short every;
} Rem;

static const char *const texts[] = {
	"Drink a glass of water", "Stand up and stretch", "Rest your eyes",
	"Take your medicine", "Breathe slowly", "Check your posture",
};
#define NTEXTS	(int) (sizeof texts / sizeof *texts)
static const unsigned char snoozes[] = { 0, 5, 10, 15, 30 };

enum { S_MAIN, S_LIST, S_EDIT };
enum { F_TEXT, F_EVERY, F_FROM, F_TO, F_DAYS, F_INCL, F_SOUND, F_VIBRO,
       F_LED, F_SNOOZE, F_SAVE, F_DEL };

static struct {
	int screen;
	int today, counts[7];
	Rem* list;
	int n, sel, top;
	struct app_timer t;	/* the one in the form */
	int is_new, dcur;
	struct nat_form f;
	const char* msg;	/* one line for the form, until the next key */
	bool quit;
} r;

/* ---- data ---- */

static void counts(void)
{
	int fd;
	char path[48];

	strcpy(path, APP_DATA "/" APP "/log");
	memset(r.counts, 0, sizeof r.counts);
	if ((fd = open(path, O_RDONLY)) >= 0) {
		(void) nat_log_days(fd, nat_midnight(time(NULL), 0), 7, r.counts);
		close(fd);
	}
	r.today = r.counts[6];
}

static int ours(const struct app_timer* t)
{
	return strncmp(t->id, PREFIX, sizeof PREFIX - 1) == 0
		&& strcmp(t->log, APP) == 0;
}

static int add_cb(struct app_timer* t, void* arg)
{
	Rem* m;

	(void) arg;
	if (!ours(t)) return 0;
	if ((m = realloc(r.list, (r.n + 1) * sizeof *m)) == NULL) return 1;
	r.list = m;
	m += r.n++;
	strcpy(m->id, t->id);
	strncpy(m->text, t->text, sizeof m->text - 1);
	m->text[sizeof m->text - 1] = 0;
	m->every = t->every;
	return 0;
}

static void load(void)
{
	free(r.list);
	r.list = NULL;
	r.n = 0;
	(void) timer_list(add_cb, NULL);
	if (r.sel > r.n) r.sel = r.n;
}

/* The lowest id remind-N not in the list (no file lookups). */
static void new_id(char* id)
{
	int i, j;

	for (i = 1; ; i++) {
		strcpy(id, PREFIX);
		nat_num(id + sizeof PREFIX - 1, i);
		for (j = 0; j < r.n && strcmp(r.list[j].id, id) != 0; j++) ;
		if (j == r.n) return;
	}
}

/* ---- the form ---- */

static void form_values(void)
{
	struct nat_form* f = &r.f;
	struct app_timer* t = &r.t;
	char b[8];
	int i;

	f->n = r.is_new ? F_SAVE + 1 : F_DEL + 1;
	f->label[F_TEXT] = "";
	strncpy(f->value[F_TEXT], t->text, sizeof f->value[0] - 1);
	f->label[F_EVERY] = "Every";
	strcat(strcpy(f->value[F_EVERY], nat_num(b, t->every)), " min");
	f->label[F_FROM] = "From";
	nat_hm(f->value[F_FROM], t->start);
	f->label[F_TO] = "To";
	nat_hm(f->value[F_TO], t->end);
	f->label[F_DAYS] = "Days";
	nat_days(f->value[F_DAYS], t->days);
	f->label[F_INCL] = "Ring at To";
	strcpy(f->value[F_INCL], t->incl ? "yes" : "no");
	f->label[F_SOUND] = "Sound";
	f->label[F_VIBRO] = "Vibro";
	f->label[F_LED] = "LED";
	for (i = 0; i < 3; i++)
		strcpy(f->value[F_SOUND + i], t->flags & (1 << i) ? "yes" : "no");
	f->label[F_SNOOZE] = "Snooze";
	strcpy(f->value[F_SNOOZE], t->snooze ? strcat(nat_num(b, t->snooze),
								" min") : "off");
	f->label[F_SAVE] = "Save";
	f->value[F_SAVE][0] = 0;
	f->label[F_DEL] = "Delete";
	f->value[F_DEL][0] = 0;
}

static void edit(int i)
{
	struct app_timer* t = &r.t;

	memset(t, 0, sizeof *t);
	r.is_new = i < 0;
	if (i < 0 || timer_read(r.list[i].id, t) < 0) {
		r.is_new = 1;
		memset(t, 0, sizeof *t);
		new_id(t->id);
		t->kind = TK_INTERVAL;
		t->start = 9 * 60;
		t->end = 17 * 60;
		t->every = 30;
		t->days = 0x7F;
		t->flags = NF_ALL;
		t->snooze = 5;
		strcpy(t->text, texts[0]);
	}
	/* what the app owns, whatever the file said */
	t->kind = TK_INTERVAL;
	t->cat = TC_REMINDER;
	t->action = TA_NOTIFY;
	strcpy(t->title, "Reminder");
	strcpy(t->log, APP);
	memset(&r.f, 0, sizeof r.f);
	r.dcur = 0;
	r.msg = NULL;
	form_values();
	r.screen = S_EDIT;
}

static int step(int v, int d, int lo, int hi, int s)
{
	v += d * s;
	return v < lo ? hi - (hi - lo) % s : v > hi ? lo : v;
}

static void change(int d)
{
	struct app_timer* t = &r.t;
	int i;

	switch (r.f.cur) {
	case F_TEXT:
		for (i = 0; i < NTEXTS && strcmp(texts[i], t->text) != 0; i++) ;
		i = i == NTEXTS ? (d > 0 ? 0 : NTEXTS - 1)
				: (i + d + NTEXTS) % NTEXTS;
		strcpy(t->text, texts[i]);
		break;
	case F_EVERY:
		/* 5-minute steps up to an hour, then 15 */
		i = t->every + d * (t->every + (d > 0) > 60 ? 15 : 5);
		t->every = i < 5 ? 240 : i > 240 ? 5 : i;
		break;
	case F_FROM: t->start = step(t->start, d, 0, 1425, 15); break;
	case F_TO: t->end = step(t->end, d, 15, 1440, 15); break;
	case F_DAYS: r.dcur = (r.dcur + d + 7) % 7; break;
	case F_INCL: t->incl ^= 1; break;
	case F_SOUND: case F_VIBRO: case F_LED:
		t->flags ^= 1 << (r.f.cur - F_SOUND);
		break;
	case F_SNOOZE:
		for (i = 0; i < 5 && snoozes[i] != t->snooze; i++) ;
		t->snooze = snoozes[(i + d + 5) % 5];
		break;
	}
	form_values();
}

static void ok(void)
{
	struct app_timer* t = &r.t;

	switch (r.f.cur) {
	case F_DAYS:
		t->days ^= nat_daybit(r.dcur);
		form_values();
		return;
	case F_SAVE:
		if (t->start >= t->end) r.msg = "From must be before To";
		else if (t->days == 0) r.msg = "Choose a day";
		else if (timer_add(t) < 0) r.msg = "Cannot save the timer";
		else break;
		return;
	case F_DEL:
		if (timer_remove(t->id) < 0) {
			r.msg = "Cannot delete it";
			return;
		}
		break;
	default:
		change(1);
		return;
	}
	load();
	r.screen = S_LIST;
}

/* ---- drawing ---- */

static void draw_main(Canvas* c)
{
	char b[12];
	int i, max = 1, h;
	time_t now = time(NULL);
	struct tm* tm = localtime(&now);
	static const char l[] = "SMTWTFS";

	canvas_set_font(c, FontSecondary);
	canvas_draw_str(c, 2, 9, "Today");
	canvas_set_font(c, FontBigNumbers);
	canvas_draw_str(c, 2, 34, nat_num(b, r.today));
	canvas_set_font(c, FontSecondary);
	canvas_draw_str(c, 2, 49, "OK: list");
	for (i = 0; i < 7; i++) if (r.counts[i] > max) max = r.counts[i];
	canvas_draw_str_aligned(c, 126, 1, AlignRight, AlignTop, nat_num(b, max));
	for (i = 0; i < 7; i++) {
		h = r.counts[i] * 32 / max;
		if (r.counts[i] && h == 0) h = 1;
		canvas_draw_box(c, 52 + i * 10, 41 - h, 7, h);
		b[0] = l[(tm->tm_wday + i + 1) % 7];
		b[1] = 0;
		canvas_draw_str_aligned(c, 55 + i * 10, 43, AlignCenter, AlignTop, b);
	}
	canvas_draw_line(c, 50, 41, 120, 41);
}

static void row(Canvas* c, int y, bool sel, const char* left, const char* right)
{
	if (sel) {
		canvas_draw_box(c, 0, y, 128, 10);
		canvas_set_color(c, ColorWhite);
	}
	canvas_draw_str(c, 2, y + 8, left);
	if (right) canvas_draw_str_aligned(c, 126, y + 1, AlignRight, AlignTop, right);
	canvas_set_color(c, ColorBlack);
}

static void draw_list(Canvas* c)
{
	char b[12];
	int i, y;

	canvas_set_font(c, FontSecondary);
	if (r.sel < r.top) r.top = r.sel;
	if (r.sel >= r.top + NAT_ROWS) r.top = r.sel - NAT_ROWS + 1;
	for (i = r.top, y = 0; i <= r.n && y < NAT_ROWS * 10; i++, y += 10) {
		if (i == 0) {
			row(c, y, r.sel == 0, "+ New reminder", NULL);
			continue;
		}
		strcat(nat_num(b, r.list[i - 1].every), "m");
		row(c, y, r.sel == i, r.list[i - 1].text, b);
	}
}

static void draw_form(Canvas* c)
{
	struct nat_form* f = &r.f;
	int i, y;

	canvas_set_font(c, FontSecondary);
	for (i = f->top, y = 0; i < f->n && i < f->top + NAT_ROWS; i++, y += 10) {
		row(c, y, f->cur == i, i == F_TEXT ? f->value[i] : f->label[i],
			i == F_TEXT ? NULL : f->value[i]);
		if (i == F_DAYS && f->cur == i) {	/* the day cursor */
			int w = canvas_string_width(c, f->value[i]);
			int x = 126 - w + r.dcur * w / 7;

			canvas_set_color(c, ColorWhite);
			canvas_draw_line(c, x, y + 9, x + w / 7 - 2, y + 9);
			canvas_set_color(c, ColorBlack);
		}
	}
	if (r.msg) {
		canvas_set_color(c, ColorWhite);
		canvas_draw_box(c, 0, 40, 128, 11);
		canvas_set_color(c, ColorBlack);
		canvas_draw_frame(c, 0, 40, 128, 11);
		canvas_draw_str_aligned(c, 64, 42, AlignCenter, AlignTop, r.msg);
	}
}

static void draw(Canvas* c, void* ctx)
{
	(void) ctx;
	canvas_clear(c);
	canvas_set_color(c, ColorBlack);
	if (r.screen == S_MAIN) draw_main(c);
	else if (r.screen == S_LIST) draw_list(c);
	else draw_form(c);
}

/* ---- input ---- */

static void input(InputEvent* e, void* ctx)
{
	FuriMessageQueue* q = ctx;

	/* on the press, as flipper/gui (T3: L1 counts from the press) */
	if (e->type == InputTypePress || (e->type == InputTypeRepeat
	    && e->key != InputKeyOk && e->key != InputKeyBack))
		furi_message_queue_put(q, e, 0);
}

static void key(const InputEvent* e)
{
	r.msg = NULL;
	switch (r.screen) {
	case S_MAIN:
		if (e->key == InputKeyBack) r.quit = true;
		else if (e->key == InputKeyOk) {
			load();
			r.sel = 0;
			r.screen = S_LIST;
		} else counts();
		break;
	case S_LIST:
		if (e->key == InputKeyBack) {
			counts();
			r.screen = S_MAIN;
		} else if (e->key == InputKeyUp) r.sel = (r.sel + r.n) % (r.n + 1);
		else if (e->key == InputKeyDown) r.sel = (r.sel + 1) % (r.n + 1);
		else if (e->key == InputKeyOk) edit(r.sel - 1);
		break;
	case S_EDIT:
		if (e->key == InputKeyBack) r.screen = S_LIST;
		else if (e->key == InputKeyUp) nat_form_move(&r.f, -1);
		else if (e->key == InputKeyDown) nat_form_move(&r.f, 1);
		else if (e->key == InputKeyLeft) change(-1);
		else if (e->key == InputKeyRight) change(1);
		else if (e->key == InputKeyOk) ok();
		break;
	}
}

static int summary(void)
{
	int i;

	counts();
	printf("today %d\nweek", r.today);
	for (i = 0; i < 7; i++) printf(" %d", r.counts[i]);
	printf("\n");
	return 0;
}

int main(int argc, char** argv)
{
	FuriMessageQueue* q;
	ViewPort* vp;
	Gui* gui;
	InputEvent e;

	if (argc == 2 && strcmp(argv[1], "-s") == 0) {
		if (nat_become(APP) < 0) return 1;
		return summary();
	}
	if (argc != 1) {
		fprintf(stderr, "usage: remind [-s]\n");
		return 2;
	}
	q = furi_message_queue_alloc(4, sizeof(InputEvent));
	vp = view_port_alloc();
	view_port_draw_callback_set(vp, draw, NULL);
	view_port_input_callback_set(vp, input, q);
	(void) nat_foreground();		/* as root, before the GUI */
	gui = furi_record_open(RECORD_GUI);	/* opens the screen, buttons */
	if (nat_become(APP) < 0) {
		fprintf(stderr, "remind: no user " APP " (pkg install remind)\n");
		furi_record_close(RECORD_GUI);
		nat_background();
		return 1;
	}
	gui->title = APP;
	counts();
	gui_add_view_port(gui, vp, GuiLayerWindow);
	while (!r.quit) {
		if (furi_message_queue_get(q, &e, FuriWaitForever) != FuriStatusOk)
			continue;
		key(&e);
		view_port_update(vp);
	}
	gui_remove_view_port(gui, vp);
	view_port_free(vp);
	furi_record_close(RECORD_GUI);
	nat_background();
	furi_message_queue_free(q);
	free(r.list);
	return 0;
}
