# Firmware TimerCam

Firmware du nœud : M5Stack TimerCam (ESP32 + OV3660 + RTC BM8563).

## Principe

Chaque réveil exécute **une seule séquence** dans `setup()`, puis coupe l'alimentation (G33 = 0).
`loop()` n'est jamais atteinte.

| Cause du réveil (registre 0x01 du BM8563) | Séquence | Action |
|---|---|---|
| Aucun drapeau | A : PIR ou bouton | Bloque le PIR (G13), photo avec LED IR, MQTT, lance le délai de 5 min |
| `TF = 1` | B : fin du délai | Réarme le PIR (impulsion G4) |
| `AF = 1` | C : heartbeat quotidien | Niveau batterie en MQTT, recalage de la RTC (NTP) |

## Compilation

- Carte : M5Stack > **M5TimerCAM** (paquet de cartes M5Stack pour Arduino IDE)
- Bibliothèques : **PubSubClient** (Nick O'Leary), **WiFiManager** (tzapu)
- Caméra (`esp_camera`) et RTC (`Wire`) pilotées directement, sans la bibliothèque M5

## Première mise en service

1. Téléverser par USB.
2. Au premier démarrage, la carte ouvre le point d'accès **Nichoir-Setup** :
   choisir le Wi-Fi et saisir l'adresse IP du broker MQTT (Raspberry Pi).
3. Débrancher l'USB : sur USB la carte ne peut pas se couper (elle passe alors en deep sleep).

## Réglages (haut du fichier)

| Constante | Rôle | Valeur |
|---|---|---|
| `COOLDOWN_MIN` | Délai entre deux photos, en minutes (timer RTC 1/60 Hz) | 6 (= 5 à 6 min) ; 1 pour les tests |
| `HB_HOUR`, `HB_MIN` | Heure du heartbeat quotidien | 12:00 |
| `BAT_DIV` | Rapport du pont diviseur sur G38 | 1,51, à calibrer au multimètre |
| `BAT_LOW_V` | Seuil sous lequel aucune photo n'est prise | 3,30 V |

## Messages MQTT

| Topic | Contenu |
|---|---|
| `nichoir/<id>/event` | JSON : `{"type":"pir","seq":42,"batt_mv":3920,"batt_pct":78,"rssi":-61,"photo":true}` (`type` = `pir`, `heartbeat` ou `low_batt`) |
| `nichoir/<id>/photo/<seq>` | JPEG binaire (800×600), même `seq` que l'événement |
