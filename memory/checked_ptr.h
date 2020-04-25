// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_MEMORY_CHECKED_PTR_H_
#define BASE_MEMORY_CHECKED_PTR_H_

#include <cstddef>  // for std::nullptr_t
#include <utility>  // for std::swap

namespace base {

// DO NOT USE! EXPERIMENTAL ONLY! This is helpful for local testing!
//
// CheckedPtr is meant to be a pointer wrapper, that will crash on
// Use-After-Free (UaF) to prevent security issues. This is very much in the
// experimental phase. More context in:
// https://docs.google.com/document/d/1pnnOAIz_DMWDI4oIOFoMAqLnf_MZ2GsrJNb_dbQ3ZBg
//
// For now, CheckedPtr is a no-op wrapper to aid local testing.
//
// Goals for this API:
// 1. Minimize amount of caller-side changes as much as physically possible.
// 2. Keep this class as small as possible, while still satisfying goal #1 (i.e.
//    we aren't striving to maximize compatibility with raw pointers, merely
//    adding support for cases encountered so far).
template <typename T>
class CheckedPtr {
 public:
  constexpr CheckedPtr() noexcept = default;
  // Deliberately implicit, because CheckedPtr is supposed to resemble raw ptr.
  // NOLINTNEXTLINE(runtime/explicit)
  constexpr CheckedPtr(std::nullptr_t) noexcept : ptr_(nullptr) {}
  // Deliberately implicit, because CheckedPtr is supposed to resemble raw ptr.
  // NOLINTNEXTLINE(runtime/explicit)
  CheckedPtr(T* p) noexcept : ptr_(p) {}

  // In addition to nullptr_t constructor above, CheckedPtr needs to have these
  // as |=default| or |constexpr| to avoid hitting -Wglobal-constructors in
  // cases like this:
  //     struct SomeStruct { int int_field; CheckedPtr<int> ptr_field; };
  //     SomeStruct g_global_var = { 123, nullptr };
  CheckedPtr(const CheckedPtr&) noexcept = default;
  CheckedPtr(CheckedPtr&&) noexcept = default;
  CheckedPtr& operator=(const CheckedPtr&) noexcept = default;
  CheckedPtr& operator=(CheckedPtr&&) noexcept = default;

  CheckedPtr& operator=(T* p) noexcept {
    ptr_ = p;
    return *this;
  }

  ~CheckedPtr() = default;

  // Avoid using. The goal of CheckedPtr is to be as close to raw pointer as
  // possible, so use it only if absolutely necessary (e.g. for const_cast).
  T* get() const { return ptr_; }

  explicit operator bool() const { return ptr_ != nullptr; }

  T* operator->() const { return get(); }
  // Deliberately implicit, because CheckedPtr is supposed to resemble raw ptr.
  // NOLINTNEXTLINE(runtime/explicit)
  operator T*() const { return get(); }
  template <typename U>
  explicit operator U*() const {
    return static_cast<U*>(get());
  }
  // Note: |T& operator*()| isn't needed, because |operator T*()| will take care
  // of |*ptr| dereferences. Better to go this way, to avoid |void&| problem.

  CheckedPtr& operator++() {
    ++ptr_;
    return *this;
  }

  void swap(CheckedPtr& other) noexcept { std::swap(ptr_, other.ptr_); }

 private:
  T* ptr_ = nullptr;
};

}  // namespace base

using base::CheckedPtr;

template <typename T>
void swap(CheckedPtr<T>& lhs, CheckedPtr<T>& rhs) noexcept {
  lhs.swap(rhs);
}

#endif  // BASE_MEMORY_CHECKED_PTR_H_
