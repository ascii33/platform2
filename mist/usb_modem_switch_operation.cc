// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mist/usb_modem_switch_operation.h"

#include <vector>

#include <base/bind.h>
#include <base/string_number_conversions.h>
#include <base/stringprintf.h>

#include "mist/context.h"
#include "mist/event_dispatcher.h"
#include "mist/proto_bindings/usb_modem_info.pb.h"
#include "mist/usb_bulk_transfer.h"
#include "mist/usb_config_descriptor.h"
#include "mist/usb_constants.h"
#include "mist/usb_device.h"
#include "mist/usb_device_descriptor.h"
#include "mist/usb_device_event_notifier.h"
#include "mist/usb_endpoint_descriptor.h"
#include "mist/usb_interface.h"
#include "mist/usb_interface_descriptor.h"
#include "mist/usb_manager.h"
#include "mist/usb_modem_switch_context.h"

using base::Bind;
using base::CancelableClosure;
using base::StringPrintf;
using base::Unretained;
using std::string;
using std::vector;

namespace mist {

namespace {

const int kDefaultUsbInterfaceIndex = 0;
const int kDefaultUsbInterfaceAlternateSettingIndex = 0;

// TODO(benchan): To be conservative, use large timeout values for now. Add UMA
// metrics to determine appropriate timeout values.
const int64 kReconnectTimeoutMilliseconds = 15000;
const int64 kUsbMessageTransferTimeoutMilliseconds = 8000;

}  // namespace

// TODO(benchan): Add unit tests for UsbModemSwitchOperation.

UsbModemSwitchOperation::UsbModemSwitchOperation(
    Context* context,
    UsbModemSwitchContext* switch_context)
    : context_(context),
      switch_context_(switch_context),
      interface_claimed_(false),
      interface_number_(0),
      endpoint_address_(0) {
  CHECK(context_);
  CHECK(switch_context_);
  CHECK(!switch_context_->sys_path().empty());
  CHECK(switch_context_->modem_info());
}

UsbModemSwitchOperation::~UsbModemSwitchOperation() {
  pending_task_.Cancel();
  reconnect_timeout_callback_.Cancel();
  CloseDevice();
}

void UsbModemSwitchOperation::Start(
    const CompletionCallback& completion_callback) {
  CHECK(!completion_callback.is_null());

  completion_callback_ = completion_callback;
  VLOG(1) << "Start modem switch operation for device '"
          << switch_context_->sys_path() << "'.";

  // Defer the execution of the first task as multiple UsbModemSwitchOperation
  // objects may be created and started in a tight loop.
  ScheduleTask(
      &UsbModemSwitchOperation::OpenDeviceAndClaimMassStorageInterface);
}

void UsbModemSwitchOperation::ScheduleTask(Task task) {
  pending_task_.Reset(Bind(task, Unretained(this)));
  context_->event_dispatcher()->PostTask(pending_task_.callback());
}

void UsbModemSwitchOperation::Complete(bool success) {
  CHECK(!completion_callback_.is_null());

  if (!success) {
    LOG(ERROR) << "Could not switch device '" << switch_context_->sys_path()
               << "' into the modem mode.";
  }

  pending_task_.Cancel();
  reconnect_timeout_callback_.Cancel();
  context_->usb_device_event_notifier()->RemoveObserver(this);

  // Defer the execution of the completion callback for two reasons:
  // 1. To prevent a task in this switch operation from occupying the message
  //    loop for too long as Complete() can be called from one of the tasks.
  // 2. The completion callback may delete this object, so this object should
  //    not be accessed after this method returns.
  context_->event_dispatcher()->PostTask(
      Bind(completion_callback_, Unretained(this), success));
}

void UsbModemSwitchOperation::CloseDevice() {
  if (!device_)
    return;

  if (interface_claimed_) {
    if (!device_->ReleaseInterface(interface_number_) &&
        // UsbDevice::ReleaseInterface may return UsbError::kErrorNoDevice
        // as the original device may no longer exist after switching to the
        // modem mode. Do not report such an error.
        device_->error().type() != UsbError::kErrorNoDevice) {
      LOG(ERROR) << StringPrintf("Could not release interface %u: %s",
                                 interface_number_,
                                 device_->error().ToString());
    }
    interface_claimed_ = false;
  }

  device_.reset();
}

void UsbModemSwitchOperation::OpenDeviceAndClaimMassStorageInterface() {
  CHECK(!interface_claimed_);

  device_.reset(
      context_->usb_manager()->GetDevice(switch_context_->bus_number(),
                                         switch_context_->device_address(),
                                         switch_context_->vendor_id(),
                                         switch_context_->product_id()));
  if (!device_) {
    LOG(ERROR) << StringPrintf("Could not find USB device '%s' "
                               "(Bus %03d Address %03d ID %04x:%04x).",
                               switch_context_->sys_path().c_str(),
                               switch_context_->bus_number(),
                               switch_context_->device_address(),
                               switch_context_->vendor_id(),
                               switch_context_->product_id());
    Complete(false);
    return;
  }

  if (!device_->Open()) {
    LOG(ERROR) << "Could not open device '" << switch_context_->sys_path()
               << "'.";
    Complete(false);
    return;
  }

  scoped_ptr<UsbConfigDescriptor> config_descriptor(
      device_->GetActiveConfigDescriptor());
  if (!config_descriptor) {
    LOG(ERROR) << "Could not get active configuration descriptor: "
               << device_->error();
    Complete(false);
    return;
  }
  VLOG(2) << *config_descriptor;

  scoped_ptr<UsbInterface> interface(
      config_descriptor->GetInterface(kDefaultUsbInterfaceIndex));
  if (!interface) {
    LOG(ERROR) << "Could not get interface 0.";
    Complete(false);
    return;
  }

  scoped_ptr<UsbInterfaceDescriptor> interface_descriptor(
      interface->GetAlternateSetting(
          kDefaultUsbInterfaceAlternateSettingIndex));
  if (!interface_descriptor) {
    LOG(ERROR) << "Could not get interface alternate setting 0.";
    Complete(false);
    return;
  }
  VLOG(2) << *interface_descriptor;

  if (interface_descriptor->GetInterfaceClass() != kUsbClassMassStorage) {
    LOG(ERROR) << "Device is not currently in mass storage mode.";
    Complete(false);
    return;
  }

  scoped_ptr<UsbEndpointDescriptor> endpoint_descriptor(
      interface_descriptor->GetEndpointDescriptorByTransferTypeAndDirection(
          kUsbTransferTypeBulk, kUsbDirectionOut));
  if (!endpoint_descriptor) {
    LOG(ERROR) << "Could not find an output bulk endpoint.";
    Complete(false);
    return;
  }
  VLOG(2) << *endpoint_descriptor;

  interface_number_ = interface_descriptor->GetInterfaceNumber();
  endpoint_address_ = endpoint_descriptor->GetEndpointAddress();

  if (!device_->DetachKernelDriver(interface_number_) &&
      // UsbDevice::DetachKernelDriver returns UsbError::kErrorNotFound when
      // there is no driver attached to the device.
      device_->error().type() != UsbError::kErrorNotFound) {
    LOG(ERROR) << StringPrintf(
        "Could not detach kernel driver from interface %u: %s",
        interface_number_, device_->error().ToString());
    Complete(false);
    return;
  }

  if (!device_->ClaimInterface(interface_number_)) {
    LOG(ERROR) << StringPrintf("Could not claim interface %u: %s",
                               interface_number_, device_->error().ToString());
    Complete(false);
    return;
  }

  interface_claimed_ = true;
  ScheduleTask(&UsbModemSwitchOperation::SendMessageToMassStorageEndpoint);
}

void UsbModemSwitchOperation::SendMessageToMassStorageEndpoint() {
  const UsbModemInfo* modem_info = switch_context_->modem_info();
  // TODO(benchan): Remove this check when we support some modem that does not
  // require a special USB message for the switch operation.
  CHECK_GT(modem_info->usb_message_size(), 0);

  context_->usb_device_event_notifier()->AddObserver(this);

  // TODO(benchan): Support sending multiple special USB messages.
  vector<uint8> bytes;
  if (!base::HexStringToBytes(modem_info->usb_message(0), &bytes)) {
    LOG(ERROR) << "Invalid USB message: " << modem_info->usb_message(0);
    Complete(false);
    return;
  }

  if (!device_->ClearHalt(endpoint_address_)) {
     LOG(ERROR) << StringPrintf(
        "Could not clear halt condition for endpoint %u: %s",
        endpoint_address_, device_->error().ToString());
    Complete(false);
    return;
  }

  scoped_ptr<UsbBulkTransfer> bulk_transfer(new UsbBulkTransfer());
  if (!bulk_transfer->Initialize(*device_,
                                 endpoint_address_,
                                 bytes.size(),
                                 kUsbMessageTransferTimeoutMilliseconds)) {
    LOG(ERROR) << "Could not create USB bulk transfer: "
               << bulk_transfer->error();
    Complete(false);
    return;
  }
  memcpy(bulk_transfer->buffer(), &bytes[0], bytes.size());

  if (!bulk_transfer->Submit(
          Bind(&UsbModemSwitchOperation::OnUsbMessageTransferred,
               Unretained(this)))) {
    LOG(ERROR) << "Could not submit USB bulk transfer: "
               << bulk_transfer->error();
    Complete(false);
    return;
  }

  bulk_transfer_ = bulk_transfer.Pass();
}

void UsbModemSwitchOperation::OnUsbMessageTransferred(UsbTransfer* transfer) {
  CHECK_EQ(bulk_transfer_.get(), transfer);

  VLOG(1) << "USB transfer completed: " << *transfer;
  bool succeeded = (transfer->GetStatus() == kUsbTransferStatusCompleted) &&
                   (transfer->GetActualLength() == transfer->GetLength());
  bulk_transfer_.reset();

  if (!succeeded) {
    LOG(ERROR) << "Could not successfully transfer USB message.";
    Complete(false);
    return;
  }

  LOG(INFO) << "Successfully transferred USB message.";

  pending_task_.Cancel();
  reconnect_timeout_callback_.Reset(
      Bind(&UsbModemSwitchOperation::OnReconnectTimeout, Unretained(this)));
  context_->event_dispatcher()->PostDelayedTask(
      reconnect_timeout_callback_.callback(),
      base::TimeDelta::FromMilliseconds(kReconnectTimeoutMilliseconds));
}

void UsbModemSwitchOperation::OnReconnectTimeout() {
  LOG(ERROR) << "Timed out waiting for the device to reconnect.";
  Complete(false);
}

void UsbModemSwitchOperation::OnUsbDeviceAdded(const string& sys_path,
                                               uint8 bus_number,
                                               uint8 device_address,
                                               uint16 vendor_id,
                                               uint16 product_id) {
  if (sys_path != switch_context_->sys_path())
    return;

  const UsbModemInfo* modem_info = switch_context_->modem_info();
  if (modem_info->final_usb_id_size() == 0) {
    VLOG(1) << "No final USB identifiers are specified. Assuming device '"
            << switch_context_->sys_path()
            << "' has been switched to the modem mode.";
    Complete(true);
    return;
  }

  for (int i = 0; i < modem_info->final_usb_id_size(); ++i) {
    const UsbId& final_usb_id = modem_info->final_usb_id(i);
    if (vendor_id == final_usb_id.vendor_id() &&
        product_id == final_usb_id.product_id()) {
      const UsbId& initial_usb_id = modem_info->initial_usb_id();
      LOG(INFO) << StringPrintf(
          "Successfully switched device '%s' from %04x:%04x to %04x:%04x.",
          switch_context_->sys_path().c_str(),
          initial_usb_id.vendor_id(),
          initial_usb_id.product_id(),
          final_usb_id.vendor_id(),
          final_usb_id.product_id());
      Complete(true);
      return;
    }
  }
}

void UsbModemSwitchOperation::OnUsbDeviceRemoved(const string& sys_path) {
  if (sys_path == switch_context_->sys_path()) {
    VLOG(1) << "Device '" << switch_context_->sys_path()
            << "' has been removed and is switching to the modem mode.";
    // TODO(benchan): Investigate if the device will always be removed from
    // the bus before it reconnects. If so, add a check.
  }
}

}  // namespace mist
