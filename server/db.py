"""Connexion MariaDB partagée par le listener et le serveur web."""
import os

import pymysql


def connect():
    return pymysql.connect(
        host=os.getenv("DB_HOST", "localhost"),
        user=os.getenv("DB_USER", "nichoir"),
        password=os.getenv("DB_PASSWORD", "change-moi"),
        database=os.getenv("DB_NAME", "nichoir"),
        autocommit=True,
        cursorclass=pymysql.cursors.DictCursor,
    )
