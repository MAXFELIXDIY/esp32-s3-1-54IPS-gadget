/*
 * Калькулятор виразів: табло показує весь вираз під час набору,
 * обчислення (з дужками й пріоритетом * / над + -) — лише на «=».
 * Напр.: 2*3*4-10  або  (2+3)*4.
 * Керування (3 кнопки): –/+ рухають курсор по клавішах, центр — натиснути.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lvgl.h"
#include "apps.h"

static const char *KEYS[] = {
    "C", "(", ")", "/",
    "7", "8", "9", "*",
    "4", "5", "6", "-",
    "1", "2", "3", "+",
    "0", ".", "=", "<",   /* "<" — backspace (на екрані ⌫) */
};
#define NKEY (int)(sizeof(KEYS) / sizeof(KEYS[0]))

static lv_obj_t *s_disp, *s_cells[NKEY];
static int s_cur;

static char s_expr[48];      /* поточний вираз */
static bool s_done;          /* показано результат (наступна цифра — новий вираз) */

/* ---------- парсер виразу (рекурсивний спуск) ---------- */

static const char *P;
static bool s_perr;

static double p_expr(void);

static double p_factor(void)
{
    while (*P == ' ') P++;
    if (*P == '+') { P++; return p_factor(); }
    if (*P == '-') { P++; return -p_factor(); }
    if (*P == '(') {
        P++;
        double v = p_expr();
        while (*P == ' ') P++;
        if (*P == ')') P++; else s_perr = true;
        return v;
    }
    char *end;
    double v = strtod(P, &end);
    if (end == P) { s_perr = true; return 0; }
    P = end;
    return v;
}

static double p_term(void)
{
    double v = p_factor();
    for (;;) {
        while (*P == ' ') P++;
        if (*P == '*') { P++; v *= p_factor(); }
        else if (*P == '/') {
            P++;
            double d = p_factor();
            if (d == 0) { s_perr = true; return 0; }
            v /= d;
        } else break;
    }
    return v;
}

static double p_expr(void)
{
    double v = p_term();
    for (;;) {
        while (*P == ' ') P++;
        if (*P == '+') { P++; v += p_term(); }
        else if (*P == '-') { P++; v -= p_term(); }
        else break;
    }
    return v;
}

static double eval(const char *s, bool *err)
{
    P = s; s_perr = false;
    double v = p_expr();
    while (*P == ' ') P++;
    if (*P) s_perr = true;          /* залишок = синтаксична помилка */
    *err = s_perr;
    return v;
}

/* ---------- введення ---------- */

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static void press(const char *k)
{
    char c = k[0];

    if (c == 'C') { s_expr[0] = 0; s_done = false; }
    else if (c == '<') {                 /* backspace */
        if (!strcmp(s_expr, "Error")) s_expr[0] = 0;
        int n = strlen(s_expr);
        if (n > 0) s_expr[n - 1] = 0;
        s_done = false;
    }
    else if (c == '=') {
        if (!s_expr[0]) return;
        bool err;
        double r = eval(s_expr, &err);
        if (err) strcpy(s_expr, "Error");
        else snprintf(s_expr, sizeof(s_expr), "%.10g", r);
        s_done = true;
    } else {
        /* після результату: цифра/дужка — новий вираз; оператор — продовжити з нього */
        if (s_done) {
            if (is_digit(c) || c == '(') s_expr[0] = 0;
            s_done = false;
        }
        if (!strcmp(s_expr, "Error")) s_expr[0] = 0;
        int n = strlen(s_expr);
        if (n < (int)sizeof(s_expr) - 1) { s_expr[n] = c; s_expr[n + 1] = 0; }
    }
    lv_label_set_text(s_disp, s_expr[0] ? s_expr : "0");
}

/* ---------- UI ---------- */

static void highlight(void)
{
    for (int i = 0; i < NKEY; i++)
        lv_obj_set_style_bg_color(s_cells[i],
            lv_color_hex(i == s_cur ? 0x2563EB : 0x1A222C), 0);
}

static void calc_open(lv_obj_t *scr)
{
    s_expr[0] = 0; s_done = false;
    s_cur = 4;   /* «7» */

    lv_obj_t *board = lv_obj_create(scr);
    lv_obj_set_size(board, 228, 44);
    lv_obj_align(board, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_radius(board, 6, 0);
    lv_obj_set_style_bg_color(board, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_border_width(board, 1, 0);
    lv_obj_set_style_border_color(board, lv_color_hex(0x2A3540), 0);
    lv_obj_set_style_pad_all(board, 6, 0);
    lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(board, LV_SCROLLBAR_MODE_OFF);

    s_disp = lv_label_create(board);
    lv_label_set_text(s_disp, "0");
    lv_obj_set_style_text_font(s_disp, &font_ua_20, 0);
    lv_obj_set_style_text_color(s_disp, lv_color_hex(0x4ADE80), 0);
    /* довгий вираз показуємо «хвостом» (праворуч), зайве зліва обрізає табло */
    lv_obj_align(s_disp, LV_ALIGN_RIGHT_MID, 0, 0);

    for (int i = 0; i < NKEY; i++) {
        int col = i % 4, row = i / 4;
        lv_obj_t *c = lv_obj_create(scr);
        lv_obj_set_size(c, 54, 30);
        lv_obj_set_pos(c, 6 + col * 57, 58 + row * 33);
        lv_obj_set_style_radius(c, 6, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
        s_cells[i] = c;

        lv_obj_t *l = lv_label_create(c);
        if (KEYS[i][0] == '<') {                 /* backspace — символ ⌫ */
            lv_label_set_text(l, LV_SYMBOL_BACKSPACE);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        } else {
            lv_label_set_text(l, KEYS[i]);
            lv_obj_set_style_text_font(l, &font_ua_20, 0);
        }
        bool oper = strchr("+-*/=C()<", KEYS[i][0]) != NULL;
        lv_obj_set_style_text_color(l, lv_color_hex(oper ? 0xFACC15 : 0xE8ECF0), 0);
        lv_obj_center(l);
    }
    highlight();
}

static void calc_close(void) { }

static void calc_btn(int btn)
{
    if (btn == BTN_LEFT_ID)  { s_cur = (s_cur + NKEY - 1) % NKEY; highlight(); }
    else if (btn == BTN_RIGHT_ID) { s_cur = (s_cur + 1) % NKEY; highlight(); }
    else press(KEYS[s_cur]);
}

const app_t app_calc = {
    .name = "Калькулятор",
    .open = calc_open, .close = calc_close, .on_btn = calc_btn,
};
