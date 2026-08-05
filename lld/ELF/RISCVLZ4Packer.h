//===- RISCVLZ4Packer.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_RISCV_LZ4_PACKER_H
#define LLD_ELF_RISCV_LZ4_PACKER_H

#include "llvm/ADT/StringRef.h"

namespace lld::elf {
bool packWithLZ4(llvm::StringRef path);
} // namespace lld::elf

#endif
