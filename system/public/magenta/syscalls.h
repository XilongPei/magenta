// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <magenta/internal.h>
#include <magenta/types.h>
#include <magenta/syscalls/types.h>

#include <magenta/syscalls/pci.h>
#include <magenta/syscalls/resource.h>

__BEGIN_CDECLS

#include <magenta/gen-syscalls.h>

// Accessors for state provided by the language runtime (eg. libc)

#define mx_process_self _mx_process_self
static inline mx_handle_t _mx_process_self(void) {
    return __magenta_process_self;
}

#define mx_vmar_root_self _mx_vmar_root_self
static inline mx_handle_t _mx_vmar_root_self(void) {
    return __magenta_vmar_root_self;
}

#define mx_job_default _mx_job_default
static inline mx_handle_t _mx_job_default(void) {
    return __magenta_job_default;
}

// Compatibility Wrappers for Deprecated Syscalls

mx_status_t _mx_handle_wait_many(mx_wait_item_t* i, uint32_t c, mx_time_t t)
__attribute__((deprecated("use _mx_object_wait_many() instead.")));
mx_status_t mx_handle_wait_many(mx_wait_item_t* i, uint32_t c, mx_time_t t)
__attribute__((deprecated("use mx_object_wait_many() instead.")));

mx_status_t _mx_handle_wait_one(mx_handle_t h, mx_signals_t s, mx_time_t t, mx_signals_t* o)
__attribute__((deprecated("use _mx_object_wait_one() instead.")));
mx_status_t mx_handle_wait_one(mx_handle_t h, mx_signals_t s, mx_time_t t, mx_signals_t* o)
__attribute__((deprecated("use mx_object_wait_one() instead.")));

__END_CDECLS
