/*
 * Календар: одна сторінка — один місяць. –/+ гортають місяці,
 * центр — повернутися до поточного місяця. Сьогоднішня дата виділена.
 * (Дата береться з системного годинника; коректна, коли час синхронізовано.)
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "apps.h"

static const char *MON[] = {
    "Січень", "Лютий", "Березень", "Квітень", "Травень", "Червень",
    "Липень", "Серпень", "Вересень", "Жовтень", "Листопад", "Грудень" };
static const char *WD[] = { "Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Нд" };

static int s_year, s_mon;              /* показуваний місяць (mon: 0..11) */
static int s_ty, s_tmon, s_tday;       /* сьогодні */
static lv_obj_t *s_title, *s_grid;

static int days_in_month(int y, int m)
{
    static const int d[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m];
}

/* стовпець першого дня місяця (0=Пн … 6=Нд) */
static int first_col(int y, int m)
{
    struct tm t = {0};
    t.tm_year = y - 1900; t.tm_mon = m; t.tm_mday = 1; t.tm_hour = 12;
    mktime(&t);                        /* нормалізує й заповнює tm_wday (0=Нд) */
    return (t.tm_wday + 6) % 7;         /* → 0=Пн */
}

static void render(void)
{
    lv_label_set_text_fmt(s_title, "%s %d", MON[s_mon], s_year);
    lv_obj_clean(s_grid);

    /* заголовок днів тижня */
    for (int c = 0; c < 7; c++) {
        lv_obj_t *l = lv_label_create(s_grid);
        lv_label_set_text(l, WD[c]);
        lv_obj_set_style_text_font(l, &font_ua_16, 0);
        lv_obj_set_style_text_color(l,
            lv_color_hex(c >= 5 ? 0xF87171 : 0x8A94A0), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, 30);
        lv_obj_set_pos(l, c * 33, 0);
    }

    int fc = first_col(s_year, s_mon);
    int dim = days_in_month(s_year, s_mon);
    bool cur_month = (s_year == s_ty && s_mon == s_tmon);

    for (int d = 1; d <= dim; d++) {
        int idx = fc + d - 1;
        int col = idx % 7, row = idx / 7;
        int x = col * 33, y = 24 + row * 26;

        lv_obj_t *cell = lv_obj_create(s_grid);
        lv_obj_set_size(cell, 30, 24);
        lv_obj_set_pos(cell, x, y);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
        bool today = cur_month && d == s_tday;
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(today ? 0x2563EB : 0x0B0F14), 0);

        lv_obj_t *l = lv_label_create(cell);
        lv_label_set_text_fmt(l, "%d", d);
        lv_obj_set_style_text_font(l, &font_ua_16, 0);
        lv_obj_set_style_text_color(l,
            lv_color_hex(today ? 0xFFFFFF : (col >= 5 ? 0xF87171 : 0xE8ECF0)), 0);
        lv_obj_center(l);
    }
}

static void cal_open(lv_obj_t *scr)
{
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    s_ty = tm.tm_year + 1900; s_tmon = tm.tm_mon; s_tday = tm.tm_mday;
    s_year = s_ty; s_mon = s_tmon;

    s_title = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title, &font_ua_20, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 6);

    s_grid = lv_obj_create(scr);
    lv_obj_set_size(s_grid, 231, 186);
    lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);

    render();
}

static void cal_close(void) { }

static void cal_btn(int btn)
{
    if (btn == BTN_LEFT_ID) {
        if (--s_mon < 0) { s_mon = 11; s_year--; }
    } else if (btn == BTN_RIGHT_ID) {
        if (++s_mon > 11) { s_mon = 0; s_year++; }
    } else {                              /* центр — до поточного місяця */
        s_year = s_ty; s_mon = s_tmon;
    }
    render();
}

const app_t app_calendar = {
    .name = "Календар",
    .open = cal_open, .close = cal_close, .on_btn = cal_btn,
};
