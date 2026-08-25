#pragma once

extern "C" {
#include "host/ble_hs.h"
}

static int bleGap(struct ble_gap_event *event, void *arg);
