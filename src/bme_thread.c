#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor_data_types.h>
#include <zephyr/rtio/rtio.h>
#include <zephyr/dsp/print_format.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bme_sensor, LOG_LEVEL_DBG);

#include "msgq.h"
#include "mutex.h"

#define MSG_QUEUE_SIZE 10

// Definition der Message Queue
K_MSGQ_DEFINE(temp_msgq, sizeof(q31_t), MSG_QUEUE_SIZE, 4);
K_MSGQ_DEFINE(press_msgq, sizeof(q31_t), MSG_QUEUE_SIZE, 4);
K_MSGQ_DEFINE(humidity_msgq, sizeof(q31_t), MSG_QUEUE_SIZE, 4);

K_MUTEX_DEFINE(temp_mutex);
K_MUTEX_DEFINE(press_mutex);
K_MUTEX_DEFINE(humidity_mutex);

/* size of stack area used by each thread */
#define STACKSIZE 1024

/* scheduling priority used by each thread */
#define PRIORITY 7

/* Thread creation */
static int run_bme();

K_THREAD_DEFINE(bme_thread, STACKSIZE,
		run_bme, NULL, NULL, NULL,
		PRIORITY, 0, 0);

/*
 * Get a device structure from a devicetree node with compatible
 * "bosch,bme280". (If there are multiple, just pick one.)
 */
const struct device *const dev = DEVICE_DT_GET_ANY(bosch_bme280);

SENSOR_DT_READ_IODEV(iodev, DT_COMPAT_GET_ANY_STATUS_OKAY(bosch_bme280),
		{SENSOR_CHAN_AMBIENT_TEMP, 0},
		{SENSOR_CHAN_HUMIDITY, 0},
		{SENSOR_CHAN_PRESS, 0});

RTIO_DEFINE(ctx, 1, 1);

static const struct device *check_bme280_device(void)
{
	if (dev == NULL) {
		/* No such node, or the node does not have status "okay". */
		printk("\nError: no device found.\n");
		return NULL;
	}

	if (!device_is_ready(dev)) {
		printk("\nError: Device \"%s\" is not ready; "
		       "check the driver initialization logs for errors.\n",
		       dev->name);
		return NULL;
	}

	printk("Found device \"%s\", getting sensor data\n", dev->name);
	return dev;
}

static int run_bme() {

	LOG_INF("BME Threads started ");


    const struct device *dev = check_bme280_device();

	if (dev == NULL) {
		return 0;
	}

	while (1) {
		uint8_t buf[128];

		int rc = sensor_read(&iodev, &ctx, buf, 128);

		if (rc != 0) {
			printk("%s: sensor_read() failed: %d\n", dev->name, rc);
			return rc;
		}

		const struct sensor_decoder_api *decoder;

		rc = sensor_get_decoder(dev, &decoder);

		if (rc != 0) {
			printk("%s: sensor_get_decode() failed: %d\n", dev->name, rc);
			return rc;
		}

		uint32_t temp_fit = 0;
		struct sensor_q31_data temp_data = {0};

		decoder->decode(buf,
			(struct sensor_chan_spec) {SENSOR_CHAN_AMBIENT_TEMP, 0},
			&temp_fit, 1, &temp_data);

		uint32_t press_fit = 0;
		struct sensor_q31_data press_data = {0};

		decoder->decode(buf,
				(struct sensor_chan_spec) {SENSOR_CHAN_PRESS, 0},
				&press_fit, 1, &press_data);

		uint32_t hum_fit = 0;
		struct sensor_q31_data hum_data = {0};

		decoder->decode(buf,
				(struct sensor_chan_spec) {SENSOR_CHAN_HUMIDITY, 0},
				&hum_fit, 1, &hum_data);
		
		q31_t temperature = temp_data.readings[0].temperature;
		q31_t pressure = temp_data.readings[0].pressure;
		q31_t humidity = temp_data.readings[0].humidity;

        k_msgq_put(&temp_msgq, &temperature, K_NO_WAIT);
        k_msgq_put(&press_msgq, &pressure, K_NO_WAIT);
        k_msgq_put(&humidity_msgq, &humidity, K_NO_WAIT);

		// printk("temp: %s%d.%d; press: %s%d.%d; humidity: %s%d.%d\n",
		// PRIq_arg(temp_data.readings[0].temperature, 6, temp_data.shift),
		// 	PRIq_arg(press_data.readings[0].pressure, 6, press_data.shift),
		//  PRIq_arg(hum_data.readings[0].humidity, 6, hum_data.shift));

		k_sleep(K_MSEC(5000));
	}
	return 0;
}
