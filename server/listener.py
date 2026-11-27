"""Listener MQTT du nichoir : stocke les événements et les photos.

Topics (publiés par la TimerCam) :
    nichoir/<id>/event        JSON {"type","seq","batt_mv","batt_pct","rssi","photo"}
    nichoir/<id>/photo/<seq>  JPEG binaire
"""
import json
import logging
import os
from datetime import datetime
from pathlib import Path

import paho.mqtt.client as mqtt
import pymysql

from db import connect

MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
PHOTO_DIR = Path(os.getenv("PHOTO_DIR", "/var/lib/nichoir/photos"))

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("listener")
db = None


def cursor():
    """Curseur MariaDB, avec reconnexion si le serveur a coupé la connexion."""
    global db
    if db is None:
        db = connect()
    db.ping(reconnect=True)
    return db.cursor()


def save_event(device, payload):
    data = json.loads(payload)
    with cursor() as c:
        c.execute(
            "INSERT INTO events (device, type, seq, batt_mv, batt_pct, rssi, has_photo) "
            "VALUES (%s, %s, %s, %s, %s, %s, %s)",
            (device, data["type"], data["seq"], data.get("batt_mv"),
             data.get("batt_pct"), data.get("rssi"), bool(data.get("photo"))),
        )
    log.info("Événement %s de %s (seq %s, %s mV)", data["type"], device, data["seq"], data.get("batt_mv"))


def save_photo(device, seq, payload):
    if payload[:2] != b"\xff\xd8":                     # en-tête JPEG
        log.warning("Photo %s/%s ignorée : pas un JPEG", device, seq)
        return
    PHOTO_DIR.mkdir(parents=True, exist_ok=True)
    filename = f"{device}_{seq}_{datetime.now():%Y%m%d-%H%M%S}.jpg"
    (PHOTO_DIR / filename).write_bytes(payload)
    with cursor() as c:
        # L'événement est publié juste avant la photo, avec le même seq
        c.execute("SELECT id FROM events WHERE device=%s AND seq=%s ORDER BY id DESC LIMIT 1",
                  (device, seq))
        row = c.fetchone()
        c.execute("INSERT INTO photos (event_id, filename, size_bytes) VALUES (%s, %s, %s)",
                  (row["id"] if row else None, filename, len(payload)))
    log.info("Photo %s enregistrée (%d octets)", filename, len(payload))


def on_connect(client, userdata, flags, reason_code, properties):
    log.info("Connecté au broker (%s)", reason_code)
    client.subscribe("nichoir/#")                      # réabonnement à chaque reconnexion


def on_message(client, userdata, msg):
    parts = msg.topic.split("/")                       # nichoir/<id>/event | nichoir/<id>/photo/<seq>
    try:
        if len(parts) == 3 and parts[2] == "event":
            save_event(parts[1], msg.payload)
        elif len(parts) == 4 and parts[2] == "photo":
            save_photo(parts[1], int(parts[3]), msg.payload)
    except (ValueError, KeyError, pymysql.MySQLError) as e:
        log.error("Message %s non traité : %s", msg.topic, e)


def main():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="nichoir-listener")
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    client.connect(MQTT_HOST, MQTT_PORT)
    client.loop_forever()                              # gère aussi les reconnexions


if __name__ == "__main__":
    main()
