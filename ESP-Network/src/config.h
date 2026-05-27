#ifndef CONFIG_H
#define CONFIG_H

/* ── Wi-Fi ───────────────────────────────────────────────────────────────── */
#define WIFI_SSID       "S23"
#define WIFI_PASS       "Mqtt1234"

/* ── MQTT broker ─────────────────────────────────────────────────────────── */
#define MQTT_HOST       "b038913592104262bbc1d6c9c55f6a01.s1.eu.hivemq.cloud"   /* URL do cluster no HiveMQ Cloud */
#define MQTT_PORT       8883
#define MQTT_TOPIC      "energia/medidor"
#define MQTT_CLIENT_ID  "esp32-developer"
#define MQTT_USER       "accuenergy"
#define MQTT_PASS       ">!wv;5Y9L4=h.ZX"

/* ── NTP ─────────────────────────────────────────────────────────────────── */
#define NTP_SERVER      "pool.ntp.org"
#define NTP_TZ          "BRT+4"           /* UTC-4 (horário de Manaus) */

#endif /* CONFIG_H */
