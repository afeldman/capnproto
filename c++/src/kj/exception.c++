// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "exception.h"
#include "string.h"
#include "debug.h"
#include <unistd.h>
#include <execinfo.h>
#include <stdlib.h>
#include <exception>

namespace kj {

ArrayPtr<const char> KJ_STRINGIFY(Exception::Nature nature) {
  static const char* NATURE_STRINGS[] = {
    "requirement not met",
    "bug in code",
    "error from OS",
    "network failure",
    "error"
  };

  const char* s = NATURE_STRINGS[static_cast<uint>(nature)];
  return arrayPtr(s, strlen(s));
}

ArrayPtr<const char> KJ_STRINGIFY(Exception::Durability durability) {
  static const char* DURABILITY_STRINGS[] = {
    "temporary",
    "permanent"
  };

  const char* s = DURABILITY_STRINGS[static_cast<uint>(durability)];
  return arrayPtr(s, strlen(s));
}

String KJ_STRINGIFY(const Exception& e) {
  uint contextDepth = 0;

  Maybe<const Exception::Context&> contextPtr = e.getContext();
  for (;;) {
    KJ_IF_MAYBE(c, contextPtr) {
      ++contextDepth;
      contextPtr = c->next.map(
          [](const Own<Exception::Context>& c) -> const Exception::Context& { return *c; });
    } else {
      break;
    }
  }

  Array<String> contextText = heapArray<String>(contextDepth);

  contextDepth = 0;
  contextPtr = e.getContext();
  for (;;) {
    KJ_IF_MAYBE(c, contextPtr) {
      contextText[contextDepth++] =
          str(c->file, ":", c->line, ": context: ", c->description, "\n");
      contextPtr = c->next.map(
          [](const Own<Exception::Context>& c) -> const Exception::Context& { return *c; });
    } else {
      break;
    }
  }

  return str(strArray(contextText, ""),
             e.getFile(), ":", e.getLine(), ": ", e.getNature(),
             e.getDurability() == Exception::Durability::TEMPORARY ? " (temporary)" : "",
             e.getDescription() == nullptr ? "" : ": ", e.getDescription(),
             "\nstack: ", strArray(e.getStackTrace(), " "));
}

Exception::Exception(Nature nature, Durability durability, const char* file, int line,
                     String description) noexcept
    : file(file), line(line), nature(nature), durability(durability),
      description(mv(description)) {
  traceCount = backtrace(trace, 16);
}

Exception::Exception(const Exception& other) noexcept
    : file(other.file), line(other.line), nature(other.nature), durability(other.durability),
      description(str(other.description)), traceCount(other.traceCount) {
  memcpy(trace, other.trace, sizeof(trace[0]) * traceCount);

  KJ_IF_MAYBE(c, other.context) {
    context = heap(**c);
  }
}

Exception::~Exception() noexcept {}

Exception::Context::Context(const Context& other) noexcept
    : file(other.file), line(other.line), description(str(other.description)) {
  KJ_IF_MAYBE(n, other.next) {
    next = heap(**n);
  }
}

void Exception::wrapContext(const char* file, int line, String&& description) {
  context = heap<Context>(file, line, mv(description), mv(context));
}

class ExceptionImpl: public Exception, public std::exception {
public:
  inline ExceptionImpl(Exception&& other): Exception(mv(other)) {}
  ExceptionImpl(const ExceptionImpl& other): Exception(other) {
    // No need to copy whatBuffer since it's just to hold the return value of what().
  }

  const char* what() const noexcept override;

private:
  mutable String whatBuffer;
};

const char* ExceptionImpl::what() const noexcept {
  whatBuffer = str(*this);
  return whatBuffer.begin();
}

// =======================================================================================

namespace {

#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 8)
#define thread_local __thread
#endif

thread_local ExceptionCallback* threadLocalCallback = nullptr;

class RepeatChar {
  // A pseudo-sequence of characters that is actually just one character repeated.
  //
  // TODO(cleanup):  Put this somewhere reusable.  Maybe templatize it too.

public:
  inline RepeatChar(char c, uint size): c(c), size_(size) {}

  class Iterator {
  public:
    Iterator() = default;
    inline Iterator(char c, uint index): c(c), index(index) {}

    inline Iterator& operator++() { ++index; return *this; }
    inline Iterator operator++(int) { ++index; return Iterator(c, index - 1); }

    inline char operator*() const { return c; }

    inline bool operator==(const Iterator& other) const { return index == other.index; }
    inline bool operator!=(const Iterator& other) const { return index != other.index; }

  private:
    char c;
    uint index;
  };

  inline uint size() const { return size_; }
  inline Iterator begin() const { return Iterator(c, 0); }
  inline Iterator end() const { return Iterator(c, size_); }

private:
  char c;
  uint size_;
};
inline RepeatChar KJ_STRINGIFY(RepeatChar value) { return value; }

}  // namespace

ExceptionCallback::ExceptionCallback(): next(getExceptionCallback()) {
  char stackVar;
  ptrdiff_t offset = reinterpret_cast<char*>(this) - &stackVar;
  KJ_ASSERT(offset < 4096 && offset > -4096,
            "ExceptionCallback must be allocated on the stack.");

  threadLocalCallback = this;
}

ExceptionCallback::ExceptionCallback(ExceptionCallback& next): next(next) {}

ExceptionCallback::~ExceptionCallback() {
  if (&next != this) {
    threadLocalCallback = &next;
  }
}

void ExceptionCallback::onRecoverableException(Exception&& exception) {
  next.onRecoverableException(mv(exception));
}

void ExceptionCallback::onFatalException(Exception&& exception) {
  next.onFatalException(mv(exception));
}

void ExceptionCallback::logMessage(const char* file, int line, int contextDepth, String&& text) {
  next.logMessage(file, line, contextDepth, mv(text));
}

class ExceptionCallback::RootExceptionCallback: public ExceptionCallback {
public:
  RootExceptionCallback(): ExceptionCallback(*this) {}

  void onRecoverableException(Exception&& exception) override {
#if KJ_NO_EXCEPTIONS
    logException(mv(exception));
#else
    if (std::uncaught_exception()) {
      // Throwing is probably dangerous.  Log instead.
      logException(mv(exception));
    } else {
      throw ExceptionImpl(mv(exception));
    }
#endif
  }

  void onFatalException(Exception&& exception) override {
#if KJ_NO_EXCEPTIONS
    logException(mv(exception));
#else
    throw ExceptionImpl(mv(exception));
#endif
  }

  void logMessage(const char* file, int line, int contextDepth, String&& text) override {
    if (contextDepth > 0) {
      text = str(RepeatChar('_', contextDepth), mv(text));
    }

    StringPtr textPtr = text;

    while (text != nullptr) {
      ssize_t n = write(STDERR_FILENO, textPtr.begin(), textPtr.size());
      if (n <= 0) {
        // stderr is broken.  Give up.
        return;
      }
      textPtr = textPtr.slice(n);
    }
  }

private:
  void logException(Exception&& e) {
    // We intentionally go back to the top exception callback on the stack because we don't want to
    // bypass whatever log processing is in effect.
    //
    // We intentionally don't log the context since it should get re-added by the exception callback
    // anyway.
    getExceptionCallback().logMessage(e.getFile(), e.getLine(), 0, str(
        e.getNature(), e.getDurability() == Exception::Durability::TEMPORARY ? " (temporary)" : "",
        e.getDescription() == nullptr ? "" : ": ", e.getDescription(),
        "\nstack: ", strArray(e.getStackTrace(), " ")));
  }
};

ExceptionCallback& getExceptionCallback() {
  static ExceptionCallback::RootExceptionCallback defaultCallback;
  ExceptionCallback* scoped = threadLocalCallback;
  return scoped != nullptr ? *scoped : defaultCallback;
}

}  // namespace kj
