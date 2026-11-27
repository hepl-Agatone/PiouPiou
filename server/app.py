"""Serveur web du nichoir : galerie des photos et état de la batterie."""
import os
from pathlib import Path

from flask import Flask, abort, render_template, send_from_directory

from db import connect

PHOTO_DIR = Path(os.getenv("PHOTO_DIR", "/var/lib/nichoir/photos"))
app = Flask(__name__)


def query(sql, args=()):
    with connect() as db, db.cursor() as c:
        c.execute(sql, args)
        return c.fetchall()


@app.route("/")
def gallery():
    photos = query(
        "SELECT p.filename, p.received_at, e.device, e.batt_pct, e.batt_mv "
        "FROM photos p LEFT JOIN events e ON e.id = p.event_id "
        "ORDER BY p.received_at DESC LIMIT 60"
    )
    status = query(
        "SELECT device, type, batt_pct, batt_mv, rssi, received_at FROM events e "
        "WHERE id = (SELECT MAX(id) FROM events WHERE device = e.device)"
    )
    stats = query(
        "SELECT COUNT(*) AS n, SUM(type='pir') AS pir FROM events "
        "WHERE received_at > NOW() - INTERVAL 1 DAY"
    )[0]
    return render_template("index.html", photos=photos, status=status, stats=stats)


@app.route("/photos/<path:filename>")
def photo(filename):
    if not filename.endswith(".jpg"):
        abort(404)
    return send_from_directory(PHOTO_DIR, filename)    # refuse les chemins hors du dossier


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.getenv("WEB_PORT", "5000")))
