
#pragma once

#include <zephyr/bluetooth/addr.h>
#include <zmk/behavior.h>

int zmk_split_bt_invoke_behavior(uint8_t source, struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event, bool state);

bool zmk_split_bt_central_peripheral_is_connected(uint8_t index);
bool zmk_split_bt_central_peripheral_is_bonded(uint8_t index);