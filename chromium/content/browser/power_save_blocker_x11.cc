// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/power_save_blocker_impl.h"

#include <X11/Xlib.h>
#include <X11/extensions/dpms.h>
// Xlib #defines Status, but we can't have that for some of our headers.
#ifdef Status
#undef Status
#endif

#include "base/basictypes.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/singleton.h"
#include "base/nix/xdg_util.h"
#include "base/synchronization/lock.h"
#include "content/public/browser/browser_thread.h"
#include "dbus/bus.h"
#include "dbus/message.h"
#include "dbus/object_path.h"
#include "dbus/object_proxy.h"
#include "ui/gfx/x/x11_types.h"

namespace {

enum DBusAPI {
  NO_API,           // Disable. No supported API available.
  GNOME_API,        // Use the GNOME API. (Supports more features.)
  FREEDESKTOP_API,  // Use the FreeDesktop API, for KDE4, KDE5, and XFCE.
};

// Inhibit flags defined in the org.gnome.SessionManager interface.
// Can be OR'd together and passed as argument to the Inhibit() method
// to specify which power management features we want to suspend.
enum GnomeAPIInhibitFlags {
  INHIBIT_LOGOUT            = 1,
  INHIBIT_SWITCH_USER       = 2,
  INHIBIT_SUSPEND_SESSION   = 4,
  INHIBIT_MARK_SESSION_IDLE = 8
};

const char kGnomeAPIServiceName[] = "org.gnome.SessionManager";
const char kGnomeAPIInterfaceName[] = "org.gnome.SessionManager";
const char kGnomeAPIObjectPath[] = "/org/gnome/SessionManager";

const char kFreeDesktopAPIServiceName[] = "org.freedesktop.PowerManagement";
const char kFreeDesktopAPIInterfaceName[] =
    "org.freedesktop.PowerManagement.Inhibit";
const char kFreeDesktopAPIObjectPath[] =
    "/org/freedesktop/PowerManagement/Inhibit";

}  // namespace

namespace content {

class PowerSaveBlockerImpl::Delegate
    : public base::RefCountedThreadSafe<PowerSaveBlockerImpl::Delegate> {
 public:
  // Picks an appropriate D-Bus API to use based on the desktop environment.
  Delegate(PowerSaveBlockerType type, const std::string& description);

  // Post a task to initialize the delegate on the UI thread, which will itself
  // then post a task to apply the power save block on the FILE thread.
  void Init();

  // Post a task to remove the power save block on the FILE thread, unless it
  // hasn't yet been applied, in which case we just prevent it from applying.
  void CleanUp();

 private:
  friend class base::RefCountedThreadSafe<Delegate>;
  ~Delegate() {}

  // Selects an appropriate D-Bus API to use for this object. Must be called on
  // the UI thread. Checks enqueue_apply_ once an API has been selected, and
  // enqueues a call back to ApplyBlock() if it is true. See the comments for
  // enqueue_apply_ below.
  void InitOnUIThread();

  // Apply or remove the power save block, respectively. These methods should be
  // called once each, on the same thread, per instance. They block waiting for
  // the action to complete (with a timeout); the thread must thus allow I/O.
  void ApplyBlock(DBusAPI api);
  void RemoveBlock(DBusAPI api);

  // Asynchronous callback functions for ApplyBlock and RemoveBlock.
  // Functions do not receive ownership of |response|.
  void ApplyBlockFinished(DBusAPI api, dbus::Response* response);
  void RemoveBlockFinished(dbus::Response* response);

  // If DPMS (the power saving system in X11) is not enabled, then we don't want
  // to try to disable power saving, since on some desktop environments that may
  // enable DPMS with very poor default settings (e.g. turning off the display
  // after only 1 second). Must be called on the UI thread.
  static bool DPMSEnabled();

  // Returns an appropriate D-Bus API to use based on the desktop environment.
  // Must be called on the UI thread, as it may call DPMSEnabled() above.
  static DBusAPI SelectAPI();

  const PowerSaveBlockerType type_;
  const std::string description_;

  // Initially, we post a message to the UI thread to select an API. When it
  // finishes, it will post a message to the FILE thread to perform the actual
  // application of the block, unless enqueue_apply_ is false. We set it to
  // false when we post that message, or when RemoveBlock() is called before
  // ApplyBlock() has run. Both api_ and enqueue_apply_ are guarded by lock_.
  DBusAPI api_;
  bool enqueue_apply_;
  base::Lock lock_;

  // Indicates that a D-Bus power save blocking request is in flight.
  bool block_inflight_;
  // Used to detect erronous redundant calls to RemoveBlock().
  bool unblock_inflight_;
  // Indicates that RemoveBlock() is called before ApplyBlock() has finished.
  // If it's true, then the RemoveBlock() call will be processed immediately
  // after ApplyBlock() has finished.
  bool enqueue_unblock_;

  scoped_refptr<dbus::Bus> bus_;

  // The cookie that identifies our inhibit request,
  // or 0 if there is no active inhibit request.
  uint32 inhibit_cookie_;

  DISALLOW_COPY_AND_ASSIGN(Delegate);
};

PowerSaveBlockerImpl::Delegate::Delegate(PowerSaveBlockerType type,
                                         const std::string& description)
    : type_(type),
      description_(description),
      api_(NO_API),
      enqueue_apply_(false),
      inhibit_cookie_(0) {
  // We're on the client's thread here, so we don't allocate the dbus::Bus
  // object yet. We'll do it later in ApplyBlock(), on the FILE thread.
}

void PowerSaveBlockerImpl::Delegate::Init() {
  base::AutoLock lock(lock_);
  DCHECK(!enqueue_apply_);
  enqueue_apply_ = true;
  block_inflight_ = false;
  unblock_inflight_ = false;
  enqueue_unblock_ = false;
  BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
                          base::Bind(&Delegate::InitOnUIThread, this));
}

void PowerSaveBlockerImpl::Delegate::CleanUp() {
  base::AutoLock lock(lock_);
  if (enqueue_apply_) {
    // If a call to ApplyBlock() has not yet been enqueued because we are still
    // initializing on the UI thread, then just cancel it. We don't need to
    // remove the block because we haven't even applied it yet.
    enqueue_apply_ = false;
  } else if (api_ != NO_API) {
    BrowserThread::PostTask(BrowserThread::FILE, FROM_HERE,
                            base::Bind(&Delegate::RemoveBlock, this, api_));
  }
}

void PowerSaveBlockerImpl::Delegate::InitOnUIThread() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  base::AutoLock lock(lock_);
  api_ = SelectAPI();
  if (enqueue_apply_ && api_ != NO_API) {
    // The thread we use here becomes the origin and D-Bus thread for the D-Bus
    // library, so we need to use the same thread above for RemoveBlock(). It
    // must be a thread that allows I/O operations, so we use the FILE thread.
    BrowserThread::PostTask(BrowserThread::FILE, FROM_HERE,
                            base::Bind(&Delegate::ApplyBlock, this, api_));
  }
  enqueue_apply_ = false;
}

void PowerSaveBlockerImpl::Delegate::ApplyBlock(DBusAPI api) {
  DCHECK_CURRENTLY_ON(BrowserThread::FILE);
  DCHECK(!bus_);  // ApplyBlock() should only be called once.
  DCHECK(!block_inflight_);

  dbus::Bus::Options options;
  options.bus_type = dbus::Bus::SESSION;
  options.connection_type = dbus::Bus::PRIVATE;
  bus_ = new dbus::Bus(options);

  scoped_refptr<dbus::ObjectProxy> object_proxy;
  scoped_ptr<dbus::MethodCall> method_call;
  scoped_ptr<dbus::MessageWriter> message_writer;

  switch (api) {
    case NO_API:
      NOTREACHED();  // We should never call this method with this value.
      return;
    case GNOME_API:
      object_proxy = bus_->GetObjectProxy(
          kGnomeAPIServiceName,
          dbus::ObjectPath(kGnomeAPIObjectPath));
      method_call.reset(
          new dbus::MethodCall(kGnomeAPIInterfaceName, "Inhibit"));
      message_writer.reset(new dbus::MessageWriter(method_call.get()));
      // The arguments of the method are:
      //     app_id:        The application identifier
      //     toplevel_xid:  The toplevel X window identifier
      //     reason:        The reason for the inhibit
      //     flags:         Flags that spefify what should be inhibited
      message_writer->AppendString(
          base::CommandLine::ForCurrentProcess()->GetProgram().value());
      message_writer->AppendUint32(0);  // should be toplevel_xid
      message_writer->AppendString(description_);
      {
        uint32 flags = 0;
        switch (type_) {
          case kPowerSaveBlockPreventDisplaySleep:
            flags |= INHIBIT_MARK_SESSION_IDLE;
            flags |= INHIBIT_SUSPEND_SESSION;
            break;
          case kPowerSaveBlockPreventAppSuspension:
            flags |= INHIBIT_SUSPEND_SESSION;
            break;
        }
        message_writer->AppendUint32(flags);
      }
      break;
    case FREEDESKTOP_API:
      object_proxy = bus_->GetObjectProxy(
          kFreeDesktopAPIServiceName,
          dbus::ObjectPath(kFreeDesktopAPIObjectPath));
      method_call.reset(
          new dbus::MethodCall(kFreeDesktopAPIInterfaceName, "Inhibit"));
      message_writer.reset(new dbus::MessageWriter(method_call.get()));
      // The arguments of the method are:
      //     app_id:        The application identifier
      //     reason:        The reason for the inhibit
      message_writer->AppendString(
          base::CommandLine::ForCurrentProcess()->GetProgram().value());
      message_writer->AppendString(description_);
      break;
  }

  block_inflight_ = true;
  object_proxy->CallMethod(
      method_call.get(), dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
      base::Bind(&PowerSaveBlockerImpl::Delegate::ApplyBlockFinished, this,
                 api));
}

void PowerSaveBlockerImpl::Delegate::ApplyBlockFinished(
    DBusAPI api,
    dbus::Response* response) {
  DCHECK_CURRENTLY_ON(BrowserThread::FILE);
  DCHECK(bus_);
  DCHECK(block_inflight_);
  block_inflight_ = false;

  if (response) {
    // The method returns an inhibit_cookie, used to uniquely identify
    // this request. It should be used as an argument to Uninhibit()
    // in order to remove the request.
    dbus::MessageReader message_reader(response);
    if (!message_reader.PopUint32(&inhibit_cookie_))
      LOG(ERROR) << "Invalid Inhibit() response: " << response->ToString();
  } else {
    LOG(ERROR) << "No response to Inhibit() request!";
  }

  if (enqueue_unblock_) {
    enqueue_unblock_ = false;
    // RemoveBlock() was called while the Inhibit operation was in flight,
    // so go ahead and remove the block now.
    BrowserThread::PostTask(BrowserThread::FILE, FROM_HERE,
                            base::Bind(&Delegate::RemoveBlock, this, api_));
  }
}

void PowerSaveBlockerImpl::Delegate::RemoveBlock(DBusAPI api) {
  DCHECK_CURRENTLY_ON(BrowserThread::FILE);
  DCHECK(bus_);  // RemoveBlock() should only be called once.
  DCHECK(!unblock_inflight_);

  if (block_inflight_) {
    DCHECK(!enqueue_unblock_);
    // Can't call RemoveBlock until ApplyBlock's async operation has
    // finished. Enqueue it for execution once ApplyBlock is done.
    enqueue_unblock_ = true;
    return;
  }

  scoped_refptr<dbus::ObjectProxy> object_proxy;
  scoped_ptr<dbus::MethodCall> method_call;

  switch (api) {
    case NO_API:
      NOTREACHED();  // We should never call this method with this value.
      return;
    case GNOME_API:
      object_proxy = bus_->GetObjectProxy(
          kGnomeAPIServiceName,
          dbus::ObjectPath(kGnomeAPIObjectPath));
      method_call.reset(
          new dbus::MethodCall(kGnomeAPIInterfaceName, "Uninhibit"));
      break;
    case FREEDESKTOP_API:
      object_proxy = bus_->GetObjectProxy(
          kFreeDesktopAPIServiceName,
          dbus::ObjectPath(kFreeDesktopAPIObjectPath));
      method_call.reset(
          new dbus::MethodCall(kFreeDesktopAPIInterfaceName, "UnInhibit"));
      break;
  }

  dbus::MessageWriter message_writer(method_call.get());
  message_writer.AppendUint32(inhibit_cookie_);
  unblock_inflight_ = true;
  object_proxy->CallMethod(
      method_call.get(), dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
      base::Bind(&PowerSaveBlockerImpl::Delegate::RemoveBlockFinished, this));
}

void PowerSaveBlockerImpl::Delegate::RemoveBlockFinished(
    dbus::Response* response) {
  DCHECK_CURRENTLY_ON(BrowserThread::FILE);
  DCHECK(bus_);
  unblock_inflight_ = false;

  if (!response)
    LOG(ERROR) << "No response to Uninhibit() request!";
  // We don't care about checking the result. We assume it works; we can't
  // really do anything about it anyway if it fails.
  inhibit_cookie_ = 0;

  bus_->ShutdownAndBlock();
  bus_ = NULL;
}

// static
bool PowerSaveBlockerImpl::Delegate::DPMSEnabled() {
  XDisplay* display = gfx::GetXDisplay();
  BOOL enabled = false;
  int dummy;
  if (DPMSQueryExtension(display, &dummy, &dummy) && DPMSCapable(display)) {
    CARD16 state;
    DPMSInfo(display, &state, &enabled);
  }
  return enabled;
}

// static
DBusAPI PowerSaveBlockerImpl::Delegate::SelectAPI() {
  scoped_ptr<base::Environment> env(base::Environment::Create());
  switch (base::nix::GetDesktopEnvironment(env.get())) {
    case base::nix::DESKTOP_ENVIRONMENT_GNOME:
    case base::nix::DESKTOP_ENVIRONMENT_UNITY:
      if (DPMSEnabled())
        return GNOME_API;
      break;
    case base::nix::DESKTOP_ENVIRONMENT_XFCE:
    case base::nix::DESKTOP_ENVIRONMENT_KDE4:
    case base::nix::DESKTOP_ENVIRONMENT_KDE5:
      if (DPMSEnabled())
        return FREEDESKTOP_API;
      break;
    case base::nix::DESKTOP_ENVIRONMENT_KDE3:
    case base::nix::DESKTOP_ENVIRONMENT_OTHER:
      // Not supported.
      break;
  }
  return NO_API;
}

PowerSaveBlockerImpl::PowerSaveBlockerImpl(PowerSaveBlockerType type,
                                           Reason reason,
                                           const std::string& description)
    : delegate_(new Delegate(type, description)) {
  delegate_->Init();
}

PowerSaveBlockerImpl::~PowerSaveBlockerImpl() {
  delegate_->CleanUp();
}

}  // namespace content
