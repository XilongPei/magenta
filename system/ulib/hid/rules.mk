# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := userlib

MODULE_SRCS += \
    $(LOCAL_DIR)/acer12.c \
    $(LOCAL_DIR)/hid.c \
    $(LOCAL_DIR)/keymaps.c \

MODULE_EXPORT := so
MODULE_SO_NAME := hid

MODULE_LIBS := \
    ulib/magenta \
    ulib/musl

include make/module.mk
