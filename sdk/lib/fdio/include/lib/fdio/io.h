// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FDIO_INCLUDE_LIB_FDIO_IO_H_
#define LIB_FDIO_INCLUDE_LIB_FDIO_IO_H_

#include <lib/fdio/limits.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <unistd.h>  // for ssize_t
#include <zircon/compiler.h>
#include <zircon/types.h>

// flag on handle args in processargs
// instructing that this fd should be dup'd to 0/1/2
// and be used for all of stdio
#define FDIO_FLAG_USE_FOR_STDIO 0x8000

// events for fdio_wait_fd()
#define FDIO_EVT_READABLE POLLIN
#define FDIO_EVT_WRITABLE POLLOUT
#define FDIO_EVT_ERROR POLLERR
#define FDIO_EVT_PEER_CLOSED POLLRDHUP
#define FDIO_EVT_ALL (POLLIN | POLLOUT | POLLERR | POLLRDHUP)

__BEGIN_CDECLS

// wait until one or more events are pending
zx_status_t fdio_wait_fd(int fd, uint32_t events, uint32_t* pending, zx_time_t deadline);

// create a fd that works with wait APIs (epoll, select, etc.) from a handle
// and expected signals (signals_in/signals_out correspond to POLLIN/POLLOUT
// events respectively). The handle will be closed when the fd is closed, unless
// shared_handle is true.
int fdio_handle_fd(zx_handle_t h, zx_signals_t signals_in, zx_signals_t signals_out,
                   bool shared_handle);

// Creates a pipe. The first argument returns the file descriptor representing
// the pipe, and the second argument returns the handle of the socket used to
// communicate with the pipe.
//
// # Errors
//
// ZX_ERR_NO_MEMORY: Failed due to a lack of memory.
//
// ZX_ERR_NO_RESOURCES: Failed to bind to the file descriptor.
zx_status_t fdio_pipe_half(int* out_fd, zx_handle_t* out_handle);

// Get a read-only VMO containing the whole contents of the file.
// This function creates a clone of the underlying VMO when possible, falling
// back to eagerly reading the contents into a freshly-created VMO.
zx_status_t fdio_get_vmo_copy(int fd, zx_handle_t* out_vmo);

// Gets a read-only VMO containing a clone of the underlying VMO.
// This function will fail rather than copying the contents if it cannot clone.
zx_status_t fdio_get_vmo_clone(int fd, zx_handle_t* out_vmo);

// Get a read-only handle to the exact VMO used by the file system server to
// represent the file. This function fails if the server does not have an exact
// VMO representation of the file (e.g., if fdio_get_vmo would need to copy
// or clone data into a new VMO).
zx_status_t fdio_get_vmo_exact(int fd, zx_handle_t* out_vmo);

// Get a read + execute VMO containing a clone of the underlying VMO.
// This function will fail rather than copying the contents if it cannot clone.
zx_status_t fdio_get_vmo_exec(int fd, zx_handle_t* out_vmo);

__END_CDECLS

#endif  // LIB_FDIO_INCLUDE_LIB_FDIO_IO_H_
