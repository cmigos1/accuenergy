/**
 * @file    web_server.h
 * @brief   Async Web Server with WebSocket for real-time NILM data
 *          Runs on Core 1 (default Arduino core)
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "nilm_data.h"

/**
 * @brief  Initialize WiFi (AP mode) and start the async web server
 */
void web_server_init();

/**
 * @brief  Broadcast updated NILM data to all connected WebSocket clients
 * @param  data  Pointer to calibrated nilm_data_t
 */
void web_server_broadcast(const nilm_data_t *data);

#endif /* WEB_SERVER_H */
