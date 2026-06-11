#ifndef CONFIG_H
#define CONFIG_H

/* ── Wi-Fi ───────────────────────────────────────────────────────────────── */
#define WIFI_SSID       "AMANDA 2.4G"
#define WIFI_PASS       "Am2026@10"

/* ── MQTT broker ─────────────────────────────────────────────────────────── */
#define MQTT_HOST       "192.168.1.91"   /* Mosquitto local */
#define MQTT_PORT       1883
#define MQTT_TOPIC      "energia/medidor"
#define MQTT_CLIENT_ID  "esp32-developer"

/* ── Correção de hardware ──────────────────────────────────────────────────── */
/* CT da fase 1 está fisicamente invertido no fio (não é possível reorientar nem
   alterar o STM32). Negar Preal/Q/FP da fase 1 para leituras positivas. */
#define PHASE1_CT_INVERTED  1

/* ── NTP ─────────────────────────────────────────────────────────────────── */
#define NTP_SERVER      "pool.ntp.org"
#define NTP_TZ          "BRT+4"           /* UTC-4 (horário de Manaus) */

#endif /* CONFIG_H */
