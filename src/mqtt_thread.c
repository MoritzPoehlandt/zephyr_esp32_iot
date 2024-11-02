/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(net_mqtt_publisher, LOG_LEVEL_DBG);

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/random/random.h>

#include <zephyr/drivers/sensor_data_types.h>
#include <zephyr/dsp/print_format.h>
#include <zephyr/rtio/rtio.h>

#include <string.h>
#include <errno.h>

#include "config.h"
#include "msgq.h" 

/* size of stack area used by each thread */
#define STACKSIZE 2048

/* scheduling priority used by each thread */
#define PRIORITY 7

#define APP_BMEM
#define APP_DMEM

/* Thread creation */
static int run_mqtt();

K_THREAD_DEFINE(mqtt_thread, STACKSIZE, run_mqtt, NULL, NULL, NULL,
		PRIORITY, 0, 0);


/* Buffers for MQTT client. */
static APP_BMEM uint8_t rx_buffer[APP_MQTT_BUFFER_SIZE];
static APP_BMEM uint8_t tx_buffer[APP_MQTT_BUFFER_SIZE];

#if defined(CONFIG_MQTT_LIB_WEBSOCKET)
/* Making RX buffer large enough that the full IPv6 packet can fit into it */
#define MQTT_LIB_WEBSOCKET_RECV_BUF_LEN 1280

/* Websocket needs temporary buffer to store partial packets */
static APP_BMEM uint8_t temp_ws_rx_buf[MQTT_LIB_WEBSOCKET_RECV_BUF_LEN];
#endif

/* The mqtt client struct */
static APP_BMEM struct mqtt_client client_ctx;

/* MQTT Broker details. */
static APP_BMEM struct sockaddr_storage broker;

#if defined(CONFIG_SOCKS)
static APP_BMEM struct sockaddr socks5_proxy;
#endif

static APP_BMEM struct pollfd fds[1];
static APP_BMEM int nfds;

static APP_BMEM bool connected;

static void prepare_fds(struct mqtt_client *client)
{
	if (client->transport.type == MQTT_TRANSPORT_NON_SECURE) {
		fds[0].fd = client->transport.tcp.sock;
	}
#if defined(CONFIG_MQTT_LIB_TLS)
	else if (client->transport.type == MQTT_TRANSPORT_SECURE) {
		fds[0].fd = client->transport.tls.sock;
	}
#endif

	fds[0].events = POLLIN;
	nfds = 1;
}

static void clear_fds(void)
{
	nfds = 0;
}

static int wait(int timeout)
{
	int ret = 0;

	if (nfds > 0) {
		ret = poll(fds, nfds, timeout);
		if (ret < 0) {
			LOG_ERR("poll error: %d", errno);
		}
	}

	return ret;
}

void mqtt_evt_handler(struct mqtt_client *const client,
		      const struct mqtt_evt *evt)
{
	int err;

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT connect failed %d", evt->result);
			break;
		}

		connected = true;
		LOG_INF("MQTT client connected!");

		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT client disconnected %d", evt->result);

		connected = false;
		clear_fds();

		break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBACK error %d", evt->result);
			break;
		}

		LOG_INF("PUBACK packet id: %u", evt->param.puback.message_id);

		break;

	case MQTT_EVT_PUBREC:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBREC error %d", evt->result);
			break;
		}

		LOG_INF("PUBREC packet id: %u", evt->param.pubrec.message_id);

		const struct mqtt_pubrel_param rel_param = {
			.message_id = evt->param.pubrec.message_id
		};

		err = mqtt_publish_qos2_release(client, &rel_param);
		if (err != 0) {
			LOG_ERR("Failed to send MQTT PUBREL: %d", err);
		}

		break;

	case MQTT_EVT_PUBCOMP:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBCOMP error %d", evt->result);
			break;
		}

		LOG_INF("PUBCOMP packet id: %u",
			evt->param.pubcomp.message_id);

		break;

	case MQTT_EVT_PINGRESP:
		LOG_INF("PINGRESP packet");
		break;

	default:
		break;
	}
}

static char *get_mqtt_payload(enum mqtt_qos qos)
{

    int32_t waterlevel = 0;

#if APP_BLUEMIX_TOPIC
	static APP_BMEM char payload[80];

    k_msgq_get(&waterlevel_msgq, &waterlevel, K_FOREVER);

	snprintk(payload, sizeof(payload),  
		"{\"waterlevel\": %d}", waterlevel);
#else
	static APP_DMEM char payload[] = "DOORS:OPEN_QoSx";

	payload[strlen(payload) - 1] = '0' + qos;
#endif

	return payload;
}

static char *get_mqtt_topic(void)
{
#if APP_BLUEMIX_TOPIC
	return "iot/type/"BLUEMIX_DEVTYPE"/id/"BLUEMIX_DEVID
	       "/evt/"BLUEMIX_EVENT"/fmt/"BLUEMIX_FORMAT;
#else
	return "sensors";
#endif
}

static int publish(struct mqtt_client *client, enum mqtt_qos qos)
{
	struct mqtt_publish_param param;

	param.message.topic.qos = qos;
	param.message.topic.topic.utf8 = (uint8_t *)get_mqtt_topic();
	param.message.topic.topic.size =
			strlen(param.message.topic.topic.utf8);
	param.message.payload.data = get_mqtt_payload(qos);
	param.message.payload.len =
			strlen(param.message.payload.data);
	param.message_id = sys_rand16_get();
	param.dup_flag = 0U;
	param.retain_flag = 0U;

	return mqtt_publish(client, &param);
}

#define RC_STR(rc) ((rc) == 0 ? "OK" : "ERROR")

#define PRINT_RESULT(func, rc) \
	LOG_INF("%s: %d <%s>", (func), rc, RC_STR(rc))

static void broker_init(void)
{
#if defined(CONFIG_NET_IPV6)
	struct sockaddr_in6 *broker6 = (struct sockaddr_in6 *)&broker;

	broker6->sin6_family = AF_INET6;
	broker6->sin6_port = htons(SERVER_PORT);
	inet_pton(AF_INET6, SERVER_ADDR, &broker6->sin6_addr);

#if defined(CONFIG_SOCKS)
	struct sockaddr_in6 *proxy6 = (struct sockaddr_in6 *)&socks5_proxy;

	proxy6->sin6_family = AF_INET6;
	proxy6->sin6_port = htons(SOCKS5_PROXY_PORT);
	inet_pton(AF_INET6, SOCKS5_PROXY_ADDR, &proxy6->sin6_addr);
#endif
#else
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker;

	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(SERVER_PORT);
	inet_pton(AF_INET, SERVER_ADDR, &broker4->sin_addr);
#if defined(CONFIG_SOCKS)
	struct sockaddr_in *proxy4 = (struct sockaddr_in *)&socks5_proxy;

	proxy4->sin_family = AF_INET;
	proxy4->sin_port = htons(SOCKS5_PROXY_PORT);
	inet_pton(AF_INET, SOCKS5_PROXY_ADDR, &proxy4->sin_addr);
#endif
#endif
}

static void client_init(struct mqtt_client *client)
{
	mqtt_client_init(client);

	broker_init();

    /* MQTT username and password*/
    static struct mqtt_utf8 password;
	static struct mqtt_utf8 user_name;

	password.utf8 = (uint8_t*)MQTT_PASSWORD;
	password.size = strlen(MQTT_PASSWORD);
	user_name.utf8 = (uint8_t*)MQTT_USERNAME;
	user_name.size = strlen(MQTT_USERNAME);

	/* MQTT client configuration */
	client->broker = &broker;
	client->evt_cb = mqtt_evt_handler;
	client->client_id.utf8 = (uint8_t *)MQTT_CLIENTID;
	client->client_id.size = strlen(MQTT_CLIENTID);
	client->password = &password;
	client->user_name = &user_name;
	client->protocol_version = MQTT_VERSION_3_1_1;

	/* MQTT buffers configuration */
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

#if defined(CONFIG_MQTT_LIB_WEBSOCKET)
	client->transport.type = MQTT_TRANSPORT_NON_SECURE_WEBSOCKET;
#else
	client->transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif

#if defined(CONFIG_MQTT_LIB_WEBSOCKET)
	client->transport.websocket.config.host = SERVER_ADDR;
	client->transport.websocket.config.url = "/mqtt";
	client->transport.websocket.config.tmp_buf = temp_ws_rx_buf;
	client->transport.websocket.config.tmp_buf_len =
						sizeof(temp_ws_rx_buf);
	client->transport.websocket.timeout = 5 * MSEC_PER_SEC;
#endif

#if defined(CONFIG_SOCKS)
	mqtt_client_set_proxy(client, &socks5_proxy,
			      socks5_proxy.sa_family == AF_INET ?
			      sizeof(struct sockaddr_in) :
			      sizeof(struct sockaddr_in6));
#endif
}

/* In this routine we block until the connected variable is 1 */
static int try_to_connect(struct mqtt_client *client)
{
	int rc, i = 0;

	while (i++ < APP_CONNECT_TRIES && !connected) {

		client_init(client);

		rc = mqtt_connect(client);
		if (rc != 0) {
			PRINT_RESULT("mqtt_connect", rc);
			k_sleep(K_MSEC(APP_SLEEP_MSECS));
			continue;
		}

		prepare_fds(client);

		if (wait(APP_CONNECT_TIMEOUT_MS)) {
			mqtt_input(client);
		}

		if (!connected) {
			mqtt_abort(client);
		}
	}

	if (connected) {
		return 0;
	}

	return -EINVAL;
}

static int process_mqtt_and_sleep(struct mqtt_client *client, int timeout)
{
	int64_t remaining = timeout;
	int64_t start_time = k_uptime_get();
	int rc;

	while (remaining > 0 && connected) {
		if (wait(remaining)) {
			rc = mqtt_input(client);
			if (rc != 0) {
				PRINT_RESULT("mqtt_input", rc);
				return rc;
			}
		}

		rc = mqtt_live(client);
		if (rc != 0 && rc != -EAGAIN) {
			PRINT_RESULT("mqtt_live", rc);
			return rc;
		} else if (rc == 0) {
			rc = mqtt_input(client);
			if (rc != 0) {
				PRINT_RESULT("mqtt_input", rc);
				return rc;
			}
		}

		remaining = timeout + start_time - k_uptime_get();
	}

	return 0;
}

#define SUCCESS_OR_EXIT(rc) { if (rc != 0) { return 1; } }
#define SUCCESS_OR_BREAK(rc) { if (rc != 0) { break; } }

static int publisher(void)
{
	int i, rc, r = 0;

	LOG_INF("attempting to connect: ");
	rc = try_to_connect(&client_ctx);
	PRINT_RESULT("try_to_connect", rc);
	SUCCESS_OR_EXIT(rc);

	i = 0;
	while (i++ < CONFIG_NET_SAMPLE_APP_MAX_ITERATIONS && connected) {
		r = -1;

		rc = mqtt_ping(&client_ctx);
		PRINT_RESULT("mqtt_ping", rc);
		SUCCESS_OR_BREAK(rc);

		rc = process_mqtt_and_sleep(&client_ctx, APP_SLEEP_MSECS);
		SUCCESS_OR_BREAK(rc);

		rc = publish(&client_ctx, MQTT_QOS_0_AT_MOST_ONCE);
		PRINT_RESULT("mqtt_publish", rc);
		SUCCESS_OR_BREAK(rc);

		rc = process_mqtt_and_sleep(&client_ctx, APP_SLEEP_MSECS);
		SUCCESS_OR_BREAK(rc);

		rc = publish(&client_ctx, MQTT_QOS_1_AT_LEAST_ONCE);
		PRINT_RESULT("mqtt_publish", rc);
		SUCCESS_OR_BREAK(rc);

		rc = process_mqtt_and_sleep(&client_ctx, APP_SLEEP_MSECS);
		SUCCESS_OR_BREAK(rc);

		rc = publish(&client_ctx, MQTT_QOS_2_EXACTLY_ONCE);
		PRINT_RESULT("mqtt_publish", rc);
		SUCCESS_OR_BREAK(rc);

		rc = process_mqtt_and_sleep(&client_ctx, APP_SLEEP_MSECS);
		SUCCESS_OR_BREAK(rc);

		r = 0;
	}

	rc = mqtt_disconnect(&client_ctx);
	PRINT_RESULT("mqtt_disconnect", rc);

	LOG_INF("Bye!");

	return r;
}

static int run_mqtt(void)
{
	int r = 0, i = 0;

	while (!CONFIG_NET_SAMPLE_APP_MAX_CONNECTIONS ||
	       i++ < CONFIG_NET_SAMPLE_APP_MAX_CONNECTIONS) {
		r = publisher();

		if (!CONFIG_NET_SAMPLE_APP_MAX_CONNECTIONS) {
			k_sleep(K_MSEC(5000));
		}
	}

	return r;
}