/*
 * Інтернет-радіо: станції + гучність через панель керування.
 * Ліва/права — рух фокуса по кнопках, середня — активувати.
 * Кнопки: ◄ станція | гучн− | ▶/⏹ | гучн+ | станція ►
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"
#include "audio.h"
#include "settings.h"

typedef struct { const char *name, *url; } station_t;
static const station_t ST[] = {
    { "SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "SomaFM Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3" },
    { "Radio Paradise",      "http://stream.radioparadise.com/mp3-128" },
    { "Хіт FM",              "https://online.hitfm.ua/HitFM" },
    { "Радіо ROKS",          "https://online.radioroks.ua/RadioROKS" },
};
#define N_ST (int)(sizeof(ST) / sizeof(ST[0]))

/* панель: 0 ◄станція 1 гучн− 2 play/stop 3 гучн+ 4 станція► */
#define N_CTRL 5
static int s_sel;       /* станція */
static int s_focus = 2; /* активна кнопка панелі */
static bool s_inited;

static lv_obj_t *s_name, *s_status, *s_info, *s_volbar, *s_volnum;
static lv_obj_t *s_ctrl[N_CTRL];
static lv_timer_t *s_timer;

static void render_sel(void) { lv_label_set_text(s_name, ST[s_sel].name); }

static void render_vol(void)
{
    int v = audio_get_volume();
    lv_bar_set_value(s_volbar, v, LV_ANIM_ON);
    lv_label_set_text_fmt(s_volnum, "%d%%", v * 100 / 256);
}

static void render_focus(void)
{
    for (int i = 0; i < N_CTRL; i++) {
        uint32_t c = (i == s_focus) ? 0x2563EB : 0x1A222C;
        if (i == 2 && audio_state() == AUDIO_PLAYING)
            c = (i == s_focus) ? 0x2563EB : 0x14532D;
        lv_obj_set_style_bg_color(s_ctrl[i], lv_color_hex(c), 0);
    }
}

static void app_poll(lv_timer_t *t)
{
    const char *st; uint32_t col = 0xC0C8D0;
    switch (audio_state()) {
    case AUDIO_STOPPED:    st = "Зупинено"; break;
    case AUDIO_CONNECTING: st = "З'єднання..."; col = 0xFACC15; break;
    case AUDIO_BUFFERING:  st = "Буферизація..."; col = 0xFACC15; break;
    case AUDIO_PLAYING:    st = "Грає"; col = 0x4ADE80; break;
    default:               st = "Помилка потоку"; col = 0xF87171; break;
    }
    lv_label_set_text(s_status, st);
    lv_obj_set_style_text_color(s_status, lv_color_hex(col), 0);
    lv_label_set_text(s_info, audio_info());

    /* кнопка Пуск/Стоп залежно від стану */
    bool playing = audio_state() == AUDIO_PLAYING ||
                   audio_state() == AUDIO_BUFFERING ||
                   audio_state() == AUDIO_CONNECTING;
    lv_label_set_text(lv_obj_get_child(s_ctrl[2], 0), playing ? "Стоп" : "Пуск");
    render_focus();
}

static lv_obj_t *make_ctrl(lv_obj_t *p, const char *txt, int w)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_set_size(b, w, 38);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_scrollbar_mode(b, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8ECF0), 0);
    lv_obj_center(l);
    return b;
}

static void radio_open(lv_obj_t *scr)
{
    if (!s_inited) { audio_init(); s_inited = true; }

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Інтернет-радіо");
    lv_obj_set_style_text_color(title, lv_color_hex(0x5A6672), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_name = lv_label_create(scr);
    lv_obj_set_style_text_font(s_name, &font_ua_20, 0);
    lv_obj_set_style_text_color(s_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(s_name, 224);
    lv_obj_set_style_text_align(s_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_name, LV_ALIGN_TOP_MID, 0, 30);
    render_sel();

    s_status = lv_label_create(scr);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 62);
    lv_label_set_text(s_status, "Зупинено");

    s_info = lv_label_create(scr);
    lv_obj_set_style_text_color(s_info, lv_color_hex(0x5A6672), 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_MID, 0, 86);
    lv_label_set_text(s_info, "");

    /* гучність */
    s_volbar = lv_bar_create(scr);
    lv_obj_set_size(s_volbar, 160, 10);
    lv_obj_align(s_volbar, LV_ALIGN_CENTER, -10, 22);
    lv_bar_set_range(s_volbar, 0, 256);
    lv_obj_set_style_bg_color(s_volbar, lv_color_hex(0x1A222C), 0);
    lv_obj_set_style_bg_color(s_volbar, lv_color_hex(0x35C4F0), LV_PART_INDICATOR);
    s_volnum = lv_label_create(scr);
    lv_obj_set_style_text_color(s_volnum, lv_color_hex(0xC0C8D0), 0);
    lv_obj_align(s_volnum, LV_ALIGN_CENTER, 82, 22);
    render_vol();

    /* панель керування */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 232, 48);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(bar, LV_OPA_0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 2, 0);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);

    s_ctrl[0] = make_ctrl(bar, "<<", 38);
    s_ctrl[1] = make_ctrl(bar, "гуч-", 46);
    s_ctrl[2] = make_ctrl(bar, "Пуск", 46);
    s_ctrl[3] = make_ctrl(bar, "гуч+", 46);
    s_ctrl[4] = make_ctrl(bar, ">>", 38);
    render_focus();

    s_timer = lv_timer_create(app_poll, 250, NULL);
}

static void radio_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    audio_stop();
}

static void play_current(void)
{
    if (!netcfg_is_connected()) { lv_label_set_text(s_status, "Немає Wi-Fi!"); return; }
    audio_play(ST[s_sel].url);
}

static void radio_btn(int btn)
{
    if (btn == BTN_LEFT_ID) { s_focus = (s_focus + N_CTRL - 1) % N_CTRL; render_focus(); return; }
    if (btn == BTN_RIGHT_ID) { s_focus = (s_focus + 1) % N_CTRL; render_focus(); return; }

    /* центр — активувати сфокусовану кнопку */
    switch (s_focus) {
    case 0: /* попередня станція */
        s_sel = (s_sel + N_ST - 1) % N_ST; render_sel();
        if (audio_state() != AUDIO_STOPPED) play_current();
        break;
    case 4: /* наступна станція */
        s_sel = (s_sel + 1) % N_ST; render_sel();
        if (audio_state() != AUDIO_STOPPED) play_current();
        break;
    case 1: /* тихіше */
        settings_set_volume(settings_volume() - 24); render_vol();
        break;
    case 3: /* гучніше */
        settings_set_volume(settings_volume() + 24); render_vol();
        break;
    case 2: { /* play / stop */
        audio_state_t a = audio_state();
        if (a == AUDIO_STOPPED || a == AUDIO_ERROR) play_current();
        else audio_stop();
        break;
    }
    }
}

const app_t app_radio = {
    .name = "Інтернет-радіо",
    .open = radio_open, .close = radio_close, .on_btn = radio_btn,
};
