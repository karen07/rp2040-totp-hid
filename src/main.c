/*
 * RP2040 USB HID keyboard + WebHID config channel + DS3231 + TOTP + flash config
 *
 * Features:
 *  - BOOTSEL button via QSPI CS trick
 *  - Onboard WS2812 status LED: red blink on RTC/OSF fault, green while pressed
 *  - Single click  -> type current TOTP
 *  - Double click  -> type password
 *  - UART0 debug + time set:
 *        TIME  YYYY-MM-DD HH:MM:SS
 *        EPOCH <seconds>
 *  - Browser via WebHID can:
 *        SET time
 *        SET/CLEAR password
 *        SET/CLEAR TOTP secret
 *        GET status
 *  - Password + TOTP secret persist in last flash sector
 *
 * Notes:
 *  - Time is stored in DS3231, not flash
 *  - Secret/password are stored in plaintext in flash
 *  - Requires matching usb_descriptors.{c,h} and tusb_config.h changes
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "bsp/board_api.h"
#include "tusb.h"

#include "pico/stdlib.h"
#include "pico/platform.h"
#include "hardware/uart.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/regs/addressmap.h"

#include "usb_descriptors.h"
#include "ws2812.pio.h"

//--------------------------------------------------------------------+
// CONFIG
//--------------------------------------------------------------------+

#define DEBOUNCE_MS 30
#define DBLCLICK_MS 350

#define TOTP_TIME_STEP 30
#define TOTP_DIGITS 6

#define DBG_UART_ID uart0
#define DBG_UART_BAUD 115200
#define DBG_UART_TX 12
#define DBG_UART_RX 13
#define DBG_UART_OK 1

#define RTC_I2C i2c1
#define RTC_I2C_BAUD 100000u
#define RTC_SDA_GPIO 10
#define RTC_SCL_GPIO 11
#define DS3231_ADDR 0x68

#define RTC_STATUS_POLL_MS 1000u
#define RTC_ERROR_BLINK_MS 500u
#define LED_ERROR_RED 24u
#define LED_BUTTON_GREEN 12u

#ifndef PICO_DEFAULT_WS2812_PIN
#define PICO_DEFAULT_WS2812_PIN 16
#endif
#define STATUS_LED_GPIO PICO_DEFAULT_WS2812_PIN

#define TIME_CMD_MAX_LINE 128

#define APP_MAX_PASSWORD_LEN 128
#define APP_MAX_TOTP_SECRET_LEN 128

#define APP_CONFIG_MAGIC 0x544F5450u // "TOTP"
#define APP_CONFIG_VERSION 1u

#ifndef PICO_FLASH_SIZE_BYTES
#error PICO_FLASH_SIZE_BYTES must be defined by pico-sdk
#endif

#define APP_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define APP_FLASH_ADDR (XIP_BASE + APP_FLASH_OFFSET)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    char totp_secret_b32[APP_MAX_TOTP_SECRET_LEN];
    char password[APP_MAX_PASSWORD_LEN];
    uint32_t crc32;
} app_config_t;

static app_config_t g_cfg;
static uint8_t g_flash_sector_buf[FLASH_SECTOR_SIZE];

static bool s_click_pending = false;
static uint32_t s_click_deadline_ms = 0;
static bool s_button_pressed = false;

static bool s_rtc_status_valid = false;
static bool s_rtc_osf = false;

static PIO s_led_pio = NULL;
static uint s_led_sm = 0;
static uint s_led_offset = 0;
static bool s_led_ready = false;

static void hid_task(void);
static void rtc_status_poll_task(bool force);
static void status_led_task(void);

//--------------------------------------------------------------------+
// Browser protocol
//--------------------------------------------------------------------+

enum {
    CMD_SET_TIME_UNIX = 0x01,
    CMD_SET_PASSWORD = 0x02,
    CMD_SET_TOTP_SECRET = 0x03,
    CMD_GET_STATUS = 0x04,
    CMD_CLEAR_PASSWORD = 0x05,
    CMD_CLEAR_SECRET = 0x06,
};

enum {
    RSP_OK = 0x80,
    RSP_ERR = 0x81,
    RSP_STATUS = 0x82,
};

enum {
    ERR_BAD_CMD = 1,
    ERR_BAD_LENGTH = 2,
    ERR_BAD_TIME = 3,
    ERR_BAD_SECRET = 4,
    ERR_FLASH_WRITE = 5,
    ERR_HID_NOT_READY = 6,
};

//--------------------------------------------------------------------+
// UART debug
//--------------------------------------------------------------------+

static void dbg_uart_init(void)
{
#if DBG_UART_OK
    uart_init(DBG_UART_ID, DBG_UART_BAUD);
    gpio_set_function(DBG_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(DBG_UART_RX, GPIO_FUNC_UART);
    uart_set_format(DBG_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(DBG_UART_ID, false, false);
    uart_set_fifo_enabled(DBG_UART_ID, true);
    uart_puts(DBG_UART_ID, "\r\n[RP2040] USB HID KBD + WebHID + DS3231 + TOTP ready\r\n");
#endif
}

static void dbg_printf(const char *fmt, ...)
{
#if DBG_UART_OK
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uart_puts(DBG_UART_ID, buf);
#else
    (void)fmt;
#endif
}

//--------------------------------------------------------------------+
// Onboard WS2812 status LED
//--------------------------------------------------------------------+

static bool app_led_try_init(PIO pio)
{
    if (!pio_can_add_program(pio, &ws2812_program))
        return false;

    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0)
        return false;

    s_led_pio = pio;
    s_led_sm = (uint)sm;
    s_led_offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, s_led_sm, s_led_offset, STATUS_LED_GPIO, 800000.0f, false);
    s_led_ready = true;
    return true;
}

static bool app_led_init(void)
{
    if (!app_led_try_init(pio0) && !app_led_try_init(pio1))
        return false;

    dbg_printf("[led] WS2812 initialized on GP%u\r\n", (unsigned)STATUS_LED_GPIO);
    return true;
}

static void app_led_write_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_led_ready)
        return;

    // WS2812 expects GRB byte order. The PIO state machine consumes the top 24 bits.
    uint32_t grb = ((uint32_t)green << 16) | ((uint32_t)red << 8) | (uint32_t)blue;
    pio_sm_put_blocking(s_led_pio, s_led_sm, grb << 8u);
}

//--------------------------------------------------------------------+
// Flash config storage
//--------------------------------------------------------------------+

static uint32_t crc32_ieee(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }

    return ~crc;
}

static void app_cfg_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = APP_CONFIG_MAGIC;
    cfg->version = APP_CONFIG_VERSION;
}

static uint32_t app_cfg_calc_crc(const app_config_t *cfg)
{
    return crc32_ieee(cfg, offsetof(app_config_t, crc32));
}

static bool app_cfg_is_valid(const app_config_t *cfg)
{
    if (!cfg)
        return false;

    if (cfg->magic != APP_CONFIG_MAGIC)
        return false;

    if (cfg->version != APP_CONFIG_VERSION)
        return false;

    if (cfg->totp_secret_b32[APP_MAX_TOTP_SECRET_LEN - 1] != '\0')
        return false;

    if (cfg->password[APP_MAX_PASSWORD_LEN - 1] != '\0')
        return false;

    return cfg->crc32 == app_cfg_calc_crc(cfg);
}

static void app_cfg_load(void)
{
    const app_config_t *flash_cfg = (const app_config_t *)APP_FLASH_ADDR;

    if (app_cfg_is_valid(flash_cfg)) {
        memcpy(&g_cfg, flash_cfg, sizeof(g_cfg));
        dbg_printf("[cfg] loaded from flash\r\n");
    } else {
        app_cfg_defaults(&g_cfg);
        dbg_printf("[cfg] flash empty/invalid, using defaults\r\n");
    }
}

static bool app_cfg_save(void)
{
    app_config_t tmp;
    memcpy(&tmp, &g_cfg, sizeof(tmp));
    tmp.magic = APP_CONFIG_MAGIC;
    tmp.version = APP_CONFIG_VERSION;
    tmp.crc32 = app_cfg_calc_crc(&tmp);

    memset(g_flash_sector_buf, 0xFF, sizeof(g_flash_sector_buf));
    memcpy(g_flash_sector_buf, &tmp, sizeof(tmp));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(APP_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(APP_FLASH_OFFSET, g_flash_sector_buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    const app_config_t *flash_cfg = (const app_config_t *)APP_FLASH_ADDR;
    bool ok = app_cfg_is_valid(flash_cfg);

    if (ok) {
        memcpy(&g_cfg, flash_cfg, sizeof(g_cfg));
        dbg_printf("[cfg] saved to flash\r\n");
    } else {
        dbg_printf("[cfg] save verify failed\r\n");
    }

    return ok;
}

//--------------------------------------------------------------------+
// I2C + DS3231
//--------------------------------------------------------------------+

static void rtc_i2c_init(void)
{
    i2c_init(RTC_I2C, RTC_I2C_BAUD);

    gpio_set_function(RTC_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(RTC_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(RTC_SDA_GPIO);
    gpio_pull_up(RTC_SCL_GPIO);

    dbg_printf("[i2c] init I2C1 SDA=GP%d SCL=GP%d baud=%u\r\n", RTC_SDA_GPIO, RTC_SCL_GPIO,
               (unsigned)RTC_I2C_BAUD);
}

static inline uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)((bcd & 0x0F) + 10u * (bcd >> 4));
}

static inline uint8_t bin_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static bool ds3231_read_time(uint8_t *sec, uint8_t *min, uint8_t *hour, uint8_t *day,
                             uint8_t *month, uint16_t *year)
{
    uint8_t reg = 0x00;
    uint8_t buf[7] = { 0 };

    int w = i2c_write_blocking(RTC_I2C, DS3231_ADDR, &reg, 1, true);
    if (w != 1)
        return false;

    int r = i2c_read_blocking(RTC_I2C, DS3231_ADDR, buf, 7, false);
    if (r != 7)
        return false;

    uint8_t raw_sec = buf[0] & 0x7F;
    uint8_t raw_min = buf[1] & 0x7F;
    uint8_t raw_hour = buf[2];

    uint8_t h;
    if (raw_hour & 0x40) {
        uint8_t hr12 = bcd_to_bin(raw_hour & 0x1F);
        bool pm = (raw_hour & 0x20) != 0;
        if (hr12 == 12)
            h = pm ? 12 : 0;
        else
            h = pm ? (uint8_t)(hr12 + 12) : hr12;
    } else {
        h = bcd_to_bin(raw_hour & 0x3F);
    }

    uint8_t d = bcd_to_bin(buf[4] & 0x3F);
    uint8_t mo = bcd_to_bin(buf[5] & 0x1F);
    uint8_t yy = bcd_to_bin(buf[6]);

    if (sec)
        *sec = bcd_to_bin(raw_sec);
    if (min)
        *min = bcd_to_bin(raw_min);
    if (hour)
        *hour = h;
    if (day)
        *day = d;
    if (month)
        *month = mo;
    if (year)
        *year = (uint16_t)(2000u + yy);

    return true;
}

static bool ds3231_write_regs(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t tmp[1 + 7];
    if (!data || len > 7)
        return false;

    tmp[0] = reg;
    memcpy(&tmp[1], data, len);

    int w = i2c_write_blocking(RTC_I2C, DS3231_ADDR, tmp, (int)(1 + len), false);
    return w == (int)(1 + len);
}

static bool ds3231_set_time_utc(int year, int mon, int mday, int hour, int min, int sec)
{
    if (year < 2000 || year > 2099)
        return false;
    if (mon < 1 || mon > 12)
        return false;
    if (mday < 1 || mday > 31)
        return false;
    if (hour < 0 || hour > 23)
        return false;
    if (min < 0 || min > 59)
        return false;
    if (sec < 0 || sec > 59)
        return false;

    uint8_t w[7];
    w[0] = (bin_to_bcd((uint8_t)sec) & 0x7F);
    w[1] = (bin_to_bcd((uint8_t)min) & 0x7F);
    w[2] = (bin_to_bcd((uint8_t)hour) & 0x3F);
    w[3] = 1;
    w[4] = (bin_to_bcd((uint8_t)mday) & 0x3F);
    w[5] = (bin_to_bcd((uint8_t)mon) & 0x1F);
    w[6] = bin_to_bcd((uint8_t)(year - 2000));

    return ds3231_write_regs(0x00, w, sizeof(w));
}

static bool ds3231_read_status(uint8_t *out_status)
{
    if (!out_status)
        return false;

    uint8_t reg = 0x0F;
    uint8_t st = 0;

    int w = i2c_write_blocking(RTC_I2C, DS3231_ADDR, &reg, 1, true);
    if (w != 1)
        return false;

    int r = i2c_read_blocking(RTC_I2C, DS3231_ADDR, &st, 1, false);
    if (r != 1)
        return false;

    *out_status = st;
    return true;
}

static bool ds3231_osf_is_set(bool *out_set)
{
    if (!out_set)
        return false;

    uint8_t st = 0;
    if (!ds3231_read_status(&st))
        return false;

    *out_set = (st & 0x80u) != 0;
    return true;
}

static void rtc_status_poll_task(bool force)
{
    static uint32_t next_poll_ms = 0;
    uint32_t now = board_millis();

    if (!force && (int32_t)(now - next_poll_ms) < 0)
        return;

    next_poll_ms = now + RTC_STATUS_POLL_MS;

    bool osf = false;
    bool valid = ds3231_osf_is_set(&osf);
    bool changed = (valid != s_rtc_status_valid) || (valid && osf != s_rtc_osf);

    s_rtc_status_valid = valid;
    s_rtc_osf = valid && osf;

    if (changed) {
        if (!valid)
            dbg_printf("[rtc] status unavailable -> LED fault\r\n");
        else
            dbg_printf("[rtc] OSF=%d -> LED %s\r\n", osf ? 1 : 0, osf ? "red blink" : "normal");
    }
}

static void ds3231_print_power_status(void)
{
    rtc_status_poll_task(true);

    if (!s_rtc_status_valid) {
        dbg_printf("[rtc] status read failed\r\n");
        return;
    }

    dbg_printf("[rtc] OSF=%d (%s)\r\n", s_rtc_osf ? 1 : 0,
               s_rtc_osf ? "oscillator stop -> power drop / time may be invalid" : "ok");
}

static bool ds3231_clear_osf(void)
{
    uint8_t reg = 0x0F;
    uint8_t st = 0;

    int w = i2c_write_blocking(RTC_I2C, DS3231_ADDR, &reg, 1, true);
    if (w != 1)
        return false;

    int r = i2c_read_blocking(RTC_I2C, DS3231_ADDR, &st, 1, false);
    if (r != 1)
        return false;

    st &= (uint8_t)~0x80;
    bool ok = ds3231_write_regs(0x0F, &st, 1);
    if (ok) {
        s_rtc_status_valid = true;
        s_rtc_osf = false;
    }
    return ok;
}

static void status_led_task(void)
{
    static uint32_t last_rgb = UINT32_MAX;
    uint32_t now = board_millis();
    uint32_t rgb = 0;

    // An unreadable RTC status is also a fault: TOTP time cannot be trusted.
    if (!s_rtc_status_valid || s_rtc_osf) {
        bool blink_on = ((now / RTC_ERROR_BLINK_MS) & 1u) == 0;
        if (blink_on)
            rgb = (uint32_t)LED_ERROR_RED << 16;
    } else if (s_button_pressed) {
        rgb = (uint32_t)LED_BUTTON_GREEN << 8;
    }

    if (rgb == last_rgb)
        return;

    last_rgb = rgb;
    app_led_write_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

static void ds3231_print_time(void)
{
    uint8_t s, m, h, d, mo;
    uint16_t y;

    if (!ds3231_read_time(&s, &m, &h, &d, &mo, &y)) {
        dbg_printf("[rtc] read failed (no ACK / wiring / addr?)\r\n");
        return;
    }

    dbg_printf("[rtc] %04u-%02u-%02u %02u:%02u:%02u\r\n", (unsigned)y, (unsigned)mo, (unsigned)d,
               (unsigned)h, (unsigned)m, (unsigned)s);
}

//--------------------------------------------------------------------+
// UNIX time conversions (UTC)
//--------------------------------------------------------------------+

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int64_t unix_time_from_ymdhms_utc(int year, int mon, int mday, int hour, int min, int sec)
{
    int64_t days = days_from_civil(year, (unsigned)mon, (unsigned)mday);
    return days * 86400LL + (int64_t)hour * 3600LL + (int64_t)min * 60LL + (int64_t)sec;
}

static bool is_leap(int y)
{
    return ((y % 4) == 0) && (((y % 100) != 0) || ((y % 400) == 0));
}

static bool ymdhms_from_unix_utc(int64_t t, int *Y, int *M, int *D, int *h, int *m, int *s)
{
    if (!Y || !M || !D || !h || !m || !s)
        return false;
    if (t < 0)
        return false;

    int64_t sec = t;

    *s = (int)(sec % 60);
    sec /= 60;
    *m = (int)(sec % 60);
    sec /= 60;
    *h = (int)(sec % 24);
    sec /= 24;

    int64_t days = sec;

    int y = 1970;
    while (1) {
        int dy = is_leap(y) ? 366 : 365;
        if (days >= dy) {
            days -= dy;
            y++;
        } else {
            break;
        }
        if (y > 2400)
            return false;
    }

    static const int mdays_norm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int mon = 1;
    for (int i = 0; i < 12; i++) {
        int dm = mdays_norm[i];
        if (i == 1 && is_leap(y))
            dm = 29;
        if (days >= dm) {
            days -= dm;
            mon++;
        } else {
            break;
        }
    }

    *Y = y;
    *M = mon;
    *D = (int)days + 1;
    return true;
}

static bool ds3231_get_unix_time_utc(int64_t *out_unix)
{
    if (!out_unix)
        return false;

    uint8_t s, m, h, d, mo;
    uint16_t y;

    if (!ds3231_read_time(&s, &m, &h, &d, &mo, &y))
        return false;

    *out_unix = unix_time_from_ymdhms_utc((int)y, (int)mo, (int)d, (int)h, (int)m, (int)s);
    return true;
}

//--------------------------------------------------------------------+
// Base32 RFC4648 decode
//--------------------------------------------------------------------+

static int base32_decode_rfc4648(const char *in, uint8_t *out, size_t *out_len)
{
    if (!in || !out || !out_len)
        return -1;

    size_t cap = *out_len;
    size_t outpos = 0;

    uint32_t buffer = 0;
    int bits_left = 0;

    for (const char *p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (c == '=' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-') {
            continue;
        }

        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');

        int val = -1;
        if (c >= 'A' && c <= 'Z')
            val = (int)(c - 'A');
        else if (c >= '2' && c <= '7')
            val = 26 + (int)(c - '2');
        else
            return -2;

        buffer = (buffer << 5) | (uint32_t)val;
        bits_left += 5;

        while (bits_left >= 8) {
            bits_left -= 8;
            if (outpos >= cap)
                return -3;
            out[outpos++] = (uint8_t)((buffer >> bits_left) & 0xFF);
        }
    }

    *out_len = outpos;
    return 0;
}

//--------------------------------------------------------------------+
// Minimal SHA1 + HMAC-SHA1
//--------------------------------------------------------------------+

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t buf_len;
} sha1_ctx_t;

static uint32_t rol32(uint32_t x, unsigned r)
{
    return (x << r) | (x >> (32 - r));
}

static void sha1_init(sha1_ctx_t *c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->len = 0;
    c->buf_len = 0;
}

static void sha1_block(sha1_ctx_t *c, const uint8_t b[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)b[i * 4 + 0] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
               ((uint32_t)b[i * 4 + 2] << 8) | ((uint32_t)b[i * 4 + 3] << 0);
    }

    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = c->h[0], b0 = c->h[1], c0 = c->h[2], d = c->h[3], e = c->h[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b0 & c0) | ((~b0) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = (b0 ^ c0 ^ d);
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b0 & c0) | (b0 & d) | (c0 & d);
            k = 0x8F1BBCDCu;
        } else {
            f = (b0 ^ c0 ^ d);
            k = 0xCA62C1D6u;
        }

        uint32_t temp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c0;
        c0 = rol32(b0, 30);
        b0 = a;
        a = temp;
    }

    c->h[0] += a;
    c->h[1] += b0;
    c->h[2] += c0;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_update(sha1_ctx_t *c, const uint8_t *data, size_t len)
{
    c->len += (uint64_t)len * 8u;

    while (len > 0) {
        size_t space = 64 - c->buf_len;
        size_t take = (len < space) ? len : space;
        memcpy(&c->buf[c->buf_len], data, take);
        c->buf_len += take;
        data += take;
        len -= take;

        if (c->buf_len == 64) {
            sha1_block(c, c->buf);
            c->buf_len = 0;
        }
    }
}

static void sha1_final(sha1_ctx_t *c, uint8_t out[20])
{
    c->buf[c->buf_len++] = 0x80;

    if (c->buf_len > 56) {
        while (c->buf_len < 64)
            c->buf[c->buf_len++] = 0x00;
        sha1_block(c, c->buf);
        c->buf_len = 0;
    }

    while (c->buf_len < 56)
        c->buf[c->buf_len++] = 0x00;

    uint64_t L = c->len;
    c->buf[56] = (uint8_t)(L >> 56);
    c->buf[57] = (uint8_t)(L >> 48);
    c->buf[58] = (uint8_t)(L >> 40);
    c->buf[59] = (uint8_t)(L >> 32);
    c->buf[60] = (uint8_t)(L >> 24);
    c->buf[61] = (uint8_t)(L >> 16);
    c->buf[62] = (uint8_t)(L >> 8);
    c->buf[63] = (uint8_t)(L >> 0);

    sha1_block(c, c->buf);

    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i] >> 0);
    }
}

static void hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                      uint8_t out[20])
{
    uint8_t k0[64];
    memset(k0, 0, sizeof(k0));

    if (key_len > 64) {
        sha1_ctx_t c;
        uint8_t kh[20];
        sha1_init(&c);
        sha1_update(&c, key, key_len);
        sha1_final(&c, kh);
        memcpy(k0, kh, 20);
    } else {
        memcpy(k0, key, key_len);
    }

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36);
        opad[i] = (uint8_t)(k0[i] ^ 0x5c);
    }

    uint8_t inner[20];
    sha1_ctx_t c;
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, msg, msg_len);
    sha1_final(&c, inner);

    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

static uint32_t pow10_u32(int digits)
{
    uint32_t p = 1;
    for (int i = 0; i < digits; i++)
        p *= 10;
    return p;
}

static uint32_t hotp_truncate(const uint8_t *hmac20)
{
    uint8_t offset = (uint8_t)(hmac20[19] & 0x0F);
    uint32_t bin =
        ((uint32_t)(hmac20[offset] & 0x7F) << 24) | ((uint32_t)(hmac20[offset + 1] & 0xFF) << 16) |
        ((uint32_t)(hmac20[offset + 2] & 0xFF) << 8) | ((uint32_t)(hmac20[offset + 3] & 0xFF) << 0);
    return bin;
}

static bool totp_generate(const uint8_t *key, size_t key_len, int64_t unix_time, int digits,
                          uint32_t *out_code)
{
    if (!key || key_len == 0 || !out_code)
        return false;
    if (digits < 1 || digits > 10)
        return false;
    if (unix_time < 0)
        return false;

    uint64_t counter = (uint64_t)(unix_time / TOTP_TIME_STEP);

    uint8_t msg[8];
    msg[0] = (uint8_t)(counter >> 56);
    msg[1] = (uint8_t)(counter >> 48);
    msg[2] = (uint8_t)(counter >> 40);
    msg[3] = (uint8_t)(counter >> 32);
    msg[4] = (uint8_t)(counter >> 24);
    msg[5] = (uint8_t)(counter >> 16);
    msg[6] = (uint8_t)(counter >> 8);
    msg[7] = (uint8_t)(counter >> 0);

    uint8_t hmac[20];
    hmac_sha1(key, key_len, msg, sizeof(msg), hmac);

    uint32_t bin = hotp_truncate(hmac);
    uint32_t mod = pow10_u32(digits);
    *out_code = bin % mod;
    return true;
}

//--------------------------------------------------------------------+
// BOOTSEL BUTTON READ
//--------------------------------------------------------------------+

static bool __no_inline_not_in_flash_func(get_bootsel_button)(void)
{
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    for (volatile int i = 0; i < 1000; ++i) {
    }

#if PICO_RP2040
#define CS_BIT (1u << 1)
#else
#define CS_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif

    bool pressed = !(sio_hw->gpio_hi_in & CS_BIT);

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}

static inline bool button_pressed_raw(void)
{
    return get_bootsel_button();
}

//--------------------------------------------------------------------+
// ASCII -> HID
//--------------------------------------------------------------------+

static bool ascii_to_hid(char c, uint8_t *mod, uint8_t *key)
{
    *mod = 0;
    *key = 0;

    if (c >= 'a' && c <= 'z') {
        *key = (uint8_t)(HID_KEY_A + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *mod = KEYBOARD_MODIFIER_LEFTSHIFT;
        *key = (uint8_t)(HID_KEY_A + (c - 'A'));
        return true;
    }
    if (c >= '1' && c <= '9') {
        *key = (uint8_t)(HID_KEY_1 + (c - '1'));
        return true;
    }
    if (c == '0') {
        *key = HID_KEY_0;
        return true;
    }

    switch (c) {
    case ' ':
        *key = HID_KEY_SPACE;
        return true;
    case '\n':
        *key = HID_KEY_ENTER;
        return true;
    case '=':
        *key = HID_KEY_EQUAL;
        return true;
    case '+':
        *mod = KEYBOARD_MODIFIER_LEFTSHIFT;
        *key = HID_KEY_EQUAL;
        return true;
    case '-':
        *key = HID_KEY_MINUS;
        return true;
    case '_':
        *mod = KEYBOARD_MODIFIER_LEFTSHIFT;
        *key = HID_KEY_MINUS;
        return true;
    case '.':
        *key = HID_KEY_PERIOD;
        return true;
    case ',':
        *key = HID_KEY_COMMA;
        return true;
    case '/':
        *key = HID_KEY_SLASH;
        return true;
    case '\\':
        *key = HID_KEY_BACKSLASH;
        return true;
    case '\t':
        *key = HID_KEY_TAB;
        return true;
    default:
        return false;
    }
}

static void kbd_press(uint8_t mod, uint8_t key)
{
    uint8_t keycode[6] = { 0 };
    keycode[0] = key;
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, mod, keycode);
}

static void kbd_release(void)
{
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL);
}

static void kbd_type_char(char c)
{
    uint8_t mod, key;
    if (!ascii_to_hid(c, &mod, &key))
        return;

    kbd_press(mod, key);
    board_delay(10);
    kbd_release();
    board_delay(10);
}

static void kbd_type_string(const char *s)
{
    for (; s && *s; s++)
        kbd_type_char(*s);
}

//--------------------------------------------------------------------+
// TOTP action
//--------------------------------------------------------------------+

static void do_totp_and_type(void)
{
    if (g_cfg.totp_secret_b32[0] == '\0') {
        dbg_printf("[totp] secret not set\r\n");
        return;
    }

    uint8_t key[64];
    size_t key_len = sizeof(key);
    int rc = base32_decode_rfc4648(g_cfg.totp_secret_b32, key, &key_len);
    if (rc != 0) {
        dbg_printf("[totp] base32 decode failed rc=%d\r\n", rc);
        return;
    }

    int64_t now_unix = 0;
    if (!ds3231_get_unix_time_utc(&now_unix)) {
        dbg_printf("[totp] ds3231_get_unix_time_utc failed\r\n");
        return;
    }

    uint32_t code = 0;
    if (!totp_generate(key, key_len, now_unix, TOTP_DIGITS, &code)) {
        dbg_printf("[totp] generate failed\r\n");
        return;
    }

    char out[32];
    snprintf(out, sizeof(out), "%0*u\n", TOTP_DIGITS, (unsigned)code);

    dbg_printf("[totp] unix=%lld code=%s", (long long)now_unix, out);
    kbd_type_string(out);
}

//--------------------------------------------------------------------+
// UART TIME command task
//--------------------------------------------------------------------+

static bool parse_time_cmd(const char *line, int *Y, int *M, int *D, int *h, int *m, int *s)
{
    if (!line || !Y || !M || !D || !h || !m || !s)
        return false;

    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')
        line++;

    if (strncmp(line, "TIME ", 5) == 0) {
        int yy, mo, dd, hh, mm, ss;
        if (sscanf(line + 5, "%d-%d-%d %d:%d:%d", &yy, &mo, &dd, &hh, &mm, &ss) != 6)
            return false;
        *Y = yy;
        *M = mo;
        *D = dd;
        *h = hh;
        *m = mm;
        *s = ss;
        return true;
    }

    if (strncmp(line, "EPOCH ", 6) == 0) {
        long long ep = 0;
        if (sscanf(line + 6, "%lld", &ep) != 1)
            return false;
        int yy, mo, dd, hh, mm, ss;
        if (!ymdhms_from_unix_utc((int64_t)ep, &yy, &mo, &dd, &hh, &mm, &ss))
            return false;
        *Y = yy;
        *M = mo;
        *D = dd;
        *h = hh;
        *m = mm;
        *s = ss;
        return true;
    }

    return false;
}

static void uart_time_task(void)
{
#if DBG_UART_OK
    static char line[TIME_CMD_MAX_LINE];
    static int pos = 0;

    while (uart_is_readable(DBG_UART_ID)) {
        char c = (char)uart_getc(DBG_UART_ID);

        if (c == '\r')
            continue;

        if (c == '\n') {
            line[pos] = 0;
            pos = 0;

            int Y, M, D, h, m, s;
            if (!parse_time_cmd(line, &Y, &M, &D, &h, &m, &s)) {
                dbg_printf("ERR parse. Use: TIME YYYY-MM-DD HH:MM:SS  OR  EPOCH <seconds>\r\n");
                continue;
            }

            if (!ds3231_set_time_utc(Y, M, D, h, m, s)) {
                dbg_printf("ERR ds3231_set_time\r\n");
                continue;
            }

            if (!ds3231_clear_osf()) {
                dbg_printf("WARN ds3231_clear_osf failed\r\n");
            }

            dbg_printf("OK set DS3231 to %04d-%02d-%02d %02d:%02d:%02d (UTC)\r\n", Y, M, D, h, m,
                       s);
            continue;
        }

        if (pos < (TIME_CMD_MAX_LINE - 1)) {
            line[pos++] = c;
        } else {
            pos = 0;
            dbg_printf("ERR line too long\r\n");
        }
    }
#endif
}

//--------------------------------------------------------------------+
// Browser / WebHID helpers
//--------------------------------------------------------------------+

static bool parse_u64_be(const uint8_t *p, uint16_t len, uint64_t *out)
{
    if (!p || !out || len < 8)
        return false;

    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    *out = v;
    return true;
}

static bool validate_totp_secret_b32(const char *s)
{
    if (!s)
        return false;

    size_t slen = strnlen(s, APP_MAX_TOTP_SECRET_LEN);
    if (slen == 0)
        return true;

    uint8_t key[64];
    size_t key_len = sizeof(key);
    int rc = base32_decode_rfc4648(s, key, &key_len);
    return rc == 0 && key_len > 0;
}

static void vendor_send_packet(const uint8_t *data, uint16_t len)
{
    if (!tud_hid_ready()) {
        dbg_printf("[hid] vendor reply skipped: not ready\r\n");
        return;
    }

    if (len > VENDOR_REPORT_SIZE)
        len = VENDOR_REPORT_SIZE;

    tud_hid_report(REPORT_ID_VENDOR, data, len);
}

static void vendor_send_ok(uint8_t cmd)
{
    uint8_t pkt[VENDOR_REPORT_SIZE] = { 0 };
    pkt[0] = RSP_OK;
    pkt[1] = cmd;
    vendor_send_packet(pkt, sizeof(pkt));
}

static void vendor_send_err(uint8_t cmd, uint8_t err)
{
    uint8_t pkt[VENDOR_REPORT_SIZE] = { 0 };
    pkt[0] = RSP_ERR;
    pkt[1] = cmd;
    pkt[2] = err;
    vendor_send_packet(pkt, sizeof(pkt));
}

static void vendor_send_status(void)
{
    uint8_t pkt[VENDOR_REPORT_SIZE] = { 0 };
    pkt[0] = RSP_STATUS;

    bool osf = false;
    bool rtc_ok = ds3231_osf_is_set(&osf);
    s_rtc_status_valid = rtc_ok;
    s_rtc_osf = rtc_ok && osf;

    pkt[1] = (g_cfg.totp_secret_b32[0] != '\0') ? 1 : 0;
    pkt[2] = (g_cfg.password[0] != '\0') ? 1 : 0;
    pkt[3] = rtc_ok ? 1 : 0;
    pkt[4] = (rtc_ok && osf) ? 1 : 0;
    pkt[5] = (uint8_t)strnlen(g_cfg.totp_secret_b32, APP_MAX_TOTP_SECRET_LEN);
    pkt[6] = (uint8_t)strnlen(g_cfg.password, APP_MAX_PASSWORD_LEN);

    int64_t now_unix = 0;
    if (ds3231_get_unix_time_utc(&now_unix)) {
        uint64_t t = (uint64_t)now_unix;
        for (int i = 0; i < 8; i++) {
            pkt[7 + i] = (uint8_t)(t >> (56 - i * 8));
        }
    }

    vendor_send_packet(pkt, sizeof(pkt));
}

//--------------------------------------------------------------------+
// MAIN
//--------------------------------------------------------------------+

int main(void)
{
    board_init();

    dbg_uart_init();
    if (!app_led_init())
        dbg_printf("[led] ERROR: no free PIO state machine/program space\r\n");
    rtc_i2c_init();
    app_cfg_load();

    dbg_printf("[boot] start; button=BOOTSEL(QSPI CS)\r\n");
    dbg_printf("READY: send 'TIME YYYY-MM-DD HH:MM:SS' or 'EPOCH <seconds>' (UTC)\r\n");
    dbg_printf("[cfg] secret=%s password=%s\r\n", g_cfg.totp_secret_b32[0] ? "set" : "empty",
               g_cfg.password[0] ? "set" : "empty");

    tud_init(BOARD_TUD_RHPORT);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    dbg_printf("[boot] tusb init done\r\n");
    ds3231_print_time();
    ds3231_print_power_status();

    while (1) {
        tud_task();
        uart_time_task();
        rtc_status_poll_task(false);
        hid_task();
        status_led_task();
    }
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

void tud_mount_cb(void)
{
    dbg_printf("[usb] mounted\r\n");
}

void tud_umount_cb(void)
{
    dbg_printf("[usb] unmounted\r\n");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    dbg_printf("[usb] suspended (rw=%d)\r\n", remote_wakeup_en ? 1 : 0);
}

void tud_resume_cb(void)
{
    dbg_printf("[usb] resume; mounted=%d\r\n", tud_mounted() ? 1 : 0);
}

//--------------------------------------------------------------------+
// HID task
//--------------------------------------------------------------------+

static void hid_task(void)
{
    static bool last_raw = false;
    static bool stable = false;
    static uint32_t last_change_ms = 0;

    bool raw = button_pressed_raw();
    uint32_t now = board_millis();

    if (s_click_pending) {
        if ((int32_t)(now - s_click_deadline_ms) >= 0) {
            s_click_pending = false;
            dbg_printf("[btn] single-click -> OTP\r\n");
            if (tud_suspended()) {
                dbg_printf("[hid] suspended -> remote wakeup\r\n");
                tud_remote_wakeup();
            } else if (tud_mounted() && tud_hid_ready()) {
                do_totp_and_type();
                dbg_printf("[hid] otp done\r\n");
            } else {
                dbg_printf("[hid] not ready -> skip otp\r\n");
            }
        }
    }

    if (raw != last_raw) {
        last_raw = raw;
        last_change_ms = now;
    }

    if ((now - last_change_ms) < DEBOUNCE_MS)
        return;

    if (stable != raw) {
        stable = raw;
        s_button_pressed = stable;

        dbg_printf("[btn] %s ; mounted=%d hid_ready=%d suspended=%d\r\n",
                   stable ? "PRESS" : "RELEASE", tud_mounted() ? 1 : 0, tud_hid_ready() ? 1 : 0,
                   tud_suspended() ? 1 : 0);

        if (stable) {
            ds3231_print_time();
            return;
        }

        if (!(tud_mounted() && tud_hid_ready()) && !tud_suspended()) {
            dbg_printf("[hid] not ready -> ignore click\r\n");
            s_click_pending = false;
            return;
        }

        if (s_click_pending) {
            s_click_pending = false;
            dbg_printf("[btn] double-click -> PASSWORD\r\n");

            if (tud_suspended()) {
                dbg_printf("[hid] suspended -> remote wakeup\r\n");
                tud_remote_wakeup();
                return;
            }

            if (g_cfg.password[0] == '\0') {
                dbg_printf("[hid] password not set\r\n");
            } else {
                dbg_printf("[hid] typing password\r\n");
                kbd_type_string(g_cfg.password);
                kbd_type_char('\n');
                dbg_printf("[hid] password done\r\n");
            }
        } else {
            s_click_pending = true;
            s_click_deadline_ms = now + DBLCLICK_MS;
            dbg_printf("[btn] click pending... wait %dms\r\n", DBLCLICK_MS);
        }
    }
}

//--------------------------------------------------------------------+
// HID callbacks
//--------------------------------------------------------------------+

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)instance;
    (void)report;
    (void)len;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_type;

    if (report_id != REPORT_ID_VENDOR) {
        return;
    }

    if (!buffer || bufsize < 1) {
        vendor_send_err(0, ERR_BAD_LENGTH);
        return;
    }

    uint8_t cmd = buffer[0];
    const uint8_t *payload = buffer + 1;
    uint16_t payload_len = (bufsize > 0) ? (uint16_t)(bufsize - 1) : 0;

    switch (cmd) {
    case CMD_SET_TIME_UNIX: {
        if (payload_len < 8) {
            vendor_send_err(cmd, ERR_BAD_LENGTH);
            return;
        }

        uint64_t raw = 0;
        if (!parse_u64_be(payload, 8, &raw)) {
            vendor_send_err(cmd, ERR_BAD_TIME);
            return;
        }

        int Y, M, D, h, m, s;
        if (!ymdhms_from_unix_utc((int64_t)raw, &Y, &M, &D, &h, &m, &s)) {
            vendor_send_err(cmd, ERR_BAD_TIME);
            return;
        }

        if (!ds3231_set_time_utc(Y, M, D, h, m, s)) {
            vendor_send_err(cmd, ERR_BAD_TIME);
            return;
        }

        if (!ds3231_clear_osf()) {
            dbg_printf("WARN ds3231_clear_osf failed\r\n");
        }

        dbg_printf("[webhid] set time -> %04d-%02d-%02d %02d:%02d:%02d UTC\r\n", Y, M, D, h, m, s);
        vendor_send_ok(cmd);
        return;
    }

    case CMD_SET_PASSWORD: {
        if (payload_len >= APP_MAX_PASSWORD_LEN) {
            vendor_send_err(cmd, ERR_BAD_LENGTH);
            return;
        }

        memset(g_cfg.password, 0, sizeof(g_cfg.password));
        memcpy(g_cfg.password, payload, payload_len);
        g_cfg.password[payload_len] = '\0';

        if (!app_cfg_save()) {
            vendor_send_err(cmd, ERR_FLASH_WRITE);
            return;
        }

        dbg_printf("[webhid] password updated len=%u\r\n", (unsigned)payload_len);
        vendor_send_ok(cmd);
        return;
    }

    case CMD_SET_TOTP_SECRET: {
        if (payload_len >= APP_MAX_TOTP_SECRET_LEN) {
            vendor_send_err(cmd, ERR_BAD_LENGTH);
            return;
        }

        char tmp[APP_MAX_TOTP_SECRET_LEN];
        memset(tmp, 0, sizeof(tmp));
        memcpy(tmp, payload, payload_len);
        tmp[payload_len] = '\0';

        if (!validate_totp_secret_b32(tmp)) {
            vendor_send_err(cmd, ERR_BAD_SECRET);
            return;
        }

        memset(g_cfg.totp_secret_b32, 0, sizeof(g_cfg.totp_secret_b32));
        memcpy(g_cfg.totp_secret_b32, tmp, payload_len);
        g_cfg.totp_secret_b32[payload_len] = '\0';

        if (!app_cfg_save()) {
            vendor_send_err(cmd, ERR_FLASH_WRITE);
            return;
        }

        dbg_printf("[webhid] totp secret updated len=%u\r\n", (unsigned)payload_len);
        vendor_send_ok(cmd);
        return;
    }

    case CMD_GET_STATUS:
        vendor_send_status();
        return;

    case CMD_CLEAR_PASSWORD:
        memset(g_cfg.password, 0, sizeof(g_cfg.password));
        if (!app_cfg_save()) {
            vendor_send_err(cmd, ERR_FLASH_WRITE);
            return;
        }
        dbg_printf("[webhid] password cleared\r\n");
        vendor_send_ok(cmd);
        return;

    case CMD_CLEAR_SECRET:
        memset(g_cfg.totp_secret_b32, 0, sizeof(g_cfg.totp_secret_b32));
        if (!app_cfg_save()) {
            vendor_send_err(cmd, ERR_FLASH_WRITE);
            return;
        }
        dbg_printf("[webhid] totp secret cleared\r\n");
        vendor_send_ok(cmd);
        return;

    default:
        vendor_send_err(cmd, ERR_BAD_CMD);
        return;
    }
}
