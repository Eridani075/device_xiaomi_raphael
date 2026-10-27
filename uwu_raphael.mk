#
# SPDX-FileCopyrightText: 2021-2026 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from core 64-bit and telephony products
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# Inherit uwuAOSP common configuration
UWU_DEVICE_TYPE := phone
UWU_SUPPORTS_TELEPHONY := true
UWU_MAINTAINER := 0x70B4E93
$(call inherit-product, vendor/uwu/config/common.mk)

# Inherit raphael device configuration
$(call inherit-product, device/xiaomi/raphael/device.mk)

# Soong-only is intentionally not enabled: raphael is A-only and the OTA
# updater is still Make-only, so a Soong-only target_files package would lack
# OTA/bin/updater and otapackage would fail.
TARGET_BOOT_ANIMATION_RES := 1080
TARGET_BUILD_APERTURE_CAMERA := false
TARGET_HAS_UDFPS := true

PRODUCT_NAME := uwu_raphael
PRODUCT_DEVICE := raphael
PRODUCT_MANUFACTURER := Xiaomi
PRODUCT_BRAND := Xiaomi
PRODUCT_MODEL := Mi 9T Pro

PRODUCT_GMS_CLIENTID_BASE := android-xiaomi

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="raphael-user 11 RKQ1.200826.002 V12.5.2.0.RFKMIXM release-keys"
