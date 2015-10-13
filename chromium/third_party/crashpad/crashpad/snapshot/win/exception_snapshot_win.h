// Copyright 2015 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CRASHPAD_SNAPSHOT_WIN_EXCEPTION_SNAPSHOT_WIN_H_
#define CRASHPAD_SNAPSHOT_WIN_EXCEPTION_SNAPSHOT_WIN_H_

#include <stdint.h>
#include <windows.h>

#include "base/basictypes.h"
#include "build/build_config.h"
#include "snapshot/cpu_context.h"
#include "snapshot/exception_snapshot.h"
#include "util/misc/initialization_state_dcheck.h"
#include "util/win/address_types.h"
#include "util/win/process_structs.h"

namespace crashpad {

class ProcessReaderWin;

namespace internal {

class ExceptionSnapshotWin final : public ExceptionSnapshot {
 public:
  ExceptionSnapshotWin();
  ~ExceptionSnapshotWin() override;

  //! \brief Initializes the object.
  //!
  //! \param[in] process_reader A ProcessReader for the process that sustained
  //!     the exception.
  //! \param[in] thread_id The thread ID in which the exception occurred.
  //! \param[in] exception_pointers_address The address of an
  //!     `EXCEPTION_POINTERS` record in the target process, passed through from
  //!     the exception handler.
  //!
  //! \return `true` if the snapshot could be created, `false` otherwise with
  //!     an appropriate message logged.
  bool Initialize(ProcessReaderWin* process_reader,
                  DWORD thread_id,
                  WinVMAddress exception_pointers);

  // ExceptionSnapshot:

  const CPUContext* Context() const override;
  uint64_t ThreadID() const override;
  uint32_t Exception() const override;
  uint32_t ExceptionInfo() const override;
  uint64_t ExceptionAddress() const override;
  const std::vector<uint64_t>& Codes() const override;

 private:
  template <class ExceptionRecordType,
            class ExceptionPointersType,
            class ContextType>
  bool InitializeFromExceptionPointers(const ProcessReaderWin& process_reader,
                                       WinVMAddress exception_pointers_address,
                                       ContextType* context_record);

#if defined(ARCH_CPU_X86_FAMILY)
  union {
    CPUContextX86 x86;
    CPUContextX86_64 x86_64;
  } context_union_;
#endif
  CPUContext context_;
  std::vector<uint64_t> codes_;
  uint64_t thread_id_;
  uint64_t exception_address_;
  uint32_t exception_flags_;
  DWORD exception_code_;
  InitializationStateDcheck initialized_;

  DISALLOW_COPY_AND_ASSIGN(ExceptionSnapshotWin);
};

}  // namespace internal
}  // namespace crashpad

#endif  // CRASHPAD_SNAPSHOT_WIN_EXCEPTION_SNAPSHOT_WIN_H_
