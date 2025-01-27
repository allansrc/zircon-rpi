// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_SYSTEM_INSTANCE_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_SYSTEM_INSTANCE_H_

#include <fuchsia/boot/c/fidl.h>
#include <fuchsia/ldsvc/llcpp/fidl.h>
#include <lib/fdio/namespace.h>
#include <lib/zx/vmo.h>

#include "coordinator.h"
#include "fdio.h"
#include "fuchsia/boot/llcpp/fidl.h"

constexpr char kItemsPath[] = "/svc/" fuchsia_boot_Items_Name;

zx_status_t wait_for_file(const char* path, zx::time deadline);

class SystemInstance : public FsProvider {
 public:
  struct ServiceStarterArgs {
    SystemInstance* instance;
    Coordinator* coordinator;
  };

  SystemInstance();

  // Implementation required to implement FsProvider
  zx::channel CloneFs(const char* path) override;

  // The heart of the public API, in the order that things get called during
  // startup.
  zx_status_t CreateDriverHostJob(const zx::job& root_job, zx::job* driver_host_job_out);
  zx_status_t CreateSvcJob(const zx::job& root_job);
  zx_status_t MaybeCreateShellJob(const zx::job& root_job,
                                  llcpp::fuchsia::boot::Arguments::SyncClient& boot_args);
  zx_status_t PrepareChannels();

  zx_status_t StartSvchost(const zx::job& root_job, const zx::channel& root_dir,
                           bool require_system, Coordinator* coordinator);
  zx_status_t ReuseExistingSvchost();

  void devmgr_vfs_init();

  void start_console_shell(llcpp::fuchsia::boot::Arguments::SyncClient& boot_args);
  int ConsoleStarter(llcpp::fuchsia::boot::Arguments::SyncClient* boot_args);

  // Thread entry point
  static int service_starter(void* arg);
  int ServiceStarter(Coordinator* coordinator);
  int WaitForSystemAvailable(Coordinator* coordinator);

  // TODO(ZX-4860): DEPRECATED. Do not add new dependencies on the fshost loader service!
  zx_status_t clone_fshost_ldsvc(zx::channel* loader);

 protected:
  DevmgrLauncher& launcher() { return launcher_; }
  zx::job& svc_job() { return svc_job_; }
  zx::job& shell_job() { return shell_job_; }

 private:
  // Private helper functions.
  void do_autorun(const char* name, const char* cmd, const zx::resource& root_resource);

  // The handle used to transmit messages to miscsvc.
  zx::channel miscsvc_client_;

  // The handle used by miscsvc to serve incoming requests.
  zx::channel miscsvc_server_;

  // The handle used to transmit messages to device_name_provider.
  zx::channel device_name_provider_client_;

  // The handle used by device_name_provider to serve incoming requests.
  zx::channel device_name_provider_server_;

  // The outgoing (exposed) connection to the svchost.
  zx::channel svchost_outgoing_;

  // The job in which we run "svc" realm services, like svchost, miscsvc, netsvc, etc.
  zx::job svc_job_;

  // The job in which we run shell processes like consoles and autorun.
  // WARNING: This job is created directly from the root job with no additional job policy
  // restrictions. Specifically, it has ZX_POL_AMBIENT_MARK_VMO_EXEC allowed. It should only be used
  // to launch processes like the shell, autorun processes, and other debug-only functions that are
  // disabled on userdebug/user build types. Because of this, we only create it when the
  // 'console.shell' kernel command line argument is enabled.
  zx::job shell_job_;

  // Used to bind the svchost to the virtual-console binary to provide fidl
  // services.
  zx::channel virtcon_fidl_;

  DevmgrLauncher launcher_;
};

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_SYSTEM_INSTANCE_H_
