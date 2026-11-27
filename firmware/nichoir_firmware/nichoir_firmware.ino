/*
 * nichoir_firmware.ino — Nichoir connecté, M5Stack TimerCam (ESP32 + OV3660)
 *
 * Matériel (schéma de câblage V2) :
 *   - PIR -> CLK1 du CD4013 ; Q1 -> Q1 (2N7000) = faux appui sur le bouton PWR
 *   - Q2 (bascule 2) -> RESET1 : quand Q2 = 1, le PIR est bloqué
 *   - G13 -> SET2   : impulsion = PIR bloqué + faux appui relâché
 *   - G4  -> RESET2 : impulsion = PIR réarmé ; G4 commande aussi la LED IR (IRLZ44N)
 *   - G33 = maintien de l'alimentation, G38 = mesure batterie (VBAT / 1,51)
 *   - RTC BM8563 (I2C G12/G14) : compte à rebours = fin du délai de 5 min,
 *                                alarme quotidienne = heartbeat
 *
 * Chaque réveil exécute UNE séquence dans setup(), puis coupe l'alimentation.
 * loop() n'est jamais atteinte.
 *
 *   Cause du réveil (registre 0x01 du BM8563) :
 *     aucun drapeau -> A : PIR (ou bouton PWR)  -> photo + délai de 5 min
 *     TF = 1        -> B : fin du délai         -> réarmement du PIR
 *     AF = 1        -> C : heartbeat quotidien  -> niveau batterie
 *
 * Bibliothèques : PubSubClient (Nick O'Leary), WiFiManager (tzapu).
 * La caméra et la RTC sont pilotées directement (esp_camera + Wire),
 * sans la bibliothèque M5, pour contrôler les registres de la RTC.
 */
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include "esp_camera.h"
#include "time.h"

// ------------------------------------------------------------- broches
#define PIN_HOLD      33   // maintien de l'alimentation (FET4)
#define PIN_BAT_ADC   38   // pont diviseur VBAT / 1,51
#define PIN_BLOCK     13   // G13 -> SET2 du CD4013
#define PIN_REARM_IR   4   // G4  -> RESET2 du CD4013 + grille de l'IRLZ44N (LED IR)
#define RTC_SDA       12
#define RTC_SCL       14
#define RTC_ADDR    0x51

// ------------------------------------------------------------- réglages
const uint16_t MQTT_PORT       = 1883;
const uint8_t  COOLDOWN_MIN    = 6;      // timer 1/60 Hz : la 1re période peut être plus courte -> 5 à 6 min
const uint8_t  HB_HOUR         = 12;     // heure du heartbeat quotidien (heure locale)
const uint8_t  HB_MIN          = 0;
const float    BAT_DIV         = 1.51f;  // à calibrer avec le multimètre
const float    BAT_LOW_V       = 3.30f;  // sous ce seuil : pas de photo (CD4013 garanti >= 3 V)
const uint32_t WIFI_TIMEOUT_MS = 8000;
const size_t   CHUNK_SIZE      = 1024;
const char*    TZ_INFO         = "CET-1CEST,M3.5.0,M10.5.0/3";  // Belgique

// ------------------------------------------------------------- BM8563 (registres PCF8563)
#define REG_CS2    0x01
#define REG_SEC    0x02
#define REG_ALM_MIN 0x09
#define REG_TMR_CTL 0x0E
#define REG_TMR_VAL 0x0F
#define CS2_TIE 0x01
#define CS2_AIE 0x02
#define CS2_TF  0x04
#define CS2_AF  0x08

RTC_DATA_ATTR uint8_t aliveCount = 0;   // perdu à chaque vraie coupure d'alimentation

Preferences  prefs;
WiFiClient   net;
PubSubClient mqtt(net);
String       devId;
char         mqttHost[40] = "192.168.2.106";

// ============================================================= RTC
static uint8_t bcd(uint8_t v)   { return ((v / 10) << 4) | (v % 10); }

uint8_t rtcRead(uint8_t reg) {
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

void rtcWrite(uint8_t reg, uint8_t v) {
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(reg);
    Wire.write(v);
    Wire.endTransmission();
}

bool rtcTimeValid() { return !(rtcRead(REG_SEC) & 0x80); }   // bit VL

void rtcSetTime(const tm& t) {
    rtcWrite(0x00, 0x00);                       // horloge en marche
    rtcWrite(0x02, bcd(t.tm_sec));              // efface aussi VL
    rtcWrite(0x03, bcd(t.tm_min));
    rtcWrite(0x04, bcd(t.tm_hour));
    rtcWrite(0x05, bcd(t.tm_mday));
    rtcWrite(0x06, t.tm_wday);
    rtcWrite(0x07, bcd(t.tm_mon + 1));
    rtcWrite(0x08, bcd(t.tm_year % 100));
}

// Alarme quotidienne : minute + heure actives, jour et jour de semaine ignorés (bit AE = 1)
void rtcArmHeartbeat() {
    rtcWrite(REG_ALM_MIN,     bcd(HB_MIN));
    rtcWrite(REG_ALM_MIN + 1, bcd(HB_HOUR));
    rtcWrite(REG_ALM_MIN + 2, 0x80);
    rtcWrite(REG_ALM_MIN + 3, 0x80);
}

// Compte à rebours en minutes (source 1/60 Hz). Limité à 255 min : pas pour le heartbeat.
void rtcStartCooldown() {
    rtcWrite(REG_TMR_CTL, 0x03);                // TE = 0, source 1/60 Hz
    rtcWrite(REG_TMR_VAL, COOLDOWN_MIN);
    rtcWrite(REG_TMR_CTL, 0x83);                // TE = 1
}

void rtcStopCooldown() { rtcWrite(REG_TMR_CTL, 0x03); }

// Écrit 0 dans AF et TF (efface les drapeaux -> la sortie INT relâche PWR_EN)
void rtcSetIrq(bool timerIrq) {
    rtcWrite(REG_CS2, CS2_AIE | (timerIrq ? CS2_TIE : 0));
}

// ============================================================= CD4013
void pulse(uint8_t pin) {                       // ~10 µs, largeur mini datasheet : 180 ns à 5 V
    digitalWrite(pin, HIGH);
    delayMicroseconds(10);
    digitalWrite(pin, LOW);
}

// ============================================================= batterie
float readBattery() {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BAT_ADC);
    return (sum / 16.0f) * BAT_DIV / 1000.0f;
}

uint8_t batteryPercent(float v) {               // courbe LiPo simplifiée, à repérer en mesure
    const float V[] = {3.30, 3.60, 3.70, 3.80, 3.90, 4.00, 4.20};
    const float P[] = {0,    10,   30,   50,   65,   80,   100};
    if (v <= V[0]) return 0;
    for (int i = 1; i < 7; i++)
        if (v <= V[i]) return P[i - 1] + (v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]);
    return 100;
}

// ============================================================= caméra
bool cameraInit() {
    camera_config_t c = {};
    c.pin_pwdn = -1;  c.pin_reset = 15;  c.pin_xclk = 27;
    c.pin_sccb_sda = 25;  c.pin_sccb_scl = 23;
    c.pin_d7 = 19; c.pin_d6 = 36; c.pin_d5 = 18; c.pin_d4 = 39;
    c.pin_d3 = 5;  c.pin_d2 = 34; c.pin_d1 = 35; c.pin_d0 = 32;
    c.pin_vsync = 22; c.pin_href = 26; c.pin_pclk = 21;
    c.xclk_freq_hz = 20000000;
    c.ledc_timer = LEDC_TIMER_0;  c.ledc_channel = LEDC_CHANNEL_0;
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size = FRAMESIZE_SVGA;              // 800x600, ~30-60 ko
    c.jpeg_quality = 12;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;
    if (esp_camera_init(&c) != ESP_OK) return false;
    sensor_t* s = esp_camera_sensor_get();
    s->set_vflip(s, 1);                         // selon l'orientation dans le nichoir
    return true;
}

// LED IR allumée, 2 images jetées (exposition), 3e image gardée
camera_fb_t* capture() {
    camera_fb_t* fb = nullptr;
    digitalWrite(PIN_REARM_IR, HIGH);           // LED IR ON (met aussi RESET2 à 1 : PIR réarmé)
    for (int i = 0; i < 3; i++) {
        if (fb) esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
    }
    digitalWrite(PIN_REARM_IR, LOW);            // LED IR OFF
    return fb;
}

// ============================================================= réseau
void syncClock() {
    configTzTime(TZ_INFO, "pool.ntp.org");
    tm t;
    if (getLocalTime(&t, 3000)) rtcSetTime(t);
}

bool netConnect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin();                               // identifiants enregistrés par WiFiManager
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(50);
    if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi: échec"); return false; }
    if (!rtcTimeValid()) syncClock();
    mqtt.setServer(mqttHost, MQTT_PORT);
    mqtt.setSocketTimeout(5);
    for (int i = 0; i < 2 && !mqtt.connected(); i++) mqtt.connect(devId.c_str());
    if (!mqtt.connected()) Serial.printf("MQTT: échec rc=%d\n", mqtt.state());
    return mqtt.connected();
}

void netClose() {
    mqtt.disconnect();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void publishEvent(const char* type, float vbat, uint32_t seq, bool photo) {
    char topic[64], msg[160];
    snprintf(topic, sizeof topic, "nichoir/%s/event", devId.c_str());
    snprintf(msg, sizeof msg,
             "{\"type\":\"%s\",\"seq\":%lu,\"batt_mv\":%d,\"batt_pct\":%u,\"rssi\":%d,\"photo\":%s}",
             type, (unsigned long)seq, (int)(vbat * 1000), batteryPercent(vbat),
             WiFi.RSSI(), photo ? "true" : "false");
    mqtt.publish(topic, msg);
}

bool publishPhoto(uint32_t seq, const camera_fb_t* fb) {
    char topic[64];
    snprintf(topic, sizeof topic, "nichoir/%s/photo/%lu", devId.c_str(), (unsigned long)seq);
    if (!mqtt.beginPublish(topic, fb->len, false)) return false;
    for (size_t sent = 0; sent < fb->len;) {
        size_t w = mqtt.write(fb->buf + sent, min(CHUNK_SIZE, fb->len - sent));
        if (w == 0) return false;
        sent += w;
    }
    return mqtt.endPublish();
}

// Première mise en service : point d'accès "Nichoir-Setup" + page web de configuration
void configPortal() {
    WiFiManager wm;
    WiFiManagerParameter pHost("mqtt", "Adresse du broker MQTT (Raspberry Pi)", mqttHost, sizeof mqttHost);
    wm.addParameter(&pHost);
    wm.setConfigPortalTimeout(180);             // 3 min, puis on continue sans réseau
    if (wm.startConfigPortal("Nichoir-Setup")) {
        strlcpy(mqttHost, pHost.getValue(), sizeof mqttHost);
        prefs.putString("mqtt", mqttHost);
        prefs.putBool("cfg", true);
    }
}

// ============================================================= séquences
// A : réveil par le PIR (ou le bouton PWR)
void wakePir() {
    pulse(PIN_BLOCK);                           // PIR bloqué + faux appui relâché
    float vbat = readBattery();                 // mesure AVANT le Wi-Fi
    uint32_t seq = prefs.getUInt("seq", 0) + 1;
    prefs.putUInt("seq", seq);

    camera_fb_t* fb = nullptr;
    if (vbat >= BAT_LOW_V && cameraInit()) fb = capture();
    pulse(PIN_BLOCK);                           // rebloque : G4 a réarmé la bascule 2 pendant la photo

    if (netConnect()) {
        publishEvent(vbat < BAT_LOW_V ? "low_batt" : "pir", vbat, seq, fb != nullptr);
        if (fb && !publishPhoto(seq, fb)) Serial.println("Photo: échec d'envoi");
        netClose();
    }
    if (fb) esp_camera_fb_return(fb);
    prefs.putBool("cool", true);                // délai de 5 min en cours
}

// B : fin du délai
void wakeCooldownEnd() {
    pulse(PIN_REARM_IR);                        // PIR réarmé (10 µs : LED IR invisible)
    prefs.putBool("cool", false);
}

// C : heartbeat quotidien
void wakeHeartbeat() {
    float vbat = readBattery();
    if (!prefs.getBool("cool", false)) pulse(PIN_REARM_IR);   // réarmement de sécurité
    if (netConnect()) {
        syncClock();                            // recale la RTC une fois par jour
        publishEvent("heartbeat", vbat, prefs.getUInt("seq", 0), false);
        netClose();
    }
}

// Programme les réveils, efface les drapeaux, coupe l'alimentation
void shutdown(bool startCooldown) {
    rtcArmHeartbeat();                          // TOUJOURS un réveil programmé
    bool cool = prefs.getBool("cool", false);
    if (startCooldown)  rtcStartCooldown();
    else if (!cool)     rtcStopCooldown();      // en heartbeat pendant un délai : timer conservé
    rtcSetIrq(cool);
    prefs.end();
    Serial.println("Extinction");
    Serial.flush();
    digitalWrite(PIN_HOLD, LOW);
    delay(500);

    // Toujours alimenté : USB branché, ou front PIR arrivé juste après le réarmement
    if (++aliveCount <= 2) ESP.restart();       // repasse par setup() -> séquence A
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);   // sur USB : évite une boucle de photos
    esp_deep_sleep_start();
}

// ============================================================= Arduino
void setup() {
    pinMode(PIN_HOLD, OUTPUT);
    digitalWrite(PIN_HOLD, HIGH);               // 1re action : maintenir l'alimentation
    pinMode(PIN_BLOCK, OUTPUT);     digitalWrite(PIN_BLOCK, LOW);
    pinMode(PIN_REARM_IR, OUTPUT);  digitalWrite(PIN_REARM_IR, LOW);
    Serial.begin(115200);
    Wire.begin(RTC_SDA, RTC_SCL);
    prefs.begin("nichoir", false);
    devId = "cam-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    prefs.getString("mqtt", mqttHost).toCharArray(mqttHost, sizeof mqttHost);

    uint8_t cs2 = rtcRead(REG_CS2);
    bool tf = cs2 & CS2_TF;
    bool af = cs2 & CS2_AF;
    Serial.printf("\nRéveil : TF=%d AF=%d\n", tf, af);

    if (!prefs.getBool("cfg", false)) {         // première mise en service
        pulse(PIN_BLOCK);                       // relâche le faux appui pendant le portail
        configPortal();
    }

    if (tf) wakeCooldownEnd();
    if (af) wakeHeartbeat();
    if (!tf && !af) {
        wakePir();
        shutdown(true);
    }
    shutdown(false);
}

void loop() {}
