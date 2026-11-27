# Serveur Raspberry Pi

```
TimerCam ──MQTT──▶ Mosquitto ──▶ listener.py ──INSERT──▶ MariaDB ◀──SELECT── app.py (Flask) ──HTTP──▶ navigateur
                                       └──▶ photos .jpg sur disque ◀──────────────┘
```

| Fichier | Rôle |
|---|---|
| `listener.py` | S'abonne à `nichoir/#`, enregistre les événements et les photos |
| `app.py` + `templates/index.html` | Galerie (60 dernières photos) et état de la batterie, rafraîchie toutes les minutes |
| `db.py` | Connexion MariaDB commune |
| `schema.sql` | Tables `events` et `photos`, utilisateur `nichoir` |
| `systemd/` | Lancement automatique au démarrage du Pi |

Les images sont stockées **sur disque** et la base garde leur nom de fichier : plus léger qu'un BLOB,
et Flask les sert directement. L'heure est mise **par le Pi à la réception** (la TimerCam n'envoie pas d'horodatage).

## Installation

```bash
sudo apt install mosquitto mosquitto-clients mariadb-server python3-venv
sudo mariadb < schema.sql                       # puis changer le mot de passe 'change-moi'

cd ~/PiouPiou/server
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

sudo cp nichoir.env.example /etc/nichoir.env    # adapter DB_PASSWORD
sudo mkdir -p /var/lib/nichoir/photos && sudo chown pi /var/lib/nichoir/photos
```

Mosquitto 2.x n'accepte que les connexions locales par défaut. Pour que la TimerCam puisse se connecter,
ajouter dans `/etc/mosquitto/conf.d/nichoir.conf` :

```
listener 1883
allow_anonymous true
```

puis `sudo systemctl restart mosquitto`.

## Lancement

Test manuel :

```bash
set -a; . /etc/nichoir.env; set +a
.venv/bin/python listener.py      # terminal 1
.venv/bin/python app.py           # terminal 2 -> http://<ip-du-pi>:5000
mosquitto_sub -v -t 'nichoir/#'   # terminal 3 : voir passer les messages
```

Au démarrage du Pi :

```bash
sudo cp systemd/*.service /etc/systemd/system/
sudo systemctl enable --now nichoir-listener nichoir-web
journalctl -u nichoir-listener -f                # journaux
```

## Test sans la TimerCam

```bash
mosquitto_pub -t nichoir/test/event -m '{"type":"pir","seq":1,"batt_mv":3900,"batt_pct":75,"rssi":-60,"photo":true}'
mosquitto_pub -t nichoir/test/photo/1 -f une_image.jpg
```
