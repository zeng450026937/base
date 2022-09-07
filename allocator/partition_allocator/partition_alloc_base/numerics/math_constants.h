// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_BASE_NUMERICS_MATH_CONSTANTS_H_
#define BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_BASE_NUMERICS_MATH_CONSTANTS_H_

namespace partition_alloc::internal::base {

constexpr double kPiDouble = 3.14159265358979323846;
constexpr float kPiFloat = 3.14159265358979323846f;

// The mean acceleration due to gravity on Earth in m/s^2.
constexpr double kMeanGravityDouble = 9.80665;
constexpr float kMeanGravityFloat = 9.80665f;

}  // namespace partition_alloc::internal::base

#endif  // BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_BASE_NUMERICS_MATH_CONSTANTS_H_
