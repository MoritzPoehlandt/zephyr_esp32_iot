/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#ifdef CONFIG_NET_CONFIG_SETTINGS
#ifdef CONFIG_NET_IPV6
#define ZEPHYR_ADDR		CONFIG_NET_CONFIG_MY_IPV6_ADDR
#define SERVER_ADDR		CONFIG_NET_CONFIG_PEER_IPV6_ADDR
#else
#define ZEPHYR_ADDR		CONFIG_NET_CONFIG_MY_IPV4_ADDR
#define SERVER_ADDR		CONFIG_NET_CONFIG_PEER_IPV4_ADDR
#endif
#else
#ifdef CONFIG_NET_IPV6
#define ZEPHYR_ADDR		"2001:db8::1"
#define SERVER_ADDR		"2001:db8::2"
#else
#define ZEPHYR_ADDR		"192.168.178.98"
#define SERVER_ADDR		"85.215.54.37"
#endif
#endif

#if defined(CONFIG_SOCKS)
#define SOCKS5_PROXY_ADDR	SERVER_ADDR
#define SOCKS5_PROXY_PORT	1080
#endif

#ifdef CONFIG_MQTT_LIB_WEBSOCKET
#define SERVER_PORT		9001
#else
#define SERVER_PORT		1883
#endif /* CONFIG_MQTT_LIB_WEBSOCKET */

#define APP_CONNECT_TIMEOUT_MS	2000
#define APP_SLEEP_MSECS		500

#define APP_CONNECT_TRIES	10

#define APP_MQTT_BUFFER_SIZE	128

#define MQTT_CLIENTID		"esp32-zephyr"
#define MQTT_USERNAME		"ESP32"
#define MQTT_PASSWORD		"1234qwer"

/* Set the following to 1 to enable the Bluemix topic format */
#define APP_BLUEMIX_TOPIC	1

/* These are the parameters for the Bluemix topic format */
#if APP_BLUEMIX_TOPIC
#define BLUEMIX_DEVTYPE		"esp32"
#define BLUEMIX_DEVID		"001"
#define BLUEMIX_EVENT		"water_level"
#define BLUEMIX_FORMAT		"json"
#endif

#endif
