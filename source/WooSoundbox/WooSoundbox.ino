/* ==========================================================
 * WooCommerce Soundbox  --  ESP32 Dev Module
 *
 * Speaks every new order from a WooCommerce store, like a UPI
 * soundbox: "New order received of rupees 1499", and
 * shows "#4521 Rahul / Rs.1499" on a 0.91" OLED.
 *
 * Nothing is added to the website. The ESP32 polls the built-in
 * WooCommerce REST API with a READ-ONLY API key every 10 s and
 * announces "processing" orders it has not seen yet (COD orders
 * and confirmed online payments -- never abandoned carts).
 *
 * Voice: Google Translate TTS (free, no key) streamed through a
 * MAX98357A I2S amp. If the voice fails, it beeps instead.
 *
 * BOARD SETTINGS (required):
 *   Board            : ESP32 Dev Module
 *   Partition Scheme : Huge APP (3MB No OTA/1MB SPIFFS)
 *
 * WIRING
 *   OLED SSD1306 128x32 (I2C)      MAX98357A (I2S)
 *     VCC -> 3V3                     VIN  -> 5V
 *     GND -> GND                     GND  -> GND
 *     SDA -> GPIO 21                 DIN  -> GPIO 27
 *     SCL -> GPIO 22                 BCLK -> GPIO 26
 *                                    LRC  -> GPIO 25
 *   LED: on-board LED (GPIO 2), lit while speaking.
 *   Tip: a 470-1000 uF capacitor across the amp's VIN/GND stops
 *   brownout resets on loud sounds.
 *
 * FIRST-TIME SETUP
 *   A board with no saved settings starts its own Wi-Fi network
 *   "Woo-Soundbox" (password: soundbox). Join it from a phone; the
 *   setup page opens by itself (or browse to 192.168.4.1). Enter Wi-Fi,
 *   store URL and the read-only API key + secret, then Save.
 *   The form is pre-filled with the values in secrets.h.
 *
 *   Hold the board's BOOT button for 10 s while it runs to erase the
 *   settings and open setup again. Setup also opens by itself if the
 *   Wi-Fi or the store login fails.
 *
 * LIBRARIES: ArduinoJson 7, ESP32-audioI2S 3.0.13,
 *            Adafruit SSD1306 + GFX.
 * ========================================================== */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Audio.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

#include "secrets.h"

// ==========================================================
// Config
// ==========================================================
// How often to ask the store for new orders. 10 s gives ~5-13 s
// from order to voice. On shared hosting use 15-30 s: faster
// polling made the test server slow down for everyone.
#define POLL_INTERVAL   10000UL

#define FETCH_ORDERS    20      // orders per check
#define BASELINE_ORDERS 100     // first check: remember, don't announce
#define SEEN_MAX        100     // order IDs remembered
#define HTTP_TIMEOUT    15000   // ms per request

// Uncomment to announce in Hindi instead of English.
//#define ANNOUNCE_HINDI

#ifdef ANNOUNCE_HINDI
#define TTS_LANG        "hi"
#else
#define TTS_LANG        "en"
#endif

#define AUDIO_VOLUME    21      // 0..21 (library maximum)
#define ORDER_HOLD      15000UL // order screen stays this long

// Setup portal
#define AP_SSID         "Woo-Soundbox"
#define AP_PASSWORD     "soundbox"      // 8+ chars
#define WIFI_TIMEOUT    20000UL         // give up joining Wi-Fi after this
#define PORTAL_TIMEOUT  300000UL        // retry after 5 min if nobody configures

// BOOT button (GPIO 0): hold 10 s while running = erase settings.
// The countdown shows on the OLED as soon as it is pressed.
#define RESET_PIN       0
#define RESET_HOLD_MS   10000UL

// Pins
#define PIN_SDA         21
#define PIN_SCL         22
#define PIN_I2S_BCLK    26
#define PIN_I2S_LRC     25
#define PIN_I2S_DOUT    27
#define LED_PIN         2

// OLED
#define OLED_W          128
#define OLED_H          32
#define OLED_ADDR       0x3C

// ==========================================================
// Objects and state
// ==========================================================
Audio audio;
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

struct Order
{
    uint32_t id;
    float    total;
    char     name[24];
};

// Ring buffer of announced (or baseline) order IDs.
uint32_t seenIds[SEEN_MAX];
int      seenCount = 0;
int      seenNext  = 0;

bool     baselineDone  = false;
uint32_t orderShownAt  = 0;
uint32_t lastOrderId   = 0;
float    lastOrderAmt  = -1;
bool     audioAttached = false;
int      lastHttpCode  = 0;

// Settings saved in flash by the setup portal.
String cfgSsid, cfgWifiPass, cfgStore, cfgKey, cfgSecret;

// Defined in the SETUP PORTAL section further down.
void runPortal(const String &why, bool canTimeout);


// ==========================================================
// OLED
// ==========================================================
void oledMessage(const char *l1, const char *l2)
{
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.print(l1);

    if (l2)
    {
        oled.setCursor(0, 16);
        oled.print(l2);
    }

    oled.display();
}


String formatRupees(float amount)
{
    long rupees = (long)amount;
    int  paise  = (int)roundf((amount - rupees) * 100.0f);

    if (paise == 100)
    {
        rupees++;
        paise = 0;
    }

    char buf[20];

    if (paise)
        snprintf(buf, sizeof(buf), "Rs.%ld.%02d", rupees, paise);
    else
        snprintf(buf, sizeof(buf), "Rs.%ld", rupees);

    return String(buf);
}


void oledHome()
{
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);

    oled.setCursor(0, 0);
    oled.print("Woo Soundbox");
    oled.setCursor(0, 12);
    oled.print("Waiting for orders");
    oled.setCursor(0, 24);

    if (lastOrderId)
    {
        oled.print("Last #");
        oled.print(lastOrderId);
        oled.print(" ");
        oled.print(formatRupees(lastOrderAmt));
    }
    else
        oled.print(WiFi.localIP());

    oled.display();
    orderShownAt = 0;
}


// "#4521 Rahul" small on top, "Rs.1499" big below.
void oledOrder(const Order &o)
{
    char top[24];
    snprintf(top, sizeof(top), "#%lu %s", (unsigned long)o.id, o.name);

    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.print(top);

    oled.setTextSize(2);
    oled.setCursor(0, 14);
    oled.print(formatRupees(o.total));
    oled.display();

    orderShownAt = millis();
}


// ==========================================================
// Sound
// ==========================================================
// Beep generated on the ESP32's second I2S port. It borrows the amp
// pins, then gives them back to the speech player (setPinout
// re-attaches them each time it is called).
#define BEEP_RATE       44100
#define BEEP_AMPLITUDE  14000   // of 32767

void beepTone(I2SClass &i2s, float freq, uint32_t ms)
{
    const uint32_t total = (BEEP_RATE * ms) / 1000;
    const float    step  = 2.0f * PI * freq / BEEP_RATE;
    const uint32_t ramp  = BEEP_RATE / 200;     // 5 ms fade: no click
    float    phase = 0.0f;
    int16_t  buf[256];
    uint32_t sent  = 0;

    while (sent < total)
    {
        int frames = 0;

        for (int i = 0; i < 256 && sent < total; i += 2, sent++, frames++)
        {
            float env = 1.0f;

            if (sent < ramp)
                env = (float)sent / ramp;
            else if (total - sent < ramp)
                env = (float)(total - sent) / ramp;

            int16_t v  = (int16_t)(sinf(phase) * BEEP_AMPLITUDE * env);
            buf[i]     = v;
            buf[i + 1] = v;

            phase += step;
            if (phase > 2.0f * PI)
                phase -= 2.0f * PI;
        }

        i2s.write((uint8_t *)buf, frames * 2 * sizeof(int16_t));
    }
}


void playBeep()
{
    I2SClass i2s;
    i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);

    if (i2s.begin(I2S_MODE_STD, BEEP_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO))
    {
        beepTone(i2s, 1500, 120);
        beepTone(i2s, 0,     60);
        beepTone(i2s, 2000, 180);
        beepTone(i2s, 0,    100);   // let the DMA drain
        i2s.end();
    }
    else
        Serial.println("Beep: I2S init failed.");

    if (audioAttached)
        audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
}


// Google Translate TTS. Returns false if no audio played, so the
// caller can beep instead.
bool speak(const String &text)
{
    Serial.print("TTS: ");
    Serial.println(text);

    if (!audio.connecttospeech(text.c_str(), TTS_LANG))
    {
        Serial.println("TTS: request failed");
        return false;
    }

    uint32_t start = millis();

    while (!audio.isRunning() && millis() - start < 5000)
    {
        audio.loop();
        delay(1);
    }

    if (!audio.isRunning())
    {
        Serial.println("TTS: stream never started");
        return false;
    }

    // 15 s cap so a stalled stream cannot mute the box for long.
    while (audio.isRunning() && millis() - start < 15000)
        audio.loop();

    Serial.println("TTS: done");
    return true;
}


void audio_info(const char *info)
{
    Serial.print("audio: ");
    Serial.println(info);
}


String buildSpeech(float amount)
{
    long rupees = (long)amount;
    int  paise  = (int)roundf((amount - rupees) * 100.0f);

    if (paise == 100)
    {
        rupees++;
        paise = 0;
    }

#ifdef ANNOUNCE_HINDI
    String s = "नया ऑर्डर मिला, " + String(rupees) + " रुपये";
    if (paise)
        s += " " + String(paise) + " पैसे";
    return s + ".";
#else
    String s = "New order received of rupees " + String(rupees);
    if (paise)
        s += " and " + String(paise) + " paise";
    return s + ".";
#endif
}


void announceOrder(const Order &o)
{
    Serial.println();
    Serial.println("*******************************************");
    Serial.printf ("  NEW ORDER #%lu  %s  Rs.%.2f\n", (unsigned long)o.id, o.name, o.total);
    Serial.println("*******************************************");

    oledOrder(o);
    digitalWrite(LED_PIN, HIGH);

    if (!speak(buildSpeech(o.total)))
    {
        Serial.println("Voice failed: beeping instead.");
        playBeep();
        playBeep();
    }

    digitalWrite(LED_PIN, LOW);

    lastOrderId  = o.id;
    lastOrderAmt = o.total;
}


// ==========================================================
// Seen-order memory
// ==========================================================
bool isSeen(uint32_t id)
{
    for (int i = 0; i < seenCount; i++)
        if (seenIds[i] == id)
            return true;

    return false;
}


void markSeen(uint32_t id)
{
    if (isSeen(id))
        return;

    seenIds[seenNext] = id;
    seenNext = (seenNext + 1) % SEEN_MAX;

    if (seenCount < SEEN_MAX)
        seenCount++;
}


// ==========================================================
// WooCommerce REST API
// ==========================================================
// Copies a name for the OLED: ASCII only (the font has no other
// glyphs), "Customer" when empty.
void copyName(char *dst, size_t len, const char *src)
{
    size_t n = 0;

    for (const char *p = src ? src : ""; *p && n < len - 1; p++)
        if (*p >= 32 && *p < 127)
            dst[n++] = *p;

    dst[n] = '\0';

    if (n == 0)
        strlcpy(dst, "Customer", len);
}


// One request. Returns the number of orders read into `out`, or -1.
// A fresh HTTPS connection every time: the server silently dropped
// kept-alive connections (HTTP -5) after a few seconds.
int fetchOnce(int perPage, Order *out, int maxOut)
{
    WiFiClientSecure client;
    client.setInsecure();       // skips certificate check: fine for learning

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);
    http.setConnectTimeout(HTTP_TIMEOUT);
    http.setReuse(false);

    String url = cfgStore +
                 "wp-json/wc/v3/orders?status=processing&orderby=date&order=desc" +
                 "&per_page=" + String(perPage) +
                 "&_fields=id,total,billing.first_name";

    if (!http.begin(client, url))
    {
        Serial.println("HTTP begin failed");
        return -1;
    }

    http.setAuthorization(cfgKey.c_str(), cfgSecret.c_str());

    uint32_t t0   = millis();
    int      code = http.GET();
    lastHttpCode  = code;

    if (code != 200)
    {
        Serial.printf("HTTP %d (%s) after %lu ms\n", code,
                      http.errorToString(code).c_str(), millis() - t0);
        http.end();
        return -1;
    }

    String body = http.getString();     // also handles chunked replies
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err)
    {
        Serial.printf("JSON error: %s\n", err.c_str());
        return -1;
    }

    JsonArray arr = doc.as<JsonArray>();
    int n = 0;

    for (JsonObject o : arr)
    {
        if (n >= maxOut)
            break;

        out[n].id    = o["id"] | 0;
        out[n].total = atof(o["total"] | "0");
        copyName(out[n].name, sizeof(out[n].name), o["billing"]["first_name"] | "");

        if (out[n].id)
            n++;
    }

    Serial.printf("Checked store: %d processing orders (%lu ms, %u bytes)\n",
                  n, millis() - t0, body.length());
    return n;
}


// Retry once on failure (timeouts and dropped connections happen).
int fetchOrders(int perPage, Order *out, int maxOut)
{
    int n = fetchOnce(perPage, out, maxOut);

    if (n < 0)
    {
        Serial.println("Retrying once...");
        delay(1000);
        n = fetchOnce(perPage, out, maxOut);
    }

    return n;
}


Order orders[BASELINE_ORDERS];

void checkStore()
{
    if (!baselineDone)
    {
        // First check: remember what already exists, announce nothing.
        int n = fetchOrders(BASELINE_ORDERS, orders, BASELINE_ORDERS);

        if (n < 0)
        {
            // The store answered but refused: settings are wrong, not
            // the network. Open setup so they can be fixed.
            if (lastHttpCode == 401 || lastHttpCode == 403)
                runPortal("The store rejected the API key (HTTP " + String(lastHttpCode) +
                          "). Check the consumer key and secret, and that the key has Read permission.", true);

            if (lastHttpCode == 404)
                runPortal("Store not found (HTTP 404). Check the store URL, e.g. https://example.com/", true);

            return;                         // network trouble: try again next interval
        }

        for (int i = 0; i < n; i++)
            markSeen(orders[i].id);

        baselineDone = true;
        Serial.printf("Baseline: %d existing orders remembered, none announced.\n", n);
        return;
    }

    int n = fetchOrders(FETCH_ORDERS, orders, FETCH_ORDERS);

    if (n < 0)
        return;

    // The reply is newest first; announce the new ones oldest first.
    for (int i = n - 1; i >= 0; i--)
    {
        if (isSeen(orders[i].id))
            continue;

        markSeen(orders[i].id);
        announceOrder(orders[i]);
    }
}


// ==========================================================
// Wi-Fi
// ==========================================================
bool connectWiFi()
{
    Serial.printf("Connecting to Wi-Fi %s", cfgSsid.c_str());
    oledMessage("Connecting WiFi", cfgSsid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(cfgSsid.c_str(), cfgWifiPass.c_str());

    uint32_t t0 = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - t0 > WIFI_TIMEOUT)
        {
            Serial.println("\nWi-Fi connect timed out.");
            return false;
        }

        delay(500);
        Serial.print(".");
    }

    Serial.printf("\nWi-Fi connected, IP %s\n", WiFi.localIP().toString().c_str());
    return true;
}


// ==========================================================
// SETUP PORTAL
//
// Settings live in flash (Preferences / NVS), so they survive power
// cycles and re-flashing. A board with none saved opens this portal.
// ==========================================================
Preferences prefs;
WebServer   server(80);
DNSServer   dns;

String portalNote      = "";
bool   portalNoteError = false;
String scanOptions     = "";


void loadConfig()
{
    prefs.begin("woobox", true);
    cfgSsid     = prefs.getString("ssid",   "");
    cfgWifiPass = prefs.getString("wpass",  "");
    cfgStore    = prefs.getString("store",  "");
    cfgKey      = prefs.getString("key",    "");
    cfgSecret   = prefs.getString("secret", "");
    prefs.end();
}


void saveConfig()
{
    prefs.begin("woobox", false);
    prefs.putString("ssid",   cfgSsid);
    prefs.putString("wpass",  cfgWifiPass);
    prefs.putString("store",  cfgStore);
    prefs.putString("key",    cfgKey);
    prefs.putString("secret", cfgSecret);
    prefs.end();
}


bool configComplete()
{
    return cfgSsid.length() && cfgStore.length() && cfgKey.length() && cfgSecret.length();
}


String htmlEscape(String v)
{
    v.replace("&", "&amp;");
    v.replace("<", "&lt;");
    v.replace(">", "&gt;");
    v.replace("\"", "&quot;");
    return v;
}


const char PORTAL_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Woo Soundbox Setup</title>
<style>
body{font-family:system-ui,sans-serif;background:#f4f4f5;margin:0;padding:16px;color:#18181b}
.card{max-width:440px;margin:0 auto;background:#fff;border-radius:12px;padding:20px;box-shadow:0 1px 4px rgba(0,0,0,.08)}
h1{font-size:20px;margin:0 0 4px}
h2{font-size:14px;text-transform:uppercase;letter-spacing:.05em;color:#71717a;margin:20px 0 8px}
label{display:block;font-size:14px;margin:10px 0 4px}
input{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border:1px solid #d4d4d8;border-radius:8px}
button{width:100%;margin-top:20px;padding:12px;font-size:16px;border:0;border-radius:8px;background:#7f54b3;color:#fff}
.note{padding:10px;border-radius:8px;font-size:14px;background:#eff6ff;color:#1e40af}
.err{background:#fef2f2;color:#b91c1c}
.hint{font-size:12px;color:#71717a;margin:4px 0 0}
</style></head><body><div class="card">
<h1>Woo Soundbox</h1>
<p class="note %NOTECLASS%">%NOTE%</p>
<form method="post" action="/save">
<h2>Wi-Fi</h2>
<label for="ssid">Network name</label>
<input id="ssid" name="ssid" list="nets" value="%SSID%" required autocomplete="off">
<datalist id="nets">%OPTIONS%</datalist>
<label for="wpass">Wi-Fi password</label>
<input id="wpass" name="wpass" type="password" value="%WPASS%">
<p class="hint">2.4 GHz networks only.</p>
<h2>WooCommerce store</h2>
<label for="store">Store URL</label>
<input id="store" name="store" type="url" value="%STORE%" placeholder="https://example.com/" required>
<label for="key">Consumer key</label>
<input id="key" name="key" value="%KEY%" placeholder="ck_..." required autocomplete="off">
<label for="secret">Consumer secret</label>
<input id="secret" name="secret" type="password" value="%SECRET%" placeholder="cs_..." required autocomplete="off">
<p class="hint">WooCommerce &rarr; Settings &rarr; Advanced &rarr; REST API &rarr; Add key, Permissions: <b>Read</b>.</p>
<button type="submit">Save and restart</button>
</form></div></body></html>)rawliteral";


String portalPage(const String &ssid, const String &wpass, const String &store,
                  const String &key, const String &secret)
{
    String page = FPSTR(PORTAL_PAGE);

    page.replace("%NOTE%",      htmlEscape(portalNote));
    page.replace("%NOTECLASS%", portalNoteError ? "err" : "");
    page.replace("%OPTIONS%",   scanOptions);
    page.replace("%SSID%",      htmlEscape(ssid));
    page.replace("%WPASS%",     htmlEscape(wpass));
    page.replace("%STORE%",     htmlEscape(store));
    page.replace("%KEY%",       htmlEscape(key));
    page.replace("%SECRET%",    htmlEscape(secret));

    return page;
}


// The form shows what is saved; on a fresh board, the secrets.h values.
void handleRoot()
{
    server.send(200, "text/html",
                portalPage(cfgSsid.length()     ? cfgSsid     : String(WIFI_SSID),
                           cfgWifiPass.length() ? cfgWifiPass : String(WIFI_PASSWORD),
                           cfgStore.length()    ? cfgStore    : String(STORE_URL),
                           cfgKey.length()      ? cfgKey      : String(WC_KEY),
                           cfgSecret.length()   ? cfgSecret   : String(WC_SECRET)));
}


void handleSave()
{
    String ssid   = server.arg("ssid");
    String wpass  = server.arg("wpass");
    String store  = server.arg("store");
    String key    = server.arg("key");
    String secret = server.arg("secret");

    ssid.trim();
    store.trim();
    key.trim();
    secret.trim();

    if (store.length() && !store.endsWith("/"))
        store += "/";

    String problem = "";

    if (ssid.length() == 0)
        problem = "Enter the Wi-Fi network name.";
    else if (!store.startsWith("https://") && !store.startsWith("http://"))
        problem = "The store URL must start with https://";
    else if (!key.startsWith("ck_"))
        problem = "The consumer key should start with ck_";
    else if (!secret.startsWith("cs_"))
        problem = "The consumer secret should start with cs_";

    if (problem.length())
    {
        portalNote      = problem;
        portalNoteError = true;
        server.send(200, "text/html", portalPage(ssid, wpass, store, key, secret));
        return;
    }

    cfgSsid     = ssid;
    cfgWifiPass = wpass;
    cfgStore    = store;
    cfgKey      = key;
    cfgSecret   = secret;
    saveConfig();

    Serial.println("Settings saved. Restarting...");

    server.send(200, "text/html",
                "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<style>body{font-family:system-ui,sans-serif;padding:24px;text-align:center}</style></head>"
                "<body><h2>Saved</h2><p>The soundbox is restarting and will join " + htmlEscape(ssid) +
                ". You can reconnect your phone to your normal Wi-Fi.</p></body></html>");

    delay(2000);
    ESP.restart();
}


// Phones probe URLs like /generate_204 to detect a captive portal.
// Redirecting everything unknown to the form makes the page pop up.
void handleNotFound()
{
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
}


void oledLines(const char *l1, const char *l2, const char *l3, const char *l4)
{
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);  oled.print(l1);
    oled.setCursor(0, 8);  oled.print(l2);
    oled.setCursor(0, 16); oled.print(l3);
    oled.setCursor(0, 24); oled.print(l4);
    oled.display();
}


// Start the box's own Wi-Fi network and serve the setup form. Never
// returns: saving restarts the ESP32. With canTimeout (settings exist,
// a connection just failed) it restarts after PORTAL_TIMEOUT so a short
// outage does not strand it in setup mode.
void runPortal(const String &why, bool canTimeout)
{
    Serial.println();
    Serial.println("=== SETUP PORTAL ===");
    Serial.println(why);

    portalNote      = why;
    portalNoteError = canTimeout;

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    delay(100);

    // Scan once so the form can suggest nearby networks.
    scanOptions = "";
    int n = WiFi.scanNetworks();

    for (int i = 0; i < n; i++)
    {
        String opt = "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";

        if (WiFi.SSID(i).length() && scanOptions.indexOf(opt) < 0)
            scanOptions += opt;
    }

    WiFi.softAP(AP_SSID, AP_PASSWORD);
    IPAddress ip = WiFi.softAPIP();

    dns.start(53, "*", ip);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);
    server.begin();

    Serial.printf("Join Wi-Fi '%s' (password '%s') and open http://%s\n",
                  AP_SSID, AP_PASSWORD, ip.toString().c_str());

    oledLines("SETUP MODE", "WiFi:" AP_SSID, "Pass:" AP_PASSWORD, "Open 192.168.4.1");

    uint32_t t0 = millis();

    while (true)
    {
        dns.processNextRequest();
        server.handleClient();

        if (canTimeout && millis() - t0 > PORTAL_TIMEOUT && WiFi.softAPgetStationNum() == 0)
        {
            Serial.println("Nobody configured the box; restarting to retry.");
            ESP.restart();
        }

        delay(2);
    }
}


// ==========================================================
// BOOT button: hold 10 s to erase settings and reopen setup.
// ==========================================================
void factoryReset()
{
    Serial.println("BOOT held 10 s: erasing all settings.");
    oledMessage("Erasing settings", "restarting...");

    if (audioAttached)
        playBeep();

    prefs.begin("woobox", false);
    prefs.clear();
    prefs.end();

    WiFi.disconnect(true, true);    // also forget the Wi-Fi the core stored

    delay(1000);
    ESP.restart();
}


// The press time is recorded by an interrupt, so a press made while a
// store check or the speaker was busy still counts from the real moment
// BOOT went down.
volatile uint32_t bootPressedAt = 0;

void IRAM_ATTR onBootChange()
{
    bootPressedAt = (digitalRead(RESET_PIN) == LOW) ? (millis() | 1) : 0;
}


// Returns true while BOOT is held, so loop() can pause meanwhile.
bool checkFactoryReset()
{
    static int lastShown = -1;

    uint32_t since = bootPressedAt;

    // Released (or never pressed). The pin check also covers a missed edge.
    if (since == 0 || digitalRead(RESET_PIN) == HIGH)
    {
        if (lastShown >= 0)
        {
            Serial.println("BOOT released: reset cancelled.");
            oledHome();
        }

        lastShown = -1;
        return false;
    }

    uint32_t held = millis() - since;

    if (held >= RESET_HOLD_MS)
        factoryReset();                     // does not return

    int left = (int)((RESET_HOLD_MS - held + 999) / 1000);

    if (left != lastShown)
    {
        if (lastShown < 0)
            Serial.println("BOOT pressed: hold 10 s to erase settings.");

        lastShown = left;

        char line[24];
        snprintf(line, sizeof(line), "Hold to reset %d s", left);
        oledMessage("ERASE SETTINGS?", line);
    }

    return true;
}


// ==========================================================
void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== WooCommerce Soundbox ===");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);

    // I2C scan: shows whether the OLED answers at all (and where).
    int found = 0;
    Serial.print("I2C scan (SDA 21, SCL 22):");

    for (uint8_t a = 1; a < 127; a++)
    {
        Wire.beginTransmission(a);

        if (Wire.endTransmission() == 0)
        {
            Serial.printf(" 0x%02X", a);
            found++;
        }
    }

    Serial.println(found ? "" : " nothing found -- check OLED wiring and power");

    // begin() does not check the display is really there, so ask it directly.
    Wire.beginTransmission(OLED_ADDR);
    bool oledAnswers = (Wire.endTransmission() == 0);

    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR) || !oledAnswers)
        Serial.println("OLED not answering at 0x3C -- check wiring/address.");
    else
        Serial.println("OLED OK at 0x3C.");

    oledMessage("Woo Soundbox", "starting...");

    pinMode(RESET_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(RESET_PIN), onBootChange, CHANGE);

    loadConfig();

    if (!configComplete())
        runPortal("No settings saved yet. Check the details below and tap Save.", false);

    if (!connectWiFi())
        runPortal("Could not join Wi-Fi '" + cfgSsid + "'. Check the network name and password.", true);

    // Ready beep, then hand the amp pins to the speech player.
    playBeep();
    audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
    audioAttached = true;
    audio.setVolume(AUDIO_VOLUME);

    oledHome();
}


void loop()
{
    static uint32_t lastCheck = 0;
    static bool     wifiLost  = false;

    // BOOT held: count down to a settings reset, nothing else meanwhile.
    if (checkFactoryReset())
    {
        delay(20);
        return;
    }

    if (orderShownAt && millis() - orderShownAt > ORDER_HOLD)
        oledHome();

    if (WiFi.status() != WL_CONNECTED)
    {
        static uint32_t lastTry = 0;

        if (!wifiLost || millis() - lastTry > 10000)
        {
            wifiLost = true;
            lastTry  = millis();
            Serial.println("Wi-Fi lost; reconnecting...");
            oledMessage("WiFi lost", "reconnecting...");
            WiFi.reconnect();
        }

        delay(50);
        return;
    }

    if (wifiLost)
    {
        // Back online: restore the screen and check straight away.
        wifiLost = false;
        Serial.println("Wi-Fi back.");
        oledHome();
        lastCheck = 0;
    }

    if (lastCheck == 0 || millis() - lastCheck >= POLL_INTERVAL)
    {
        lastCheck = millis();
        checkStore();
    }

    audio.loop();
    delay(5);
}
