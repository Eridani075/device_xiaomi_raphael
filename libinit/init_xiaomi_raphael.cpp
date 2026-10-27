/*
 * SPDX-FileCopyrightText: 2021-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "vendor_init.h"

#include "include/libinit_dalvik_heap.h"
#include "include/libinit_variant.h"

static const variant_info raphael_global_info = {
    .hwc_value = "GLOBAL",
    .sku_value = "",

    .brand = "Xiaomi",
    .device = "raphael",
    .marketname = "",
    .model = "Mi 9T Pro",
    .build_fingerprint = "",
    .nfc = true,
};

static const variant_info raphaelin_info = {
    .hwc_value = "INDIA",
    .sku_value = "",

    .brand = "Xiaomi",
    .device = "raphael",
    .marketname = "",
    .model = "Redmi K20 Pro",
    .build_fingerprint = "",
    .nfc = false,
};

static const variant_info raphael_info = {
    .hwc_value = "",
    .sku_value = "",

    .brand = "Xiaomi",
    .device = "raphael",
    .marketname = "",
    .model = "Redmi K20 Pro",
    .build_fingerprint = "",
    .nfc = true,
};

static const std::vector<variant_info> variants = {
    raphael_global_info,
    raphaelin_info,
    raphael_info,
};

void vendor_load_properties() {
    search_variant(variants);
    set_dalvik_heap();
}
