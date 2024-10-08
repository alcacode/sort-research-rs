// Copyright 2022 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Interface to vectorized quicksort with dynamic dispatch.
// Blog post: https://tinyurl.com/vqsort-blog
// Paper with measurements: https://arxiv.org/abs/2205.05982
//
// To ensure the overhead of using wide vectors (e.g. AVX2 or AVX-512) is
// worthwhile, we recommend using this code for sorting arrays whose size is at
// least 512 KiB.

#pragma once

#include "../base.h"

namespace hwy {

// Tag arguments that determine the sort order.
struct SortAscending {
  constexpr bool IsAscending() const { return true; }
};
struct SortDescending {
  constexpr bool IsAscending() const { return false; }
};

// User-level caching is no longer required, so this class is no longer
// beneficial. We recommend using the simpler VQSort() interface instead, and
// retain this class only for compatibility. It now just calls VQSort.
class HWY_CONTRIB_DLLEXPORT Sorter {
 public:
  Sorter() {}
  ~Sorter() {}

  // Move-only
  Sorter(const Sorter&) = delete;
  Sorter& operator=(const Sorter&) = delete;
  Sorter(Sorter&& /*other*/) {}
  Sorter& operator=(Sorter&& /*other*/) { return *this; }

  // Sorts keys[0, n). Dispatches to the best available instruction set,
  // and does not allocate memory.
  void operator()(uint64_t* HWY_RESTRICT keys, size_t n, SortAscending) const;
  void operator()(int32_t* HWY_RESTRICT keys, size_t n, SortAscending) const;

  // Unused
  static void Fill24Bytes(const void*, size_t, void*) {}
  static bool HaveFloat64() { return false; }

 private:
  void Delete() {}

  template <typename T>
  T* Get() const {
    return nullptr;
  }

#if HWY_COMPILER_CLANG
  HWY_DIAGNOSTICS(push)
  HWY_DIAGNOSTICS_OFF(disable : 4700, ignored "-Wunused-private-field")
#endif
  void* unused_ = nullptr;
#if HWY_COMPILER_CLANG
  HWY_DIAGNOSTICS(pop)
#endif
};

// Internal use only
HWY_CONTRIB_DLLEXPORT uint64_t* GetGeneratorState();

}  // namespace hwy

#include <time.h>

#include <cstdint>

#include "../base.h"
#include "shared-inl.h"

// Check if we have sys/random.h. First skip some systems on which the check
// itself (features.h) might be problematic.
#if defined(ANDROID) || defined(__ANDROID__) || HWY_ARCH_RVV
#define VQSORT_GETRANDOM 0
#endif

#if !defined(VQSORT_GETRANDOM) && HWY_OS_LINUX
#include <features.h>

// ---- which libc
#if defined(__UCLIBC__)
#define VQSORT_GETRANDOM 1  // added Mar 2015, before uclibc-ng 1.0

#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 25)
#define VQSORT_GETRANDOM 1
#else
#define VQSORT_GETRANDOM 0
#endif

#else
// Assume MUSL, which has getrandom since 2018. There is no macro to test, see
// https://www.openwall.com/lists/musl/2013/03/29/13.
#define VQSORT_GETRANDOM 1

#endif  // ---- which libc
#endif  // linux

#if !defined(VQSORT_GETRANDOM)
#define VQSORT_GETRANDOM 0
#endif

// Choose a seed source for SFC generator: 1=getrandom, 2=CryptGenRandom.
// Allow user override - not all Android support the getrandom wrapper.
#ifndef VQSORT_SECURE_SEED

#if VQSORT_GETRANDOM
#define VQSORT_SECURE_SEED 1
#elif defined(_WIN32) || defined(_WIN64)
#define VQSORT_SECURE_SEED 2
#else
#define VQSORT_SECURE_SEED 0
#endif

#endif  // VQSORT_SECURE_SEED

// Pull in dependencies of the chosen seed source.
#if VQSORT_SECURE_SEED == 1
#include <sys/random.h>
#elif VQSORT_SECURE_SEED == 2
#include <windows.h>
#pragma comment(lib, "advapi32.lib")
// Must come after windows.h.
#include <wincrypt.h>
#endif  // VQSORT_SECURE_SEED

namespace hwy {
namespace {

void Fill16Bytes(void* bytes) {
#if VQSORT_SECURE_SEED == 1
  // May block if urandom is not yet initialized.
  const ssize_t ret = getrandom(bytes, 16, /*flags=*/0);
  if (ret == 16)
    return;
#elif VQSORT_SECURE_SEED == 2
  HCRYPTPROV hProvider{};
  if (CryptAcquireContextA(&hProvider, nullptr, nullptr, PROV_RSA_FULL,
                           CRYPT_VERIFYCONTEXT)) {
    const BOOL ok =
        CryptGenRandom(hProvider, 16, reinterpret_cast<BYTE*>(bytes));
    CryptReleaseContext(hProvider, 0);
    if (ok)
      return;
  }
#endif

  // VQSORT_SECURE_SEED == 0, or one of the above failed. Get some entropy from
  // the address and the clock() timer.
  uint64_t* words = reinterpret_cast<uint64_t*>(bytes);
  uint64_t** seed_stack = &words;
  void (*seed_code)(void*) = &Fill16Bytes;
  const uintptr_t bits_stack = reinterpret_cast<uintptr_t>(seed_stack);
  const uintptr_t bits_code = reinterpret_cast<uintptr_t>(seed_code);
  const uint64_t bits_time = static_cast<uint64_t>(clock());
  words[0] = bits_stack ^ bits_time ^ 0xFEDCBA98;  // "Nothing up my sleeve"
  words[1] = bits_code ^ bits_time ^ 0x01234567;   // constants.
}

}  // namespace

uint64_t* GetGeneratorState() {
  thread_local uint64_t state[3] = {0};
  // This is a counter; zero indicates not yet initialized.
  if (HWY_UNLIKELY(state[2] == 0)) {
    Fill16Bytes(state);
    state[2] = 1;
  }
  return state;
}

}  // namespace hwy

#include "traits-inl.h"
#include "vqsort-inl.h"

HWY_BEFORE_NAMESPACE();
namespace hwy {
namespace HWY_NAMESPACE {

void SortI32Asc(int32_t* HWY_RESTRICT keys, size_t num) {
  SortTag<int32_t> d;
  detail::SharedTraits<detail::TraitsLane<detail::OrderAscending<int32_t>>> st;
  Sort(d, st, keys, num);
}

void SortU64Asc(uint64_t* HWY_RESTRICT keys, size_t num) {
  SortTag<uint64_t> d;
  detail::SharedTraits<detail::TraitsLane<detail::OrderAscending<uint64_t>>> st;
  Sort(d, st, keys, num);
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace hwy
HWY_AFTER_NAMESPACE();

// #if HWY_ONCE
namespace hwy {
// namespace {
// HWY_EXPORT(SortI32Asc);
// }  // namespace

void Sorter::operator()(int32_t* HWY_RESTRICT keys,
                        size_t n,
                        SortAscending) const {
  // HWY_DYNAMIC_DISPATCH(SortI32Asc)(keys, n, Get<int32_t>());
  hwy::HWY_NAMESPACE::SortI32Asc(keys, n);
}

void Sorter::operator()(uint64_t* HWY_RESTRICT keys,
                        size_t n,
                        SortAscending) const {
  // HWY_DYNAMIC_DISPATCH(SortU64Asc)(keys, n, Get<uint64_t>());
  hwy::HWY_NAMESPACE::SortU64Asc(keys, n);
}

}  // namespace hwy
