#
# Copyright (C) 2021-2022 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from sm8150-common
include device/xiaomi/sm8150-common/BoardConfigCommon.mk

# Include MIUI Camera board settings if present
ifneq ($(wildcard device/xiaomi/miuicamera/BoardConfig.mk),)
include device/xiaomi/miuicamera/BoardConfig.mk
endif

DEVICE_PATH := device/xiaomi/raphael

# Assert
TARGET_OTA_ASSERT_DEVICE := raphael,raphaelin

# Display
TARGET_SCREEN_DENSITY := 440

# Filesystem
# The 6GiB retrofit super has no room for ext4 images of the SSI partitions, so
# every partition in the super group is erofs (lz4hc). The fstab that says so is
# rootdir/etc/fstab.raphael, selected by androidboot.fstab_suffix=raphael; the
# shared sm8150-common fstab stays untouched for the other sm8150 devices.
BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_KERNEL_CMDLINE := $(filter-out androidboot.fstab_suffix=qcom,$(BOARD_KERNEL_CMDLINE)) androidboot.fstab_suffix=raphael
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/rootdir/etc/fstab.raphael

# Kernel
BOARD_USES_SOONG_KERNEL := true
SOONG_KERNEL_MODULE := //device/xiaomi/raphael:kernel
TARGET_KERNEL_VERSION := 4.14
TARGET_NEEDS_DTBOIMAGE := true


# Properties
TARGET_VENDOR_PROP += $(DEVICE_PATH)/vendor.prop

# Inherit from the proprietary version
include vendor/xiaomi/raphael/BoardConfigVendor.mk
