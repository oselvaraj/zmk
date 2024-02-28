#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include "zmk/mouse.h"
#include "zmk/trackpad.h"
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/sensor_event.h>
#include <zmk/events/endpoint_changed.h>
#include "drivers/sensor/gen4.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

const struct device *trackpad = DEVICE_DT_GET(DT_INST(0, cirque_gen4));

static zmk_trackpad_finger_contacts_t present_contacts = 0;
static zmk_trackpad_finger_contacts_t contacts_to_send = 0;
static zmk_trackpad_finger_contacts_t received_contacts = 0;

static uint8_t btns;
static uint16_t scantime;

static bool trackpad_enabled;

static bool mousemode;
static bool surface_mode;
static bool button_mode;

static int8_t xDelta, yDelta, scrollDelta;

static struct zmk_ptp_finger fingers[5];
static const struct zmk_ptp_finger empty_finger = {0};

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_WORK_QUEUE_DEDICATED)
K_THREAD_STACK_DEFINE(trackpad_work_stack_area, CONFIG_ZMK_TRACKPAD_DEDICATED_THREAD_STACK_SIZE);
static struct k_work_q trackpad_work_q;
#endif

struct k_work_q *zmk_trackpad_work_q() {
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_WORK_QUEUE_DEDICATED)
    return &trackpad_work_q;
#else
    return &k_sys_work_q;
#endif
}

static void zmk_trackpad_tick(struct k_work *work) {
    if (mousemode) {
        // LOG_DBG("Mouse sender running");
        zmk_hid_mouse_set(btns, xDelta, yDelta, scrollDelta);
        zmk_endpoints_send_mouse_report();
    } else if (surface_mode) {

        // LOG_DBG("Trackpad sender running");
        LOG_DBG("total contacts: %d, received contacts: %d, bitmap contacts %d", present_contacts,
                received_contacts, contacts_to_send);

        zmk_hid_ptp_set((contacts_to_send & BIT(0)) ? fingers[0] : empty_finger,
                        (contacts_to_send & BIT(1)) ? fingers[1] : empty_finger,
                        (contacts_to_send & BIT(2)) ? fingers[2] : empty_finger,
                        (contacts_to_send & BIT(3)) ? fingers[3] : empty_finger,
                        (contacts_to_send & BIT(4)) ? fingers[4] : empty_finger, present_contacts,
                        scantime, button_mode ? btns : 0);
        zmk_endpoints_send_ptp_report();
        contacts_to_send = 0;
        received_contacts = 0;
        return;
    } else if (!surface_mode) {
        // report buttons only
        /// LOG_DBG("Trackpad button thing trigd");
        zmk_hid_ptp_set(empty_finger, empty_finger, empty_finger, empty_finger, empty_finger, 0,
                        scantime, button_mode ? btns : 0);
        zmk_endpoints_send_ptp_report();
    }
}

K_WORK_DEFINE(trackpad_work, zmk_trackpad_tick);

static void handle_trackpad_ptp(const struct device *dev, const struct sensor_trigger *trig) {
    int ret = sensor_sample_fetch(dev);
    if (ret < 0) {
        LOG_ERR("fetch: %d", ret);
        return;
    }
    LOG_DBG("Trackpad handler trigd %d", 0);

    struct sensor_value contacts, confidence_tip, id, x, y, buttons, scan_time;
    sensor_channel_get(dev, SENSOR_CHAN_CONTACTS, &contacts);
    sensor_channel_get(dev, SENSOR_CHAN_BUTTONS, &buttons);
    sensor_channel_get(dev, SENSOR_CHAN_SCAN_TIME, &scan_time);
    // expects bitmap format
    present_contacts = contacts.val1 ? contacts.val1 : present_contacts;
    // Buttons and scan time
    btns = buttons.val1;
    scantime = scan_time.val1;
    // released Fingers
    sensor_channel_get(dev, SENSOR_CHAN_X, &x);
    sensor_channel_get(dev, SENSOR_CHAN_Y, &y);
    sensor_channel_get(dev, SENSOR_CHAN_CONFIDENCE_TIP, &confidence_tip);
    sensor_channel_get(dev, SENSOR_CHAN_FINGER, &id);
    // If finger has changed
    fingers[id.val1].confidence_tip = confidence_tip.val1;
    fingers[id.val1].contact_id = id.val1;
    fingers[id.val1].x = x.val1;
    fingers[id.val1].y = y.val1;
    contacts_to_send |= BIT(id.val1);
    received_contacts++;

    LOG_DBG("total contacts: %d, received contacts: %d", present_contacts, received_contacts);

    if (present_contacts == received_contacts)
        k_work_submit_to_queue(zmk_trackpad_work_q(), &trackpad_work);

    ZMK_EVENT_RAISE(new_zmk_sensor_event(
        (struct zmk_sensor_event){.sensor_index = 0,
                                  .channel_data_size = 1,
                                  .channel_data = {(struct zmk_sensor_channel_data){
                                      .value = buttons, .channel = SENSOR_CHAN_BUTTONS}},
                                  .timestamp = k_uptime_get()}));
}

static void handle_mouse_mode(const struct device *dev, const struct sensor_trigger *trig) {
    LOG_DBG("Trackpad handler trigd in mouse mode %d", 0);
    int ret = sensor_sample_fetch(dev);
    if (ret < 0) {
        LOG_ERR("fetch: %d", ret);
        return;
    }

    struct sensor_value x, y, buttons, wheel;
    sensor_channel_get(dev, SENSOR_CHAN_XDELTA, &x);
    sensor_channel_get(dev, SENSOR_CHAN_YDELTA, &y);
    sensor_channel_get(dev, SENSOR_CHAN_BUTTONS, &buttons);
    sensor_channel_get(dev, SENSOR_CHAN_WHEEL, &wheel);

    btns = buttons.val1;
    xDelta = x.val1;
    yDelta = y.val1;
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_REVERSE_SCROLL)
    scrollDelta = -wheel.val1;
#else
    scrollDelta = wheel.val1;
#endif
    k_work_submit_to_queue(zmk_trackpad_work_q(), &trackpad_work);

    ZMK_EVENT_RAISE(new_zmk_sensor_event(
        (struct zmk_sensor_event){.sensor_index = 0,
                                  .channel_data_size = 1,
                                  .channel_data = {(struct zmk_sensor_channel_data){
                                      .value = buttons, .channel = SENSOR_CHAN_BUTTONS}},
                                  .timestamp = k_uptime_get()}));
}

void zmk_trackpad_set_enabled(bool enabled) {
    if (trackpad_enabled == enabled)
        return;
    struct sensor_trigger trigger = {
        .type = SENSOR_TRIG_DATA_READY,
        .chan = SENSOR_CHAN_ALL,
    };
    trackpad_enabled = enabled;
    if (trackpad_enabled) {
        // Activate everything
        if (mousemode) {
            if (sensor_trigger_set(trackpad, &trigger, handle_mouse_mode) < 0) {
                LOG_ERR("can't set trigger mouse mode");
            };
        } else {
            if (sensor_trigger_set(trackpad, &trigger, handle_trackpad_ptp) < 0) {
                LOG_ERR("can't set trigger");
            };
        }
    } else {
        // Clear reports, stop trigger
        if (mousemode) {
            zmk_hid_mouse_clear();
            zmk_endpoints_send_mouse_report();
        } else {
            zmk_hid_ptp_set(empty_finger, empty_finger, empty_finger, empty_finger, empty_finger, 0,
                            scantime + 1, 0);
            zmk_endpoints_send_ptp_report();
        }
        if (sensor_trigger_set(trackpad, &trigger, NULL) < 0) {
            LOG_ERR("can't unset trigger");
        };
    }
}

bool zmk_trackpad_get_enabled() { return trackpad_enabled; }

void zmk_trackpad_selective_set(uint8_t selective) {
    surface_mode = selective & BIT(0);
    button_mode = selective & BIT(1);
    LOG_DBG("Surface: %d, Button %d", surface_mode, button_mode);
}

void zmk_trackpad_set_mouse_mode(bool mouse_mode) {
    struct sensor_trigger trigger = {
        .type = SENSOR_TRIG_DATA_READY,
        .chan = SENSOR_CHAN_ALL,
    };
    struct sensor_value attr;
    mousemode = mouse_mode;
    attr.val1 = mousemode;

    sensor_attr_set(trackpad, SENSOR_CHAN_ALL, SENSOR_ATTR_CONFIGURATION, &attr);
    if (mousemode) {

        zmk_hid_ptp_set(empty_finger, empty_finger, empty_finger, empty_finger, empty_finger, 0,
                        scantime + 1, 0);
        zmk_endpoints_send_ptp_report();

        if (sensor_trigger_set(trackpad, &trigger, handle_mouse_mode) < 0) {
            LOG_ERR("can't set trigger mouse mode");
        };
    } else {
        zmk_hid_mouse_clear();
        zmk_endpoints_send_mouse_report();
        if (sensor_trigger_set(trackpad, &trigger, handle_trackpad_ptp) < 0) {
            LOG_ERR("can't set trigger");
        };
    }
}

static int trackpad_init() {

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_WORK_QUEUE_DEDICATED)
    k_work_queue_start(&trackpad_work_q, trackpad_work_stack_area,
                       K_THREAD_STACK_SIZEOF(trackpad_work_stack_area),
                       CONFIG_ZMK_TRACKPAD_DEDICATED_THREAD_PRIORITY, NULL);
#endif
    button_mode = true;
    surface_mode = true;
    zmk_trackpad_set_mouse_mode(true);
    return 0;
}

static int trackpad_event_listener(const zmk_event_t *eh) {
    zmk_trackpad_set_mouse_mode(true);
    return 0;
}

static ZMK_LISTENER(trackpad, trackpad_event_listener);
static ZMK_SUBSCRIPTION(trackpad, zmk_endpoint_changed);

SYS_INIT(trackpad_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);