// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/channel.h>
#include <lib/zx/interrupt.h>
#include <lib/zx/vmo.h>
#include <string.h>

#include "common.h"
#include "device.h"
// TODO(ZX-3927): Stop depending on the types in this file.
#include <lib/zx/status.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/syscalls/pci.h>

#define RPC_ENTRY zxlogf(DEBUG, "[%s] %s: entry", cfg_->addr(), __func__)

#define RPC_UNIMPLEMENTED \
  RPC_ENTRY;              \
  return RpcReply(ch, ZX_ERR_NOT_SUPPORTED)

namespace pci {

zx_status_t Device::DdkRxrpc(zx_handle_t channel) {
  if (channel == ZX_HANDLE_INVALID) {
    // A new connection has been made, there's nothing else to do.
    return ZX_OK;
  }

  // Clear the buffers. We only servce new requests after we've finished
  // previous messages, so we won't overwrite data here.
  memset(&request_, 0, sizeof(request_));
  memset(&response_, 0, sizeof(response_));

  uint32_t bytes_in;
  uint32_t handles_in;
  zx_handle_t handle;
  zx::unowned_channel ch(channel);
  zx_status_t st = ch->read(0, &request_, &handle, sizeof(request_), 1, &bytes_in, &handles_in);
  if (st != ZX_OK) {
    return ZX_ERR_INTERNAL;
  }

  if (bytes_in != sizeof(request_)) {
    return ZX_ERR_INTERNAL;
  }

  {
    fbl::AutoLock dev_lock(&dev_lock_);
    if (disabled_) {
      return RpcReply(ch, ZX_ERR_BAD_STATE);
    }
  }

  switch (request_.op) {
    case PCI_OP_CONFIG_READ:
      return RpcConfigRead(ch);
      break;
    case PCI_OP_CONFIG_WRITE:
      return RpcConfigWrite(ch);
      break;
    case PCI_OP_ENABLE_BUS_MASTER:
      return RpcEnableBusMaster(ch);
      break;
    case PCI_OP_GET_AUXDATA:
      return RpcGetAuxdata(ch);
      break;
    case PCI_OP_GET_BAR:
      return RpcGetBar(ch);
      break;
    case PCI_OP_GET_BTI:
      return RpcGetBti(ch);
      break;
    case PCI_OP_GET_DEVICE_INFO:
      return RpcGetDeviceInfo(ch);
      break;
    case PCI_OP_GET_NEXT_CAPABILITY:
      return RpcGetNextCapability(ch);
      break;
    case PCI_OP_MAP_INTERRUPT:
      return RpcMapInterrupt(ch);
      break;
    case PCI_OP_QUERY_IRQ_MODE:
      return RpcQueryIrqMode(ch);
      break;
    case PCI_OP_RESET_DEVICE:
      return RpcResetDevice(ch);
      break;
    case PCI_OP_SET_IRQ_MODE:
      return RpcSetIrqMode(ch);
      break;
    default:
      return RpcReply(ch, ZX_ERR_INVALID_ARGS);
  };

  return ZX_OK;
}

// Utility method to handle setting up the payload to return to the proxy and common
// error situations.
zx_status_t Device::RpcReply(const zx::unowned_channel& ch, zx_status_t st, zx_handle_t* handles,
                             const uint32_t handle_cnt) {
  response_.op = request_.op;
  response_.txid = request_.txid;
  response_.ret = st;
  return ch->write(0, &response_, sizeof(response_), handles, handle_cnt);
}

zx_status_t Device::RpcConfigRead(const zx::unowned_channel& ch) {
  response_.cfg.width = request_.cfg.width;
  response_.cfg.offset = request_.cfg.offset;

  if (request_.cfg.offset >= PCI_EXT_CONFIG_SIZE) {
    return RpcReply(ch, ZX_ERR_OUT_OF_RANGE);
  }

  switch (request_.cfg.width) {
    case 1:
      response_.cfg.value = cfg_->Read(PciReg8(request_.cfg.offset));
      break;
    case 2:
      response_.cfg.value = cfg_->Read(PciReg16(request_.cfg.offset));
      break;
    case 4:
      response_.cfg.value = cfg_->Read(PciReg32(request_.cfg.offset));
      break;
    default:
      return RpcReply(ch, ZX_ERR_INVALID_ARGS);
  }

  zxlogf(TRACE, "%s Read%u[%#x] = %#x", cfg_->addr(), request_.cfg.width * 8, request_.cfg.offset,
         response_.cfg.value);
  return RpcReply(ch, ZX_OK);
}

zx_status_t Device::RpcConfigWrite(const zx::unowned_channel& ch) {
  response_.cfg.width = request_.cfg.width;
  response_.cfg.offset = request_.cfg.offset;
  response_.cfg.value = request_.cfg.value;

  // Don't permit writes inside the config header.
  if (request_.cfg.offset < PCI_CONFIG_HDR_SIZE) {
    return RpcReply(ch, ZX_ERR_ACCESS_DENIED);
  }

  if (request_.cfg.offset >= PCI_EXT_CONFIG_SIZE) {
    return RpcReply(ch, ZX_ERR_OUT_OF_RANGE);
  }

  switch (request_.cfg.width) {
    case 1:
      cfg_->Write(PciReg8(request_.cfg.offset), static_cast<uint8_t>(request_.cfg.value));
      break;
    case 2:
      cfg_->Write(PciReg16(request_.cfg.offset), static_cast<uint16_t>(request_.cfg.value));
      break;
    case 4:
      cfg_->Write(PciReg32(request_.cfg.offset), request_.cfg.value);
      break;
    default:
      return RpcReply(ch, ZX_ERR_INVALID_ARGS);
  }

  zxlogf(TRACE, "[%s] Write%u[%#x] <- %#x", cfg_->addr(), request_.cfg.width * 8,
         request_.cfg.offset, request_.cfg.value);
  return RpcReply(ch, ZX_OK);
}

zx_status_t Device::RpcEnableBusMaster(const zx::unowned_channel& ch) {
  return RpcReply(ch, EnableBusMaster(request_.enable));
}

zx_status_t Device::RpcGetAuxdata(const zx::unowned_channel& ch) { RPC_UNIMPLEMENTED; }

zx_status_t Device::RpcGetBar(const zx::unowned_channel& ch) {
  fbl::AutoLock dev_lock(&dev_lock_);
  auto bar_id = request_.bar.id;
  if (bar_id >= bar_count_) {
    return RpcReply(ch, ZX_ERR_INVALID_ARGS);
  }

  // If this device supports MSIX then we need to deny access to the BARs it
  // uses.
  auto& msix = caps_.msix;
  if (msix && (msix->table_bar() == bar_id || msix->pba_bar() == bar_id)) {
    return RpcReply(ch, ZX_ERR_ACCESS_DENIED);
  }

  // Both unused BARs and BARs that are the second half of a 64 bit
  // BAR have a size of zero.
  auto& bar = bars_[bar_id];
  if (bar.size == 0) {
    return RpcReply(ch, ZX_ERR_NOT_FOUND);
  }

  zx_status_t st;
  zx_handle_t handle = ZX_HANDLE_INVALID;
  uint32_t handle_cnt = 0;
  response_.bar.id = bar_id;
  // MMIO Bars have an associated VMO for the driver to map, whereas
  // IO bars have a Resource corresponding to an IO range for the
  // driver to access. These are mutually exclusive, so only one
  // handle is ever needed.
  if (bar.is_mmio) {
    response_.bar.is_mmio = true;
    zx::vmo vmo;
    st = bar.allocation->CreateVmObject(&vmo);
    if (st == ZX_OK) {
      handle = vmo.release();
      handle_cnt++;
    } else {
      return RpcReply(ch, ZX_ERR_INTERNAL);
    }
  } else {
    zx::resource res;
    response_.bar.is_mmio = false;
    if (bar.allocation->resource().get() != ZX_HANDLE_INVALID) {
      st = bar.allocation->resource().duplicate(ZX_RIGHT_SAME_RIGHTS, &res);
      if (st != ZX_OK) {
        return RpcReply(ch, ZX_ERR_INTERNAL);
      }

      handle = res.release();
      handle_cnt++;
    }
    response_.bar.io_addr = static_cast<uint16_t>(bar.address);
    response_.bar.io_size = static_cast<uint16_t>(bar.size);
  }

  return RpcReply(ch, ZX_OK, &handle, handle_cnt);
}

zx_status_t Device::RpcGetBti(const zx::unowned_channel& ch) { RPC_UNIMPLEMENTED; }

zx_status_t Device::RpcGetDeviceInfo(const zx::unowned_channel& ch) {
  response_.info.vendor_id = vendor_id();
  response_.info.device_id = device_id();
  response_.info.base_class = class_id();
  response_.info.sub_class = subclass();
  response_.info.program_interface = prog_if();
  response_.info.revision_id = rev_id();
  response_.info.bus_id = bus_id();
  response_.info.dev_id = dev_id();
  response_.info.func_id = func_id();

  return RpcReply(ch, ZX_OK);
}

namespace {
template <class T, class L>
zx_status_t GetNextCapability(PciRpcMsg* req, PciRpcMsg* resp, const L* list) {
  resp->cap.id = req->cap.id;
  resp->cap.is_extended = req->cap.is_extended;
  resp->cap.is_first = req->cap.is_first;
  // Scan for the capability type requested, returning the first capability
  // found after we've seen the capability owning the previous offset.  We
  // can't scan entirely based on offset being >= than a given base because
  // capabilities pointers can point backwards in config space as long as the
  // structures are valid.
  zx_status_t st = ZX_ERR_NOT_FOUND;
  bool found_prev = (req->cap.is_first) ? true : false;
  T scan_offset = static_cast<T>(req->cap.offset);

  for (auto& cap : *list) {
    if (found_prev) {
      if (cap.id() == req->cap.id) {
        resp->cap.offset = cap.base();
        st = ZX_OK;
        break;
      }
    } else {
      if (cap.base() == scan_offset) {
        found_prev = true;
      }
    }
  }
  return st;
}

}  // namespace
zx_status_t Device::RpcGetNextCapability(const zx::unowned_channel& ch) {
  // Capabilities and Extended Capabilities only differ by what list they're in along with the
  // size of their entries. We can offload most of the work into a templated work function.
  zx_status_t st = ZX_ERR_NOT_FOUND;
  if (request_.cap.is_extended) {
    st = GetNextCapability<uint16_t, ExtCapabilityList>(&request_, &response_,
                                                        &capabilities().ext_list);
  } else {
    st = GetNextCapability<uint8_t, CapabilityList>(&request_, &response_, &capabilities().list);
  }
  return RpcReply(ch, st);
}

zx_status_t Device::RpcQueryIrqMode(const zx::unowned_channel& ch) {
  response_.irq.max_irqs = 0;
  zx_status_t st = QueryIrqMode(request_.irq.mode, &response_.irq.max_irqs);
  zxlogf(DEBUG, "[%s] QueryIrqMode { mode = %u, max_irqs = %u, status = %s }", cfg_->addr(),
         request_.irq.mode, response_.irq.max_irqs, zx_status_get_string(st));
  return RpcReply(ch, st);
}

zx_status_t Device::RpcSetIrqMode(const zx::unowned_channel& ch) {
  zx_status_t st = SetIrqMode(request_.irq.mode, request_.irq.requested_irqs);
  zxlogf(DEBUG, "[%s] SetIrqMode { mode = %u, requested_irqs = %u, status = %s }", cfg_->addr(),
         request_.irq.mode, request_.irq.requested_irqs, zx_status_get_string(st));
  return RpcReply(ch, st);
}

zx_status_t Device::RpcMapInterrupt(const zx::unowned_channel& ch) {
  fbl::AutoLock dev_lock(&dev_lock_);

  if (irqs_.mode == PCI_IRQ_MODE_DISABLED) {
    return RpcReply(ch, ZX_ERR_BAD_STATE);
  }

  if (irqs_.mode == PCI_IRQ_MODE_LEGACY || irqs_.mode == PCI_IRQ_MODE_MSI_X) {
    return RpcReply(ch, ZX_ERR_NOT_SUPPORTED);
  }

  zx::status<ddk::MmioView> view_res = cfg_->get_view();
  if (!view_res.is_ok()) {
    return RpcReply(ch, view_res.status_value());
  }

  zx_status_t status;
  zx_handle_t handle;
  size_t handle_cnt = 0;
  switch (irqs_.mode) {
    case PCI_IRQ_MODE_MSI:
      status = zx_msi_create(irqs_.msi_allocation.get(), /*options=*/0, request_.irq.which_irq,
                             view_res->get_vmo()->get(), view_res->get_offset() + caps_.msi->base(),
                             &handle);
  }

  if (status == ZX_OK) {
    handle_cnt++;
  }

  zxlogf(DEBUG, "[%s] MapInterrupt { irq = %u, status = %s }", cfg_->addr(), request_.irq.which_irq,
         zx_status_get_string(status));
  return RpcReply(ch, status, &handle, handle_cnt);
}

zx_status_t Device::RpcResetDevice(const zx::unowned_channel& ch) { RPC_UNIMPLEMENTED; }

}  // namespace pci
