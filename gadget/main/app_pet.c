/*
 * Віртуальний улюбленець (tamagotchi-подібний).
 * Потреби: Їжа, Вода, Гра — з часом спадають; дії їх поповнюють.
 * Стан зберігається в NVS і «старіє» навіть коли пристрій вимкнено
 * (за реальним часом, якщо він синхронізований).
 *
 * Керування: ліва (–) — погодувати, центр — погратись, права (+) — напоїти;
 *            утримання центру — вихід.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "esp_random.h"
#include "lvgl.h"
#include "apps.h"

#define PETC 0x7FD4F5      /* тіло улюбленця */
#define DARK 0x0B0F14
#define C_FOOD 0xF59E42
#define C_WATER 0x35C4F0
#define C_FUN  0xFACC15

#define DECAY_SEC 20       /* -1 бал кожні 20 с на кожну потребу */
#define ACT_GAIN 25        /* +бал за дію */

static int s_food, s_water, s_fun;     /* 0..100 */
static int64_t s_last_ts;              /* час останнього збереження (сек) */

/* поведінка собачки */
typedef enum { B_IDLE, B_RUN, B_BEG, B_BARK, B_SAD } beh_t;
static beh_t s_beh;
static int s_beh_ttl;                  /* тіки, що лишились у поточній поведінці */
static int s_x;                        /* позиція під час бігу */

static lv_obj_t *s_body, *s_react;
static lv_obj_t *s_fill[3];
static lv_timer_t *s_timer;
static int s_phase;
static int s_react_ttl;                /* тіки, поки показуємо реакцію */

/* ---------- збереження ---------- */

static int clampi(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

static void pet_load(void)
{
    s_food = s_water = s_fun = 80;
    s_last_ts = 0;
    nvs_handle_t h;
    if (nvs_open("pet", NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "food", &v) == ESP_OK) s_food = v;
        if (nvs_get_i32(h, "water", &v) == ESP_OK) s_water = v;
        if (nvs_get_i32(h, "fun", &v) == ESP_OK) s_fun = v;
        nvs_get_i64(h, "ts", &s_last_ts);
        nvs_close(h);
    }
    /* «старіння» за реальним часом відсутності (якщо годинник дійсний) */
    int64_t now = (int64_t)time(NULL);
    if (s_last_ts > 0 && now > 1600000000LL && now > s_last_ts) {
        int dec = (int)((now - s_last_ts) / DECAY_SEC);
        if (dec > 0) {
            s_food = clampi(s_food - dec);
            s_water = clampi(s_water - dec);
            s_fun = clampi(s_fun - dec);
        }
    }
}

static void pet_save(void)
{
    nvs_handle_t h;
    if (nvs_open("pet", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "food", s_food);
    nvs_set_i32(h, "water", s_water);
    nvs_set_i32(h, "fun", s_fun);
    nvs_set_i64(h, "ts", (int64_t)time(NULL));
    nvs_commit(h);
    nvs_close(h);
}

/* ---------- примітиви ---------- */

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, int r, uint32_t c)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_size(o, w, h); lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    return o;
}

static lv_obj_t *dot(lv_obj_t *p, int x, int y, int d, uint32_t c)
{
    return box(p, x, y, d, d, LV_RADIUS_CIRCLE, c);
}

static int mood_min(void)
{
    int m = s_food; if (s_water < m) m = s_water; if (s_fun < m) m = s_fun;
    return m;
}

static void bars_update(void)
{
    int v[3] = { s_food, s_water, s_fun };
    for (int i = 0; i < 3; i++)
        lv_obj_set_width(s_fill[i], v[i] * 150 / 100);
}

/* ---------- малювання собачки (вид збоку, дивиться праворуч) ---------- */
#define EAR 0x4FB8E0

/* стоїть/біжить/гавкає/сумує; legrun=1 — біговий крок, sad — понурена, bark — паща */
static void draw_dog(int x, int ty, int legrun, bool bark, bool sad)
{
    lv_obj_t *f = s_body;
    bool blink = (s_phase % 20) < 1;
    int stride = legrun ? ((s_phase % 4 < 2) ? 6 : -6) : 0;

    /* хвіст (виляє, якщо не сумний) */
    int twy = sad ? 22 : (s_phase % 8 < 4 ? 8 : 2);
    box(f, x - 2, ty + twy, 12, 6, 3, PETC);
    /* тулуб */
    box(f, x + 8, ty + 16, 48, 22, 11, PETC);
    /* лапи */
    box(f, x + 12, ty + 36, 7, 16, 3, PETC);
    box(f, x + 22 + stride, ty + 36, 7, 16, 3, PETC);
    box(f, x + 40 - stride, ty + 36, 7, 16, 3, PETC);
    box(f, x + 50, ty + 36, 7, 16, 3, PETC);
    /* голова (нижче, якщо сумний) */
    int hy = sad ? ty + 12 : ty + 2;
    box(f, x + 42, hy - 2, 11, 15, 4, EAR);        /* вухо-флоп */
    dot(f, x + 42, hy + 4, 26, PETC);              /* голова */
    box(f, x + 62, hy + 15, 14, 9, 3, PETC);       /* морда */
    dot(f, x + 71, hy + 18, 5, DARK);              /* ніс */
    box(f, x + 54, hy + 12, 5, blink ? 2 : 6, 2, DARK);  /* око */
    if (bark)      box(f, x + 62, hy + 24, 14, 7, 2, DARK);   /* відкрита паща */
    else if (sad)  box(f, x + 65, hy + 25, 9, 3, 1, DARK);    /* сумний ротик */
}

/* стоїть на задніх лапах (жебрає) */
static void draw_beg(int x, int ty)
{
    lv_obj_t *f = s_body;
    bool blink = (s_phase % 20) < 1;
    box(f, x + 14, ty + 34, 34, 20, 10, PETC);     /* присід */
    box(f, x + 20, ty + 6, 28, 32, 13, PETC);      /* вертикальний тулуб */
    box(f, x + 18, ty + 16, 8, 16, 3, PETC);       /* передні лапки вгору */
    box(f, x + 42, ty + 16, 8, 16, 3, PETC);
    box(f, x + 16, ty - 12, 9, 14, 4, EAR);        /* вушка */
    box(f, x + 42, ty - 12, 9, 14, 4, EAR);
    dot(f, x + 16, ty - 8, 34, PETC);              /* голова (анфас) */
    box(f, x + 25, ty + 4, 5, blink ? 2 : 6, 2, DARK);   /* очі */
    box(f, x + 39, ty + 4, 5, blink ? 2 : 6, 2, DARK);
    dot(f, x + 31, ty + 12, 6, DARK);              /* ніс */
    box(f, x + 8, ty + 40, 12, 6, 3, PETC);        /* хвіст */
}

static void pick_behavior(void)
{
    if (mood_min() < 22) { s_beh = B_SAD; s_beh_ttl = 26; return; }
    uint32_t r = esp_random() % 100;
    if (r < 38)      { s_beh = B_IDLE; s_beh_ttl = 16 + esp_random() % 16; }
    else if (r < 64) { s_beh = B_RUN;  s_beh_ttl = 44; s_x = -70; }
    else if (r < 82) { s_beh = B_BEG;  s_beh_ttl = 18; }
    else             { s_beh = B_BARK; s_beh_ttl = 8; }
}

static void make_bar(lv_obj_t *scr, int y, const char *label, uint32_t col, int idx)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &font_ua_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_pos(l, 8, y - 2);
    box(scr, 74, y, 150, 12, 3, 0x1A222C);          /* тло смуги */
    s_fill[idx] = box(scr, 74, y, 1, 12, 3, col);   /* заповнення */
}

/* ---------- app ---------- */

static void react(const char *msg)
{
    lv_label_set_text(s_react, msg);
    lv_obj_clear_flag(s_react, LV_OBJ_FLAG_HIDDEN);
    s_react_ttl = 8;                    /* ~1 с */
}

static void tick(lv_timer_t *t)
{
    s_phase++;

    /* зміна поведінки, коли час вичерпано */
    if (--s_beh_ttl <= 0) pick_behavior();

    /* перемалювати собачку відповідно до поведінки */
    lv_obj_clean(s_body);
    int bob = (s_phase % 16 < 8) ? s_phase % 4 : 4 - s_phase % 4;
    switch (s_beh) {
    case B_RUN:
        s_x += 7; if (s_x > 182) s_x = -70;
        draw_dog(s_x, 8 + bob, 1, false, false);
        break;
    case B_BEG:
        draw_beg(86, 22);
        break;
    case B_BARK:
        draw_dog(86, 8, 0, (s_phase % 4 < 2), false);
        if (s_react_ttl == 0) react("Гав!");
        break;
    case B_SAD:
        draw_dog(86, 12, 0, false, true);
        break;
    default:  /* B_IDLE */
        draw_dog(86, 8 + bob, 0, false, false);
        break;
    }

    if (s_react_ttl > 0 && --s_react_ttl == 0)
        lv_obj_add_flag(s_react, LV_OBJ_FLAG_HIDDEN);

    /* спад потреб раз на DECAY_SEC (тік = 150 мс) */
    static int acc;
    if (++acc >= DECAY_SEC * 1000 / 150) {
        acc = 0;
        s_food = clampi(s_food - 1);
        s_water = clampi(s_water - 1);
        s_fun = clampi(s_fun - 1);
        bars_update();
        pet_save();
    }
}

static void pet_open(lv_obj_t *scr)
{
    pet_load();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Улюбленець");
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* контейнер собачки (малюється щотіку в tick) */
    s_body = lv_obj_create(scr);
    lv_obj_set_size(s_body, 240, 100);
    lv_obj_set_pos(s_body, 0, 24);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);

    s_beh = B_IDLE;
    pick_behavior();

    /* реакція (з'являється при дії) */
    s_react = lv_label_create(scr);
    lv_obj_set_style_text_font(s_react, &font_ua_20, 0);
    lv_obj_set_style_text_color(s_react, lv_color_hex(0x4ADE80), 0);
    lv_obj_align(s_react, LV_ALIGN_TOP_MID, 60, 30);
    lv_obj_add_flag(s_react, LV_OBJ_FLAG_HIDDEN);

    /* смуги потреб */
    make_bar(scr, 128, "Їжа",  C_FOOD, 0);
    make_bar(scr, 152, "Вода", C_WATER, 1);
    make_bar(scr, 176, "Гра",  C_FUN, 2);
    bars_update();

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "–  їсти     центр  гра     +  пити");
    lv_obj_set_style_text_font(hint, &font_ua_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8A94A0), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_timer = lv_timer_create(tick, 150, NULL);
}

static void pet_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    pet_save();
}

static void pet_btn(int btn)
{
    if (btn == BTN_LEFT_ID) {
        s_food = clampi(s_food + ACT_GAIN); react("Ням!");
        s_beh = B_BEG; s_beh_ttl = 16;          /* радісно жебрає */
    } else if (btn == BTN_RIGHT_ID) {
        s_water = clampi(s_water + ACT_GAIN); react("Буль!");
        s_beh = B_IDLE; s_beh_ttl = 12;
    } else {
        s_fun = clampi(s_fun + ACT_GAIN); react("Гав!");
        s_beh = B_RUN; s_beh_ttl = 44; s_x = -70; /* весело біжить */
    }
    bars_update();
    pet_save();
}

const app_t app_pet = {
    .name = "Улюбленець",
    .open = pet_open, .close = pet_close, .on_btn = pet_btn,
};
