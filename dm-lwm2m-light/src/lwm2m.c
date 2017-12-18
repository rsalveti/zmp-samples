/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/lwm2m"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_FOTA_LEVEL
#include <logging/sys_log.h>

#include <zephyr.h>
#include <misc/reboot.h>
#include <net/net_mgmt.h>
#include <net/lwm2m.h>
#include <stdio.h>
#include <version.h>
#include <board.h>
#include <tc_util.h>

#include "flash_block.h"
#include "mcuboot.h"
#include "product_id.h"
#include "lwm2m_credentials.h"
#include "app_work_queue.h"

#define WAIT_TIME	K_SECONDS(10)
#define CONNECT_TIME	K_SECONDS(10)

/* Network configuration checks */
#if defined(CONFIG_NET_IPV6)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV6_ADDR) > 1,
		"CONFIG_NET_APP_PEER_IPV6_ADDR must be defined in boards/$(BOARD)-local.conf");
#endif
#if defined(CONFIG_NET_IPV4)
#if !defined(CONFIG_NET_DHCPV4)
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_MY_IPV4_ADDR) > 1,
		"DHCPv4 must be enabled, or CONFIG_NET_APP_MY_IPV4_ADDR must be defined, in boards/$(BOARD)-local.conf");
#endif
BUILD_ASSERT_MSG(sizeof(CONFIG_NET_APP_PEER_IPV4_ADDR) > 1,
		"CONFIG_NET_APP_PEER_IPV4_ADDR must be defined in boards/$(BOARD)-local.conf");
#endif
#if defined(CONFIG_LWM2M_FIRMWARE_UPDATE_PULL_COAP_PROXY_SUPPORT)
BUILD_ASSERT_MSG(sizeof(CONFIG_LWM2M_FIRMWARE_UPDATE_PULL_COAP_PROXY_ADDR) > 1,
		"CONFIG_LWM2M_FIRMWARE_UPDATE_PULL_COAP_PROXY_ADDR must be defined in boards/$(BOARD)-local.conf");
#endif

#define CLIENT_MANUFACTURER	"Zephyr"
#define CLIENT_FIRMWARE_VER	"1.0"
#define CLIENT_DEVICE_TYPE	"Zephyr OMA-LWM2M Client"
#define CLIENT_HW_VER		CONFIG_SOC

#define NUM_TEST_RESULTS	5

struct update_lwm2m_data {
	int failures;

	/* For test reporting */
	struct k_work tc_work;
	u8_t tc_results[NUM_TEST_RESULTS];
	u8_t tc_count;
};

static struct update_lwm2m_data update_data;
static bool tc_logging;

static char ep_name[LWM2M_DEVICE_ID_SIZE];

extern struct device *flash_dev;
static struct lwm2m_ctx app_ctx;

/*
 * TODO: Find a better way to handle update markers, and if possible
 * identify a common solution that could also be shared with hawkbit
 * or other fota-implementations (e.g. common file-system structure).
 */
struct update_counter {
	int current;
	int update;
};

typedef enum {
	COUNTER_CURRENT = 0,
	COUNTER_UPDATE,
} update_counter_t;

static struct k_delayed_work reboot_work;
static struct net_mgmt_event_callback cb;

#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
NET_PKT_TX_SLAB_DEFINE(lwm2m_tx_udp, 5);
NET_PKT_DATA_POOL_DEFINE(lwm2m_data_udp, 20);

static struct k_mem_slab *tx_udp_slab(void)
{
	return &lwm2m_tx_udp;
}

static struct net_buf_pool *data_udp_pool(void)
{
	return &lwm2m_data_udp;
}
#endif

static int lwm2m_update_counter_read(struct update_counter *update_counter)
{
	return flash_read(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			update_counter, sizeof(*update_counter));
}

static int lwm2m_update_counter_update(update_counter_t type, u32_t new_value)
{
	struct update_counter update_counter;
	int ret;

	flash_read(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			&update_counter, sizeof(update_counter));
	if (type == COUNTER_UPDATE) {
		update_counter.update = new_value;
	} else {
		update_counter.current = new_value;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_erase(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			  FLASH_AREA_APPLICATION_STATE_SIZE);
	flash_write_protection_set(flash_dev, true);
	if (ret) {
		return ret;
	}

	flash_write_protection_set(flash_dev, false);
	ret = flash_write(flash_dev, FLASH_AREA_APPLICATION_STATE_OFFSET,
			  &update_counter, sizeof(update_counter));
	flash_write_protection_set(flash_dev, true);

	return ret;
}

static void reboot(struct k_work *work)
{
	SYS_LOG_INF("Rebooting device");
	sys_reboot(0);
}

static int device_reboot_cb(u16_t obj_inst_id)
{
	SYS_LOG_INF("DEVICE: Reboot in progress");
	k_delayed_work_submit(&reboot_work, MSEC_PER_SEC);

	return 0;
}

static int firmware_update_cb(u16_t obj_inst_id)
{
	struct update_counter update_counter;
	int ret = 0;

	SYS_LOG_DBG("Executing firmware update");

	/* Bump update counter so it can be verified on the next reboot */
	ret = lwm2m_update_counter_read(&update_counter);
	if (ret) {
		SYS_LOG_ERR("Failed read update counter");
		goto cleanup;
	}
	SYS_LOG_INF("Update Counter: current %d, update %d",
			update_counter.current, update_counter.update);
	ret = lwm2m_update_counter_update(COUNTER_UPDATE,
			update_counter.current + 1);
	if (ret) {
		SYS_LOG_ERR("Failed to update the update counter: %d", ret);
		goto cleanup;
	}

	boot_trigger_ota();

	k_delayed_work_submit(&reboot_work, MSEC_PER_SEC);

	return 0;

cleanup:
	return ret;
}

static int firmware_block_received_cb(u16_t obj_inst_id,
				      u8_t *data, u16_t data_len,
				      bool last_block, size_t total_size)
{
	static int bytes_written;
	static u8_t percent_downloaded;
	static u32_t bytes_downloaded;
	u8_t downloaded;
	int ret = 0;

	if (total_size > FLASH_BANK_SIZE) {
		SYS_LOG_ERR("Artifact file size too big (%d)", total_size);
		return -EINVAL;
	}

	if (!data_len) {
		SYS_LOG_ERR("Data len is zero, nothing to write.");
		return -EINVAL;
	}

	/* Erase bank 1 before starting the write process */
	if (bytes_downloaded == 0) {
		SYS_LOG_INF("Download firmware started, erasing second bank");
		flash_write_protection_set(flash_dev, false);
		ret = flash_erase(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
					FLASH_BANK_SIZE);
		flash_write_protection_set(flash_dev, true);
		if (ret != 0) {
			SYS_LOG_ERR("Failed to erase flash bank 1");
			goto cleanup;
		}
	}

	bytes_downloaded += data_len;

	/* display a % downloaded, if it's different */
	if (total_size) {
		downloaded = bytes_downloaded * 100 / total_size;
	} else {
		/* Total size is empty when there is only one block */
		downloaded = 100;
	}

	if (downloaded > percent_downloaded) {
		percent_downloaded = downloaded;
		SYS_LOG_INF("%d%%", percent_downloaded);
	}

	ret = flash_block_write(flash_dev, FLASH_AREA_IMAGE_1_OFFSET,
				&bytes_written, data, data_len,
				last_block);
	if (ret < 0) {
		SYS_LOG_ERR("Failed to write flash block");
		goto cleanup;
	}

	if (!last_block) {
		/* Keep going */
		return ret;
	}

	if (total_size && (bytes_downloaded != total_size)) {
		SYS_LOG_ERR("Early last block, downloaded %d, expecting %d",
				bytes_downloaded, total_size);
		ret = -EIO;
	}

cleanup:
	bytes_written = 0;
	bytes_downloaded = 0;
	percent_downloaded = 0;

	return ret;
}

static int lwm2m_setup(void)
{
	const struct product_id_t *product_id = product_id_get();
	char device_serial_no[10];
	int ret;

	snprintf(device_serial_no, sizeof(device_serial_no), "%08x",
			product_id->number);
	/* Check if there is a valid device id stored in the device */
	ret = lwm2m_get_device_id(flash_dev, ep_name);
	if (ret) {
		SYS_LOG_ERR("Fail to read LWM2M Device ID");
	}
	if (ret || ep_name[LWM2M_DEVICE_ID_SIZE - 1] != '\0') {
		/* No UUID, use the serial number instead */
		SYS_LOG_WRN("LWM2M Device ID not set, using serial number");
		strncpy(ep_name, device_serial_no, LWM2M_DEVICE_ID_SIZE);
	}
	SYS_LOG_INF("LWM2M Endpoint Client Name: %s", ep_name);

	/* Device Object values and callbacks */
	lwm2m_engine_set_string("3/0/0", CLIENT_MANUFACTURER);
	lwm2m_engine_set_string("3/0/1", (char *) product_id->name);
	lwm2m_engine_set_string("3/0/2", device_serial_no);
	lwm2m_engine_set_string("3/0/3", CLIENT_FIRMWARE_VER);
	lwm2m_engine_register_exec_callback("3/0/4", device_reboot_cb);
	lwm2m_engine_set_string("3/0/17", CLIENT_DEVICE_TYPE);
	lwm2m_engine_set_string("3/0/18", CLIENT_HW_VER);
	lwm2m_engine_set_string("3/0/19", KERNEL_VERSION_STRING);
	lwm2m_engine_set_u32("3/0/21", (int) (FLASH_BANK_SIZE / 1024));

	/* Firmware Object callbacks */
	lwm2m_firmware_set_write_cb(firmware_block_received_cb);
	lwm2m_firmware_set_update_cb(firmware_update_cb);

	/* Reboot work, used when executing update */
	k_delayed_work_init(&reboot_work, reboot);

	return 0;
}

static void handle_test_result(struct update_lwm2m_data *data, u8_t result)
{
	if (!tc_logging || data->tc_count >= NUM_TEST_RESULTS) {
		return;
	}

	data->tc_results[data->tc_count++] = result;

	if (data->tc_count == NUM_TEST_RESULTS) {
		app_wq_submit(&data->tc_work);
	}
}

static void rd_client_event(struct lwm2m_ctx *client,
			    enum lwm2m_rd_client_event client_event)
{
	switch (client_event) {

	case LWM2M_RD_CLIENT_EVENT_NONE:
		/* do nothing */
		break;

	case LWM2M_RD_CLIENT_EVENT_BOOTSTRAP_FAILURE:
		if (tc_logging) {
			_TC_END_RESULT(TC_FAIL, "lwm2m_registration");
			TC_END_REPORT(TC_FAIL);
			tc_logging = false;
		}
		break;

	case LWM2M_RD_CLIENT_EVENT_BOOTSTRAP_COMPLETE:
		/* do nothing here and continue to registration */
		break;

	case LWM2M_RD_CLIENT_EVENT_REGISTRATION_FAILURE:
		if (tc_logging) {
			_TC_END_RESULT(TC_FAIL, "lwm2m_registration");
			TC_END_REPORT(TC_FAIL);
			tc_logging = false;
		}
		break;

	case LWM2M_RD_CLIENT_EVENT_REGISTRATION_COMPLETE:
		if (tc_logging) {
			_TC_END_RESULT(TC_PASS, "lwm2m_registration");
		}
		break;

	case LWM2M_RD_CLIENT_EVENT_REG_UPDATE_FAILURE:
		handle_test_result(&update_data, TC_FAIL);
		break;

	case LWM2M_RD_CLIENT_EVENT_REG_UPDATE_COMPLETE:
		handle_test_result(&update_data, TC_PASS);
		break;

	case LWM2M_RD_CLIENT_EVENT_DEREGISTER_FAILURE:
		SYS_LOG_DBG("Deregister failure!");
		/* TODO: handle deregister? */
		break;

	case LWM2M_RD_CLIENT_EVENT_DISCONNECT:
		SYS_LOG_DBG("Disconnected");
		/* TODO: handle disconnect? */
		break;

	}
}

static void event_iface_up(struct net_mgmt_event_callback *cb,
		u32_t mgmt_event, struct net_if *iface)
{
	int ret;

	if (!iface) {
		SYS_LOG_ERR("No network interface specified!");
		return;
	}

	memset(&app_ctx, 0x0, sizeof(app_ctx));
	app_ctx.net_init_timeout = WAIT_TIME;
	app_ctx.net_timeout = CONNECT_TIME;
#if defined(CONFIG_NET_CONTEXT_NET_PKT_POOL)
	app_ctx.tx_slab = tx_udp_slab;
	app_ctx.data_pool = data_udp_pool;
#endif

	/* small delay to finalize networking */
	k_sleep(K_SECONDS(2));
	TC_PRINT("LwM2M registration\n");

#if defined(CONFIG_NET_IPV6)
	ret = lwm2m_rd_client_start(&app_ctx, CONFIG_NET_APP_PEER_IPV6_ADDR,
				    CONFIG_LWM2M_PEER_PORT, ep_name,
				    rd_client_event);
#elif defined(CONFIG_NET_IPV4)
	ret = lwm2m_rd_client_start(&app_ctx, CONFIG_NET_APP_PEER_IPV4_ADDR,
				    CONFIG_LWM2M_PEER_PORT, ep_name,
				    rd_client_event);
#else
	SYS_LOG_ERR("LwM2M client requires IPv4 or IPv6.");
	return;
#endif
	if (ret < 0) {
		SYS_LOG_ERR("LWM2M RD client error (%d)", ret);
		return;
	}

	SYS_LOG_INF("setup complete.");
}

/* TODO: Make it generic, and if possible sharing with hawkbit via library */
static int lwm2m_image_init(void)
{
	int ret = 0;
	struct update_counter counter;
	u8_t boot_status;

	/* Update boot status and update counter */
	ret = lwm2m_update_counter_read(&counter);
	if (ret) {
		SYS_LOG_ERR("Failed read update counter");
		return ret;
	}
	SYS_LOG_INF("Update Counter: current %d, update %d",
			counter.current, counter.update);
	boot_status = boot_status_read();
	SYS_LOG_INF("Current boot status %x", boot_status);
	if (boot_status == BOOT_STATUS_ONGOING) {
		boot_status_update();
		SYS_LOG_INF("Updated boot status to %x", boot_status_read());
		ret = boot_erase_flash_bank(FLASH_AREA_IMAGE_1_OFFSET);
		if (ret) {
			SYS_LOG_ERR("Flash bank erase at offset %x: error %d",
					FLASH_AREA_IMAGE_1_OFFSET, ret);
			return ret;
		}
		SYS_LOG_DBG("Erased flash bank at offset %x",
				FLASH_AREA_IMAGE_1_OFFSET);
		if (counter.update != -1) {
			ret = lwm2m_update_counter_update(COUNTER_CURRENT,
						counter.update);
			if (ret) {
				SYS_LOG_ERR("Failed to update the update "
					    "counter: %d", ret);
				return ret;
			}
			ret = lwm2m_update_counter_read(&counter);
			if (ret) {
				SYS_LOG_ERR("Failed read update counter");
				return ret;
			}
			SYS_LOG_INF("Update Counter updated");
		}
	}

	/* Check if a firmware update status needs to be reported */
	if (counter.update != -1 &&
			counter.current == counter.update) {
		/* Successful update */
		SYS_LOG_INF("Firmware updated successfully");
		lwm2m_engine_set_u8("5/0/5", RESULT_SUCCESS);
	} else if (counter.update > counter.current) {
		/* Failed update */
		SYS_LOG_INF("Firmware failed to be updated");
		lwm2m_engine_set_u8("5/0/5", RESULT_UPDATE_FAILED);
	}

	return ret;
}

/*
 * This work handler prints the results for updating LwM2M registration.
 */
static void lwm2m_reg_update_result(struct k_work *work)
{
	struct update_lwm2m_data *data =
		CONTAINER_OF(work, struct update_lwm2m_data, tc_work);
	/*
	 * `result_name' is long enough for the function name, '_',
	 * two digits of test result, and '\0'. If NUM_TEST_RESULTS is
	 * 100 or more, space for more digits is needed.
	 */
	size_t result_len = strlen(__func__) + 1 + 2 + 1;
	char result_name[result_len];
	u8_t result, final_result = TC_PASS;
	size_t i;

	/* Ensure we have enough space to print the result name. */
	BUILD_ASSERT_MSG(NUM_TEST_RESULTS <= 99,
			 "result_len is too small to print test number");

	TC_PRINT("Update LwM2M registration\n");
	for (i = 0; i < data->tc_count; i++) {
		result = data->tc_results[i];
		snprintf(result_name, sizeof(result_name), "%s_%zu",
			 __func__, i);
		if (result == TC_FAIL) {
			final_result = TC_FAIL;
		}
		_TC_END_RESULT(result, result_name);
	}
	TC_END_REPORT(final_result);
	tc_logging = false;
}

int lwm2m_init(void)
{
	struct net_if *iface;
	int ret;

	TC_START("LwM2M tests");

	TC_PRINT("Initializing LWM2M Image\n");
	ret = lwm2m_image_init();
	if (ret < 0) {
		SYS_LOG_ERR("Failed to setup image properties (%d)", ret);
		_TC_END_RESULT(TC_FAIL, "lwm2m_image_init");
		TC_END_REPORT(TC_FAIL);
		return ret;
	}
	_TC_END_RESULT(TC_PASS, "lwm2m_image_init");

	TC_PRINT("Initializing LWM2M Engine\n");
	ret = lwm2m_setup();
	if (ret < 0) {
		SYS_LOG_ERR("Cannot setup LWM2M fields (%d)", ret);
		_TC_END_RESULT(TC_FAIL, "lwm2m_setup");
		TC_END_REPORT(TC_FAIL);
		return ret;
	}

	iface = net_if_get_default();
	if (!iface) {
		SYS_LOG_ERR("Cannot find default network interface!");
		_TC_END_RESULT(TC_FAIL, "lwm2m_setup");
		TC_END_REPORT(TC_FAIL);
		return -ENETDOWN;
	}
	_TC_END_RESULT(TC_PASS, "lwm2m_setup");

	/* initialize test case data */
	update_data.failures = 0;
	k_work_init(&update_data.tc_work, lwm2m_reg_update_result);
	update_data.tc_count = 0;
	tc_logging = true;

	/* Subscribe to NET_IF_UP if interface is not ready */
	if (!atomic_test_bit(iface->flags, NET_IF_UP)) {
		net_mgmt_init_event_callback(&cb, event_iface_up,
				NET_EVENT_IF_UP);
		net_mgmt_add_event_callback(&cb);
	} else {
		event_iface_up(NULL, NET_EVENT_IF_UP, iface);
	}

	return 0;
}
