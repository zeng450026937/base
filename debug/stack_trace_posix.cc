// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/debug/stack_trace.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <ostream>

#if defined(__GLIBCXX__)
#include <cxxabi.h>
#endif

#if defined(OS_MACOSX)
#include <AvailabilityMacros.h>
#endif

#include "base/basictypes.h"
#include "base/debug/debugger.h"
#include "base/eintr_wrapper.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "base/string_number_conversions.h"

#if defined(USE_SYMBOLIZE)
#include "base/third_party/symbolize/symbolize.h"
#endif

namespace base {
namespace debug {

namespace {

volatile sig_atomic_t in_signal_handler = 0;

// The prefix used for mangled symbols, per the Itanium C++ ABI:
// http://www.codesourcery.com/cxx-abi/abi.html#mangling
const char kMangledSymbolPrefix[] = "_Z";

// Characters that can be used for symbols, generated by Ruby:
// (('a'..'z').to_a+('A'..'Z').to_a+('0'..'9').to_a + ['_']).join
const char kSymbolCharacters[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

#if !defined(USE_SYMBOLIZE)
// Demangles C++ symbols in the given text. Example:
//
// "out/Debug/base_unittests(_ZN10StackTraceC1Ev+0x20) [0x817778c]"
// =>
// "out/Debug/base_unittests(StackTrace::StackTrace()+0x20) [0x817778c]"
void DemangleSymbols(std::string* text) {
  // Note: code in this function is NOT async-signal safe (std::string uses
  // malloc internally).

#if defined(__GLIBCXX__)

  std::string::size_type search_from = 0;
  while (search_from < text->size()) {
    // Look for the start of a mangled symbol, from search_from.
    std::string::size_type mangled_start =
        text->find(kMangledSymbolPrefix, search_from);
    if (mangled_start == std::string::npos) {
      break;  // Mangled symbol not found.
    }

    // Look for the end of the mangled symbol.
    std::string::size_type mangled_end =
        text->find_first_not_of(kSymbolCharacters, mangled_start);
    if (mangled_end == std::string::npos) {
      mangled_end = text->size();
    }
    std::string mangled_symbol =
        text->substr(mangled_start, mangled_end - mangled_start);

    // Try to demangle the mangled symbol candidate.
    int status = 0;
    scoped_ptr_malloc<char> demangled_symbol(
        abi::__cxa_demangle(mangled_symbol.c_str(), NULL, 0, &status));
    if (status == 0) {  // Demangling is successful.
      // Remove the mangled symbol.
      text->erase(mangled_start, mangled_end - mangled_start);
      // Insert the demangled symbol.
      text->insert(mangled_start, demangled_symbol.get());
      // Next time, we'll start right after the demangled symbol we inserted.
      search_from = mangled_start + strlen(demangled_symbol.get());
    } else {
      // Failed to demangle.  Retry after the "_Z" we just found.
      search_from = mangled_start + 2;
    }
  }

#endif  // defined(__GLIBCXX__)
}
#endif  // !defined(USE_SYMBOLIZE)

class BacktraceOutputHandler {
 public:
  virtual void HandleOutput(const char* output) = 0;

 protected:
  virtual ~BacktraceOutputHandler() {}
};

void OutputPointer(void* pointer, BacktraceOutputHandler* handler) {
  char buf[1024] = { '\0' };
  handler->HandleOutput(" [0x");
  internal::itoa_r(reinterpret_cast<intptr_t>(pointer), buf, sizeof(buf), 16);
  handler->HandleOutput(buf);
  handler->HandleOutput("]");
}

void ProcessBacktrace(void *const *trace,
                      int size,
                      BacktraceOutputHandler* handler) {
  // NOTE: This code MUST be async-signal safe (it's used by in-process
  // stack dumping signal handler). NO malloc or stdio is allowed here.

#if defined(USE_SYMBOLIZE)
  for (int i = 0; i < size; ++i) {
    handler->HandleOutput("\t");

    char buf[1024] = { '\0' };

    // Subtract by one as return address of function may be in the next
    // function when a function is annotated as noreturn.
    void* address = static_cast<char*>(trace[i]) - 1;
    if (google::Symbolize(address, buf, sizeof(buf)))
      handler->HandleOutput(buf);
    else
      handler->HandleOutput("<unknown>");

    OutputPointer(trace[i], handler);
    handler->HandleOutput("\n");
  }
#else
  bool printed = false;

  // Below part is async-signal unsafe (uses malloc), so execute it only
  // when we are not executing the signal handler.
  if (in_signal_handler == 0) {
    scoped_ptr_malloc<char*> trace_symbols(backtrace_symbols(trace, size));
    if (trace_symbols.get()) {
      for (int i = 0; i < size; ++i) {
        std::string trace_symbol = trace_symbols.get()[i];
        DemangleSymbols(&trace_symbol);
        handler->HandleOutput(trace_symbol.c_str());
        handler->HandleOutput("\n");
      }

      printed = true;
    }
  }

  if (!printed) {
    for (int i = 0; i < size; ++i) {
      OutputPointer(trace[i], handler);
      handler->HandleOutput("\n");
    }
  }
#endif  // defined(USE_SYMBOLIZE)
}

void StackDumpSignalHandler(int signal, siginfo_t* info, ucontext_t* context) {
  // NOTE: This code MUST be async-signal safe.
  // NO malloc or stdio is allowed here.

  // Record the fact that we are in the signal handler now, so that the rest
  // of StackTrace can behave in an async-signal-safe manner.
  in_signal_handler = 1;

  if (BeingDebugged())
    BreakDebugger();

  char buf[1024] = "Received signal ";
  size_t buf_len = strlen(buf);
  internal::itoa_r(signal, buf + buf_len, sizeof(buf) - buf_len, 10);
  RAW_LOG(ERROR, buf);

  debug::StackTrace().PrintBacktrace();

  // TODO(shess): Port to Linux.
#if defined(OS_MACOSX)
  // TODO(shess): Port to 64-bit.
#if ARCH_CPU_X86_FAMILY && ARCH_CPU_32_BITS
  size_t len;

  // NOTE: Even |snprintf()| is not on the approved list for signal
  // handlers, but buffered I/O is definitely not on the list due to
  // potential for |malloc()|.
  len = static_cast<size_t>(
      snprintf(buf, sizeof(buf),
               "ax: %x, bx: %x, cx: %x, dx: %x\n",
               context->uc_mcontext->__ss.__eax,
               context->uc_mcontext->__ss.__ebx,
               context->uc_mcontext->__ss.__ecx,
               context->uc_mcontext->__ss.__edx));
  write(STDERR_FILENO, buf, std::min(len, sizeof(buf) - 1));

  len = static_cast<size_t>(
      snprintf(buf, sizeof(buf),
               "di: %x, si: %x, bp: %x, sp: %x, ss: %x, flags: %x\n",
               context->uc_mcontext->__ss.__edi,
               context->uc_mcontext->__ss.__esi,
               context->uc_mcontext->__ss.__ebp,
               context->uc_mcontext->__ss.__esp,
               context->uc_mcontext->__ss.__ss,
               context->uc_mcontext->__ss.__eflags));
  write(STDERR_FILENO, buf, std::min(len, sizeof(buf) - 1));

  len = static_cast<size_t>(
      snprintf(buf, sizeof(buf),
               "ip: %x, cs: %x, ds: %x, es: %x, fs: %x, gs: %x\n",
               context->uc_mcontext->__ss.__eip,
               context->uc_mcontext->__ss.__cs,
               context->uc_mcontext->__ss.__ds,
               context->uc_mcontext->__ss.__es,
               context->uc_mcontext->__ss.__fs,
               context->uc_mcontext->__ss.__gs));
  write(STDERR_FILENO, buf, std::min(len, sizeof(buf) - 1));
#endif  // ARCH_CPU_32_BITS
#endif  // defined(OS_MACOSX)
  _exit(1);
}

class PrintBacktraceOutputHandler : public BacktraceOutputHandler {
 public:
  PrintBacktraceOutputHandler() {}

  virtual void HandleOutput(const char* output) {
    // NOTE: This code MUST be async-signal safe (it's used by in-process
    // stack dumping signal handler). NO malloc or stdio is allowed here.
    ignore_result(HANDLE_EINTR(write(STDERR_FILENO, output, strlen(output))));
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(PrintBacktraceOutputHandler);
};

class StreamBacktraceOutputHandler : public BacktraceOutputHandler {
 public:
  StreamBacktraceOutputHandler(std::ostream* os) : os_(os) {
  }

  virtual void HandleOutput(const char* output) {
    (*os_) << output;
  }

 private:
  std::ostream* os_;

  DISALLOW_COPY_AND_ASSIGN(StreamBacktraceOutputHandler);
};

void WarmUpBacktrace() {
  // Warm up stack trace infrastructure. It turns out that on the first
  // call glibc initializes some internal data structures using pthread_once,
  // and even backtrace() can call malloc(), leading to hangs.
  //
  // Example stack trace snippet (with tcmalloc):
  //
  // #8  0x0000000000a173b5 in tc_malloc
  //             at ./third_party/tcmalloc/chromium/src/debugallocation.cc:1161
  // #9  0x00007ffff7de7900 in _dl_map_object_deps at dl-deps.c:517
  // #10 0x00007ffff7ded8a9 in dl_open_worker at dl-open.c:262
  // #11 0x00007ffff7de9176 in _dl_catch_error at dl-error.c:178
  // #12 0x00007ffff7ded31a in _dl_open (file=0x7ffff625e298 "libgcc_s.so.1")
  //             at dl-open.c:639
  // #13 0x00007ffff6215602 in do_dlopen at dl-libc.c:89
  // #14 0x00007ffff7de9176 in _dl_catch_error at dl-error.c:178
  // #15 0x00007ffff62156c4 in dlerror_run at dl-libc.c:48
  // #16 __GI___libc_dlopen_mode at dl-libc.c:165
  // #17 0x00007ffff61ef8f5 in init
  //             at ../sysdeps/x86_64/../ia64/backtrace.c:53
  // #18 0x00007ffff6aad400 in pthread_once
  //             at ../nptl/sysdeps/unix/sysv/linux/x86_64/pthread_once.S:104
  // #19 0x00007ffff61efa14 in __GI___backtrace
  //             at ../sysdeps/x86_64/../ia64/backtrace.c:104
  // #20 0x0000000000752a54 in base::debug::StackTrace::StackTrace
  //             at base/debug/stack_trace_posix.cc:175
  // #21 0x00000000007a4ae5 in
  //             base::(anonymous namespace)::StackDumpSignalHandler
  //             at base/process_util_posix.cc:172
  // #22 <signal handler called>
  StackTrace stack_trace;
}

}  // namespace

#if !defined(OS_IOS)
bool EnableInProcessStackDumping() {
  // When running in an application, our code typically expects SIGPIPE
  // to be ignored.  Therefore, when testing that same code, it should run
  // with SIGPIPE ignored as well.
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  bool success = (sigaction(SIGPIPE, &action, NULL) == 0);

  // Avoid hangs during backtrace initialization, see above.
  WarmUpBacktrace();

  sig_t handler = reinterpret_cast<sig_t>(&StackDumpSignalHandler);
  success &= (signal(SIGILL, handler) != SIG_ERR);
  success &= (signal(SIGABRT, handler) != SIG_ERR);
  success &= (signal(SIGFPE, handler) != SIG_ERR);
  success &= (signal(SIGBUS, handler) != SIG_ERR);
  success &= (signal(SIGSEGV, handler) != SIG_ERR);
  success &= (signal(SIGSYS, handler) != SIG_ERR);

  return success;
}
#endif  // !defined(OS_IOS)

StackTrace::StackTrace() {
  // NOTE: This code MUST be async-signal safe (it's used by in-process
  // stack dumping signal handler). NO malloc or stdio is allowed here.

  // Though the backtrace API man page does not list any possible negative
  // return values, we take no chance.
  count_ = std::max(backtrace(trace_, arraysize(trace_)), 0);
}

void StackTrace::PrintBacktrace() const {
  // NOTE: This code MUST be async-signal safe (it's used by in-process
  // stack dumping signal handler). NO malloc or stdio is allowed here.

  PrintBacktraceOutputHandler handler;
  ProcessBacktrace(trace_, count_, &handler);
}

void StackTrace::OutputToStream(std::ostream* os) const {
  StreamBacktraceOutputHandler handler(os);
  ProcessBacktrace(trace_, count_, &handler);
}

namespace internal {

// NOTE: code from sandbox/linux/seccomp-bpf/demo.cc.
char *itoa_r(intptr_t i, char *buf, size_t sz, int base) {
  // Make sure we can write at least one NUL byte.
  size_t n = 1;
  if (n > sz)
    return NULL;

  if (base < 2 || base > 16) {
    buf[0] = '\000';
    return NULL;
  }

  char *start = buf;

  uintptr_t j = i;

  // Handle negative numbers (only for base 10).
  if (i < 0 && base == 10) {
    j = -i;

    // Make sure we can write the '-' character.
    if (++n > sz) {
      buf[0] = '\000';
      return NULL;
    }
    *start++ = '-';
  }

  // Loop until we have converted the entire number. Output at least one
  // character (i.e. '0').
  char *ptr = start;
  do {
    // Make sure there is still enough space left in our output buffer.
    if (++n > sz) {
      buf[0] = '\000';
      return NULL;
    }

    // Output the next digit.
    *ptr++ = "0123456789abcdef"[j % base];
    j /= base;
  } while (j);

  // Terminate the output with a NUL character.
  *ptr = '\000';

  // Conversion to ASCII actually resulted in the digits being in reverse
  // order. We can't easily generate them in forward order, as we can't tell
  // the number of characters needed until we are done converting.
  // So, now, we reverse the string (except for the possible "-" sign).
  while (--ptr > start) {
    char ch = *ptr;
    *ptr = *start;
    *start++ = ch;
  }
  return buf;
}

}  // namespace internal

}  // namespace debug
}  // namespace base
