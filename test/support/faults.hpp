// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"
#include "support/isa.hpp"

#include <cstdint>
#include <cstdlib>
#if defined(__linux__) && defined(__x86_64__)
#include <bits/types/siginfo_t.h>
#include <bits/types/sigset_t.h>
#include <sys/ucontext.h>
extern "C" {
#include <signal.h>
}

namespace fixture {
class FaultHandler {
public:
  FaultHandler() {
    struct sigaction action{};
    action.sa_sigaction = &handle;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR2);
    fixture::require(pthread_sigmask(SIG_SETMASK, nullptr, &original_mask_) == 0,
                     "read original signal mask");
    auto mask = original_mask_;
    sigaddset(&mask, SIGUSR1);
    fixture::require(pthread_sigmask(SIG_SETMASK, &mask, nullptr) == 0,
                     "set interrupted signal mask");
    fixture::require(sigaction(SIGSEGV, &action, &previous_) == 0,
                     "install consumer SIGSEGV handler");
  }

  ~FaultHandler() {
    (void)sigaction(SIGSEGV, &previous_, nullptr);
    (void)pthread_sigmask(SIG_SETMASK, &original_mask_, nullptr);
  }

  void verify_mask() const {
    sigset_t mask{};
    fixture::require(pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0 &&
                         sigismember(&mask, SIGUSR1) == 1 &&
                         sigismember(&mask, SIGUSR2) == sigismember(&original_mask_, SIGUSR2) &&
                         sigismember(&mask, SIGSEGV) == sigismember(&original_mask_, SIGSEGV),
                     "escape retains interrupted mask and removes handler-only blocked signals");
  }

  FaultHandler(const FaultHandler &) = delete;
  FaultHandler &operator=(const FaultHandler &) = delete;

private:
  struct sigaction previous_{};
  sigset_t original_mask_{};

  static void handle(int signal, siginfo_t *information, void *context) {
    const auto *machine = static_cast<ucontext_t *>(context);
    const auto pc = static_cast<std::uintptr_t>(machine->uc_mcontext.gregs[REG_RIP]);
    const bool writing = (machine->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    sigset_t handler_mask{};
    if (pthread_sigmask(SIG_SETMASK, &machine->uc_sigmask, &handler_mask) != 0) { std::_Exit(1); }
    if (armv5::Core::host_fault(
            reinterpret_cast<std::uintptr_t>(information->si_addr), writing, pc)) {
      return;
    }
    if (pthread_sigmask(SIG_SETMASK, &handler_mask, nullptr) != 0) { std::_Exit(1); }
    std::_Exit(128 + signal);
  }
};

} // namespace fixture
#endif
