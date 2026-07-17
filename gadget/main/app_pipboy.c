/*
 * Pip-Boy 3000 — імітація інтерфейсу з Fallout (зелений CRT).
 * Вкладки: STATUS (персонаж + HP/AP), S.P.E.C.I.A.L, SKILLS, DATA.
 * Керування: –/+ перемикання вкладок, центр — наступна; утримання — вихід.
 * «Живі» дані: годинник, заряд («power cell»), Wi-Fi («radio»).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_random.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"
#include "battery.h"

#define PB   0x2BE04A     /* яскраво-зелений Pip-Boy */
#define PBD  0x0F5A22     /* тьмяний зелений (фон смуг) */
#define PBG  0x061A0C     /* майже чорне тло */

typedef enum { T_STATUS, T_SPECIAL, T_SKILLS, T_DATA, T_COUNT } tab_t;
static const char *TABS[] = { "STATUS", "S.P.E.C.I.A.L.", "SKILLS", "DATA" };
static int s_tab;

static lv_obj_t *s_hdr, *s_body, *s_foot, *s_scan;
static lv_timer_t *s_timer;
static int s_scan_y;
static bool s_cursor;

/* дин. елементи вкладки DATA */
static lv_obj_t *s_clk, *s_rads;

/* анімований персонаж (STATUS) */
static lv_obj_t *s_fig, *s_eyeR, *s_wink, *s_thumb;
#define FIG_X 2
#define FIG_Y 6
#define THUMB_Y 40
static int s_phase;

/* ---------- примітиви ---------- */

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, int r, uint32_t c)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    return o;
}

static lv_obj_t *dot(lv_obj_t *p, int x, int y, int d, uint32_t c)
{
    lv_obj_t *o = box(p, x, y, d, d, LV_RADIUS_CIRCLE, c);
    return o;
}

static lv_obj_t *txt(lv_obj_t *p, int x, int y, const char *s, uint32_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, s);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    return l;
}

/* смуга-показник: тьмяне тло + яскраве заповнення value/max */
static void bar(lv_obj_t *p, int x, int y, int w, int val, int max)
{
    box(p, x, y, w, 8, 2, PBD);
    int fw = max > 0 ? w * val / max : 0;
    if (fw > 0) box(p, x, y, fw, 8, 2, PB);
}

/* ---------- оригінальний талісман: велика голова, поза «палець угору» ---------- */

/* створює фігурку в контейнері s_fig (розм. ~112x160), зберігає динам. частини */
static void build_figure(void)
{
    lv_obj_t *f = s_fig;
    const int cx = 56;

    /* маленька антенка-вихор зверху (власна риса маскота) */
    box(f, cx - 2, 0, 4, 8, 2, PB);
    dot(f, cx - 5, -2, 8, PB);
    /* велика кругла голова (мультяшна пропорція) */
    dot(f, cx - 34, 4, 68, PB);
    /* очі: ліве відкрите, праве — підморгує */
    dot(f, cx - 17, 28, 8, PBG);
    s_eyeR = dot(f, cx + 9, 28, 8, PBG);
    s_wink = box(f, cx + 7, 32, 13, 3, 1, PBG);     /* «заплющене» під час підморгування */
    lv_obj_add_flag(s_wink, LV_OBJ_FLAG_HIDDEN);
    /* широка привітна усмішка */
    box(f, cx - 19, 46, 38, 12, 6, PBG);            /* рот */
    box(f, cx - 19, 46, 38, 3, 0, PB);              /* зуби */
    /* комір + маленький тулуб */
    box(f, cx - 17, 72, 34, 6, 3, PB);
    box(f, cx - 15, 76, 30, 28, 8, PB);
    box(f, cx - 1, 78, 2, 22, 0, PBG);              /* застібка */
    /* ліва рука вниз */
    box(f, cx - 23, 78, 9, 24, 4, PB);
    /* права рука піднята з кулаком */
    box(f, cx + 14, 80, 9, 12, 4, PB);              /* плече */
    box(f, cx + 17, 60, 9, 24, 3, PB);              /* передпліччя */
    dot(f, cx + 13, 48, 16, PB);                    /* кулак */
    /* великий палець угору (анімований) */
    s_thumb = box(f, cx + 25, THUMB_Y, 7, 15, 3, PB);
    /* короткі ніжки */
    box(f, cx - 13, 104, 11, 24, 4, PB);
    box(f, cx + 2, 104, 11, 24, 4, PB);
}

/* ---------- рендер вкладок ---------- */

static void render_status(void)
{
    s_fig = lv_obj_create(s_body);
    lv_obj_set_size(s_fig, 112, 160);
    lv_obj_set_pos(s_fig, FIG_X, FIG_Y);
    lv_obj_set_style_bg_opa(s_fig, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_fig, 0, 0);
    lv_obj_set_style_pad_all(s_fig, 0, 0);
    lv_obj_clear_flag(s_fig, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_fig, LV_SCROLLBAR_MODE_OFF);
    build_figure();

    txt(s_body, 132, 8, "LVL 12", PB);
    int bat = battery_level(); if (bat < 0) bat = 88;
    txt(s_body, 132, 34, "HP", PB);
    bar(s_body, 132, 52, 84, 320, 340);
    txt(s_body, 132, 66, "AP", PB);
    bar(s_body, 132, 84, 84, bat, 100);         /* AP = заряд плати */
    txt(s_body, 132, 100, "COND", PB);
    txt(s_body, 132, 118, "OK  100%", PB);

    /* умова кінцівок */
    static const char *limb[] = { "HEAD", "TORSO", "L.ARM",
                                  "R.ARM", "L.LEG", "R.LEG" };
    for (int i = 0; i < 6; i++) {
        int y = 150 + i * 15;
        txt(s_body, 10, y, limb[i], PB);
        dot(s_body, 92, y + 3, 8, PB);          /* «здорова» кінцівка */
    }
}

static void render_special(void)
{
    static const char *nm[] = { "STRENGTH", "PERCEPTION", "ENDURANCE",
                                "CHARISMA", "INTELLIGENCE", "AGILITY", "LUCK" };
    static const int val[] = { 6, 7, 5, 4, 8, 6, 5 };
    for (int i = 0; i < 7; i++) {
        int y = 6 + i * 27;
        txt(s_body, 8, y, nm[i], PB);
        char v[4]; snprintf(v, sizeof(v), "%d", val[i]);
        txt(s_body, 196, y, v, PB);
        bar(s_body, 8, y + 17, 180, val[i], 10);
    }
}

static void render_skills(void)
{
    static const char *nm[] = { "ENERGY WEAPONS", "LOCKPICK", "SCIENCE",
                                "REPAIR", "SPEECH", "SNEAK", "MEDICINE" };
    static const int val[] = { 72, 55, 90, 63, 41, 48, 77 };
    for (int i = 0; i < 7; i++) {
        int y = 6 + i * 27;
        txt(s_body, 8, y, nm[i], PB);
        char v[6]; snprintf(v, sizeof(v), "%d%%", val[i]);
        txt(s_body, 196, y, v, PB);
        bar(s_body, 8, y + 17, 180, val[i], 100);
    }
}

static void render_data(void)
{
    char clk[8]; netcfg_get_clock(clk, sizeof(clk));
    int bat = battery_level(); if (bat < 0) bat = 88;

    txt(s_body, 8, 8, "TIME", PB);
    s_clk = txt(s_body, 150, 8, clk, PB);

    txt(s_body, 8, 40, "POWER CELL", PB);
    char b[12]; snprintf(b, sizeof(b), "%d%%", bat);
    txt(s_body, 150, 40, b, PB);
    bar(s_body, 8, 60, 208, bat, 100);

    txt(s_body, 8, 82, "RADIO", PB);
    txt(s_body, 150, 82, netcfg_is_connected() ? "ONLINE" : "----", PB);

    txt(s_body, 8, 114, "RADS", PB);
    s_rads = txt(s_body, 150, 114, "12 rad/s", PB);
    bar(s_body, 8, 134, 208, 12, 100);

    txt(s_body, 8, 156, "CAPS", PB);
    txt(s_body, 150, 156, "1247", PB);

    txt(s_body, 8, 188, "> VAULT-TEC INDUSTRIES", PBD);
}

static void render(void)
{
    lv_obj_clean(s_body);
    s_clk = s_rads = NULL;
    s_fig = s_eyeR = s_wink = s_thumb = NULL;

    /* заголовок вкладки */
    char h[40];
    snprintf(h, sizeof(h), "%s %s", TABS[s_tab], s_cursor ? "_" : " ");
    lv_label_set_text(s_hdr, h);

    /* індикатор вкладок унизу */
    char dots[32] = "";
    for (int i = 0; i < T_COUNT; i++)
        strcat(dots, i == s_tab ? "[o]" : " . ");
    lv_label_set_text(s_foot, dots);

    switch (s_tab) {
    case T_STATUS:  render_status();  break;
    case T_SPECIAL: render_special(); break;
    case T_SKILLS:  render_skills();  break;
    default:        render_data();    break;
    }
    lv_obj_move_foreground(s_scan);   /* скан-лінія понад вмістом */
}

/* ---------- анімація CRT ---------- */

static void tick(lv_timer_t *t)
{
    /* рух скан-лінії */
    s_scan_y = (s_scan_y + 6) % 208;
    lv_obj_set_y(s_scan, 30 + s_scan_y);

    /* анімація персонажа на STATUS */
    if (s_tab == T_STATUS && s_fig) {
        s_phase++;
        int p = s_phase % 24;
        int bob = p < 12 ? p / 4 : (24 - p) / 4;          /* похитування 0..3 */
        lv_obj_set_y(s_fig, FIG_Y + bob);
        if (s_thumb)                                       /* «пампінг» пальця */
            lv_obj_set_y(s_thumb, THUMB_Y - ((s_phase % 8) < 4 ? 0 : 3));
        if (s_eyeR && s_wink) {                            /* підморгування ~кожні 3 с */
            bool wink = (s_phase % 26) < 3;
            lv_obj_add_flag(wink ? s_eyeR : s_wink, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(wink ? s_wink : s_eyeR, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* блимання курсора в заголовку */
    static int cnt = 0;
    if (++cnt % 4 == 0) {
        s_cursor = !s_cursor;
        char h[40];
        snprintf(h, sizeof(h), "%s %s", TABS[s_tab], s_cursor ? "_" : " ");
        lv_label_set_text(s_hdr, h);
    }

    /* «живі» дані на вкладці DATA */
    if (s_tab == T_DATA) {
        if (s_clk) { char c[8]; netcfg_get_clock(c, sizeof(c)); lv_label_set_text(s_clk, c); }
        if (s_rads && (cnt % 6 == 0)) {
            char r[12]; snprintf(r, sizeof(r), "%d rad/s", 8 + (int)(esp_random() % 12));
            lv_label_set_text(s_rads, r);
        }
    }
}

/* ---------- app ---------- */

static void pip_open(lv_obj_t *scr)
{
    s_tab = T_STATUS;
    s_scan_y = 0;
    s_cursor = true;

    lv_obj_set_style_bg_color(scr, lv_color_hex(PBG), 0);

    s_hdr = lv_label_create(scr);
    lv_obj_set_style_text_font(s_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hdr, lv_color_hex(PB), 0);
    lv_obj_align(s_hdr, LV_ALIGN_TOP_LEFT, 8, 6);

    /* рамка-лінія під заголовком */
    box(scr, 6, 26, 228, 2, 0, PBD);

    s_body = lv_obj_create(scr);
    lv_obj_set_size(s_body, 232, 200);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);

    s_foot = lv_label_create(scr);
    lv_obj_set_style_text_font(s_foot, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_foot, lv_color_hex(PB), 0);
    lv_obj_align(s_foot, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* скан-лінія (напівпрозора зелена смуга поверх усього) */
    s_scan = lv_obj_create(scr);
    lv_obj_set_size(s_scan, 240, 3);
    lv_obj_set_style_bg_color(s_scan, lv_color_hex(PB), 0);
    lv_obj_set_style_bg_opa(s_scan, 60, 0);
    lv_obj_set_style_border_width(s_scan, 0, 0);
    lv_obj_clear_flag(s_scan, LV_OBJ_FLAG_SCROLLABLE);

    render();
    s_timer = lv_timer_create(tick, 120, NULL);
}

static void pip_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
}

static void pip_btn(int btn)
{
    if (btn == BTN_LEFT_ID)  s_tab = (s_tab + T_COUNT - 1) % T_COUNT;
    else                     s_tab = (s_tab + 1) % T_COUNT;   /* центр і + — далі */
    render();
}

const app_t app_pipboy = {
    .name = "Pip-Boy",
    .open = pip_open, .close = pip_close, .on_btn = pip_btn,
};
