-- Base de données du nichoir (MariaDB)
-- Usage : sudo mariadb < schema.sql
-- Les images sont stockées sur disque (PHOTO_DIR) ; la base garde le chemin.
-- Plus léger qu'un BLOB, et Flask les sert directement comme fichiers.

CREATE DATABASE IF NOT EXISTS nichoir CHARACTER SET utf8mb4;
USE nichoir;

CREATE TABLE IF NOT EXISTS events (
    id          INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    device      VARCHAR(32)  NOT NULL,                 -- ex. cam-a1b2c3d4
    type        ENUM('pir', 'heartbeat', 'low_batt') NOT NULL,
    seq         INT UNSIGNED NOT NULL,                 -- numéro envoyé par la TimerCam
    batt_mv     SMALLINT UNSIGNED,
    batt_pct    TINYINT UNSIGNED,
    rssi        SMALLINT,
    has_photo   BOOLEAN      NOT NULL DEFAULT FALSE,   -- photo annoncée par l'événement
    received_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,  -- horodatage par le Pi
    INDEX idx_device_seq (device, seq),
    INDEX idx_received (received_at)
);

CREATE TABLE IF NOT EXISTS photos (
    id          INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    event_id    INT UNSIGNED NULL,
    filename    VARCHAR(128) NOT NULL UNIQUE,
    size_bytes  INT UNSIGNED NOT NULL,
    received_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (event_id) REFERENCES events(id) ON DELETE SET NULL
);

-- Utilisateur dédié : changer le mot de passe, et le reporter dans /etc/nichoir.env
CREATE USER IF NOT EXISTS 'nichoir'@'localhost' IDENTIFIED BY 'change-moi';
GRANT SELECT, INSERT ON nichoir.* TO 'nichoir'@'localhost';
FLUSH PRIVILEGES;
