# XingZhi Cube 1.54TFT (AiKoder S3 LCD) — повний технічний довідник

> Плата: ESP32-S3 N16R8, дисплей 1.54" IPS 240×240, клон з AliExpress.
> Ідентифікована як **XingZhi Cube 1.54TFT WiFi-варіант** з екосистеми
> [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
> (конфіг `main/boards/xingzhi-cube-1.54tft-wifi/config.h`).
> Існує варіант `-ml307` з 4G-модемом (UART GPIO 11/12) — під це металевий SIM-слот.
>
> Документ зібраний з експериментальних сесій 2026-07 (бринг-ап заліза,
> перебір пінів, аналіз стокового бінарника). Все з поміткою «підтверджено» —
> перевірено на живій платі.

---

## 1. Специфікація заліза

| Компонент | Деталі |
|---|---|
| SoC | ESP32-S3 R8 (QFN56, rev v0.2), dual-core Xtensa LX7 240 МГц |
| PSRAM | 8 МБ **OPI (octal)** у корпусі — критично для конфігів! |
| Flash | 16 МБ SPI NOR, quad (Boya на цьому екземплярі / Winbond W25Q128JVSQ на фото) |
| MAC | `10:20:ba:6c:d3:50` |
| Дисплей | 1.54" IPS 240×240, контролер **ST7789-сумісний** |
| Аудіо-вихід | I2S-підсилювач без enable-піна (тип MAX98357/NS4168) |
| Мікрофон | MEMS I2S (металевий корпус біля кнопки «40» / u.FL) |
| RGB LED | WS2812, GRB, під падом 44/RX0 |
| Живлення | USB-C (нативний USB S3), роз'єм АКБ V+/V−, зарядка (LED CHG), апаратна кнопка POWER |
| Порт | `/dev/cu.usbmodem2101` — вбудований USB-Serial/JTAG, без UART-моста |
| Слот | microSD/SIM (металевий, по центру). SD: FAT32 до 32 ГБ без додаткових бібліотек |
| Антена | u.FL |
| Кварц | 40 МГц |

---

## 2. Повна карта GPIO (усе підтверджено на платі)

### Дисплей ST7789 (підтверджено 2026-07-09)
| Сигнал | GPIO | Примітка |
|---|---|---|
| SCLK | **9** | |
| MOSI | **10** | |
| CS | **14** | перебором «працювало» і 21 — лінія, ймовірно, активна/підтягнута; правильний CS=14 за конфігом плати |
| DC | **8** | |
| RST | **18** | |
| BL | **13** | active high, LEDC PWM |

Параметри: **SPI mode 3 обов'язково** (SCK idle high — у mode 0 панель мовчить),
40 МГц стабільні, `invert_color(true)` як для всіх IPS,
драйвер `esp_lcd_new_panel_st7789`. Повний кадр через esp_lcd + DMA ≈ 25 FPS.
Контролер **не відповідає на RDDID (0x04)** — читання ID неможливе,
ідентифікація тільки візуальним перебором.

### Кнопки (active-low, внутрішній pullup; підтверджено 2026-07-10)
| Кнопка | GPIO |
|---|---|
| Ліва | 39 |
| Середня | 0 (BOOT) |
| Права | 40 |
| POWER | апаратна — вимикає живлення, НЕ GPIO |

### Аудіо (підтверджено 2026-07-10)
| Сигнал | GPIO | Параметри |
|---|---|---|
| Динамік DOUT | 7 | 24 кГц / 16 біт / моно; PWM-тон НЕ працює — тільки I2S |
| Динамік BCLK | 15 | |
| Динамік LRCK | 16 | |
| Мікрофон WS | 4 | 16 кГц, 32-бітні слоти, дані в лівому, моно |
| Мікрофон SCK | 5 | |
| Мікрофон DIN | 6 | зсув семплів `>> 12` дає нормальний рівень 16-біт |

### Інша периферія
| Що | GPIO | Примітка |
|---|---|---|
| WS2812 RGB LED | 48 | GRB, драйвер `espressif/led_strip` через RMT |
| Батарея ADC | 17 | ADC2 канал 6 — **ділиться з Wi-Fi**, читати рідко і стійко до помилок |
| Заряджання (детект) | 38 | |
| Роз'єм периферії SCL | 12 | 4-пін JST 1.25мм: GND/VCC/12/11 |
| Роз'єм периферії SDA | 11 | також придатний як цифровий вихід / ADC2 вхід |
| UART0 | 43 (TX), 44 (RX) | вільні від кнопок |

---

## 3. Середовища розробки

### ESP-IDF (основне, рекомендовано)
- **Версія: v5.5.x** (`~/esp/esp-idf`). Активація: `. ~/esp/esp-idf/export.sh`
- ⚠️ **v6.0.2 НЕсумісна з xiaozhi-esp32**: компонент `mqtt` винесено з ядра →
  `Failed to resolve component 'mqtt'`. Для xiaozhi тільки v5.x.
- Збірка: `idf.py set-target esp32s3 && idf.py -p /dev/cu.usbmodem2101 flash monitor`

### VS Code + розширення ESP-IDF
Ручне налаштування через `settings.json` (майстер може падати на QEMU — це не критично):
```json
{
  "idf.espIdfPath": "/Users/<user>/esp/esp-idf",
  "idf.toolsPath": "/Users/<user>/.espressif",
  "idf.pythonInstallPath": "/Users/<user>/.espressif"
}
```
Після зміни — **повний перезапуск VS Code**, інакше стара версія кешується.

### Arduino / PlatformIO (для швидких тестів)
Робочий `platformio.ini`:
```ini
[env:aikoder-s3-lcd]
platform = espressif32@6.9.0
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
board_build.flash_size = 16MB
board_build.partitions = default_16MB.csv
board_build.psram_type = opi
board_build.arduino.memory_type = qio_opi
build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_CDC_ON_BOOT=1
lib_deps =
    moononournation/GFX Library for Arduino@1.3.7
```
- ⚠️ **`memory_type = qio_opi` обов'язково** — без нього плата бут-лупиться
  (`RTC_SW_SYS_RST` нескінченно), бо OPI PSRAM конфліктує з дефолтним QSPI-конфігом.
- ⚠️ GFX Library **тільки 1.3.7** — новіші ламаються на `esp32-hal-periman.h`.
- Serial з'являється не одразу: після прошивки перепідключити монітор / натиснути RESET.

---

## 4. Критичні граблі (кожна коштувала годин дебагу)

### Пам'ять і DMA
- **Буфери дисплея LVGL — ТІЛЬКИ внутрішня RAM** (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`).
  У PSRAM SPI-DMA зависає, коли BLE-контролер вимикає кеш → `main` застрягає
  в `refr_sync_areas` назавжди.
- **BLE потребує ~34 КБ безперервної внутрішньої RAM** → перед `ble_start()`
  повний `esp_wifi_deinit()`. Wi-Fi і BLE на цій платі — взаємовиключні.
- **BLE ініціалізація — тільки в окремій задачі**, у UI-потоці — фриз.
- TLS/Wi-Fi буфери для важких мережевих застосунків — у PSRAM:
  `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`.

### Дисплей
- SPI mode 3, не 0. Крапка. У mode 0 нуль реакції, жодних помилок.
- `invert_color(true)` — без нього кольори негативні.
- RDDID не працює → автодетект контролера неможливий.

### LVGL
- `lv_label_set_text_fmt` **не підтримує `%f`** — float через `snprintf` +
  `lv_label_set_text`, інакше число просто не відображається.
- Вбудовані символи (`LV_SYMBOL_WIFI` тощо) рендеряться лише
  `&lv_font_montserrat_14`, кастомний кириличний шрифт їх не має.
- Кириличні шрифти: `lv_font_conv`, Montserrat Medium, діапазони
  ASCII + 0x400–0x4FF + тире/лапки/°/₴.

### Мережа
- **HTTP TX-буфер ≥ 2048** для довгих URL (Google TTS ~700 символів),
  інакше `HTTP_CLIENT: Out of buffer`.
- Перед Wi-Fi скануванням зупинити автоперепідключення
  (`esp_wifi_disconnect()` + пауза ~120 мс), інакше скан повертає 0 мереж.
- ADC2 (батарея) ділиться з Wi-Fi — читання може повертати помилку, це норма.

### Аудіо
- Динамік розуміє тільки I2S — PWM-тон на пін не дає звуку.
- Мікрофон: 32-бітні слоти, корисні дані в лівому каналі, зсув `>> 12`.

---

## 5. Історія ідентифікації (щоб не повторювати помилок)

1. **Хибний слід — JD9853.** Стоковий бінарник (`ai_koder_flash.bin`) містив
   рядки JD9853 і клас `AiKoderS3LcdBoard` (SPI3_HOST, `set_gap(0,24)`,
   роздільність 172×320) — це вело до тижня марних спроб. Реальна панель
   на платі — ST7789 240×240.
2. **Arduino-перебір пінів не працював** не через піни, а через бут-луп
   від неправильного PSRAM-конфігу + пізніше через хибний драйвер.
   Комбінація «правильні піни + ST7789 + mode 3» жодного разу не була
   спробувана одночасно.
3. Розпіновку остаточно знайдено через ESP-IDF `display-probe`
   (систематичний візуальний перебір), а плату ідентифіковано збігом
   з `xingzhi-cube-1.54tft-wifi/config.h` у xiaozhi-esp32.

**Мораль:** силкскрін біля FPC (8/9/10/13/18/21) — це просто список задіяних
GPIO, не порядок сигналів. Стоковий бінарник міг бути універсальним
(під кілька ревізій плати) — довіряти його рядкам не можна.

---

## 6. Структура прошивки проєкту

```
gadget/          — основна прошивка («робочий стіл робота»)
radio/           — окреме інтернет-радіо (MP3-стрім → Helix → I2S)
firmware/        — базовий каркас (LVGL 9 + ST7789 + кнопки-енкодер)
display-probe/   — діагностика (пошук пінів, тести SPI-швидкостей)
```

Ключові спільні модулі gadget: `netcfg.c` (Wi-Fi/NVS/SNTP, TZ Києва),
`audio.c` (I2S стрім + `audio_play_clip`), `led.c` (WS2812), `battery.c` (ADC2).

Архітектура застосунків: `app_t { name, open(scr), close(), on_btn(btn) }`,
масив `APPS[]` у `main.c`. Голосовий асистент: мікрофон → WAV (PSRAM) →
Groq Whisper (`language=uk`) → Llama 3.3 70B → екран → Google Translate TTS.

Секрети: `main/groq_key.h` (з `.example`, у `.gitignore`).

---

## 7. Швидкий старт з нуля

```bash
# ESP-IDF v5.5
cd ~/esp && git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3

# Проєкт
git clone https://github.com/MAXFELIXDIY/esp32-s3-1-54IPS-gadget.git
cd esp32-s3-1-54IPS-gadget/gadget
cp main/groq_key.h.example main/groq_key.h   # вписати ключ

. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodem2101 flash monitor
```
