//===- RISCVLZ4Packer.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVLZ4Packer.h"
#include "LZ4/lz4hc.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <iterator>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace lld;
using namespace lld::elf;

namespace {

constexpr size_t loaderLen = 168;
constexpr unsigned char loaderTemplate[loaderLen] = {
    0x41, 0x65, 0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0xA5, 0x09, 0x8C, 0x41,
    0x1D, 0x46, 0x93, 0x06, 0x20, 0x03, 0x7D, 0x57, 0x81, 0x47, 0x93, 0x08,
    0xE0, 0x0D, 0x73, 0x00, 0x00, 0x00, 0x97, 0x02, 0x00, 0x00, 0x93, 0x82,
    0x22, 0x08, 0x83, 0xA2, 0x02, 0x00, 0x17, 0x05, 0x00, 0x00, 0x13, 0x05,
    0xA5, 0x07, 0xC1, 0x65, 0x10, 0x41, 0x11, 0x05, 0x2A, 0x96, 0x03, 0x48,
    0x05, 0x00, 0x05, 0x05, 0x13, 0x57, 0x48, 0x00, 0x19, 0xC7, 0x35, 0x20,
    0xAA, 0x86, 0x3D, 0x28, 0x36, 0x85, 0x63, 0x51, 0xC5, 0x02, 0x83, 0x47,
    0x05, 0x00, 0xB3, 0x86, 0xF5, 0x40, 0x83, 0x47, 0x15, 0x00, 0x09, 0x05,
    0xA2, 0x07, 0x9D, 0x8E, 0x13, 0x77, 0xF8, 0x00, 0x29, 0x20, 0x11, 0x07,
    0x31, 0x28, 0xF1, 0xB7, 0x82, 0x82, 0x93, 0x47, 0xF7, 0x00, 0x81, 0xEB,
    0x83, 0x47, 0x05, 0x00, 0x05, 0x05, 0x3E, 0x97, 0x93, 0xC7, 0xF7, 0x0F,
    0xF5, 0xDB, 0x82, 0x80, 0x83, 0xC7, 0x06, 0x00, 0x85, 0x06, 0x23, 0x80,
    0xF5, 0x00, 0x85, 0x05, 0x7D, 0x17, 0x6D, 0xFB, 0x82, 0x80, 0x01, 0x00,
    0x66, 0x66, 0x55, 0x55, 0x22, 0x22, 0x11, 0x11, 0x44, 0x44, 0x33, 0x33};

bool readFile(const std::string &path, std::vector<char> &data) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    std::fprintf(stderr, "[LZ4] failed to open %s: %s\n", path.c_str(),
                 std::strerror(errno));
    return false;
  }
  if (std::fseek(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    return false;
  }
  long len = std::ftell(f);
  if (len <= 0) {
    std::fclose(f);
    return false;
  }
  std::rewind(f);
  data.resize(static_cast<size_t>(len));
  bool ok = std::fread(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  if (!ok)
    std::fprintf(stderr, "[LZ4] failed to read %s\n", path.c_str());
  return ok;
}

bool writeFile(const std::string &path, const std::vector<char> &data) {
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "[LZ4] failed to open %s: %s\n", path.c_str(),
                 std::strerror(errno));
    return false;
  }
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  if (std::fclose(f) != 0)
    ok = false;
  if (!ok)
    std::fprintf(stderr, "[LZ4] failed to write %s\n", path.c_str());
  return ok;
}

template <class T> void append(std::vector<char> &out, const T &value) {
  const char *p = reinterpret_cast<const char *>(&value);
  out.insert(out.end(), p, p + sizeof(T));
}

void appendBytes(std::vector<char> &out, const void *data, size_t size) {
  const char *p = reinterpret_cast<const char *>(data);
  out.insert(out.end(), p, p + size);
}

} // namespace

bool lld::elf::packWithLZ4(llvm::StringRef pathRef) {
  std::string path = pathRef.str();
  struct stat inputStat {};
  if (::stat(path.c_str(), &inputStat) != 0) {
    std::fprintf(stderr, "[LZ4] failed to stat %s: %s\n", path.c_str(),
                 std::strerror(errno));
    return false;
  }

  std::vector<char> input;
  if (!readFile(path, input))
    return false;

  if (input.size() < sizeof(Elf32_Ehdr)) {
    std::fprintf(stderr, "[LZ4] input too small: %s\n", path.c_str());
    return false;
  }

  const auto *eh = reinterpret_cast<const Elf32_Ehdr *>(input.data());
  if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
      eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3 ||
      eh->e_ident[EI_CLASS] != ELFCLASS32 ||
      eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_machine != EM_RISCV) {
    std::fprintf(stderr, "[LZ4] not an ELF32 little-endian RISC-V file: %s\n",
                 path.c_str());
    return false;
  }

  if (eh->e_phoff + uint64_t(eh->e_phnum) * eh->e_phentsize > input.size() ||
      eh->e_phentsize != sizeof(Elf32_Phdr)) {
    std::fprintf(stderr, "[LZ4] invalid program header table: %s\n",
                 path.c_str());
    return false;
  }

  std::vector<const Elf32_Phdr *> loads;
  for (unsigned i = 0; i != eh->e_phnum; ++i) {
    const auto *ph = reinterpret_cast<const Elf32_Phdr *>(
        input.data() + eh->e_phoff + i * eh->e_phentsize);
    if (ph->p_type == PT_LOAD)
      loads.push_back(ph);
  }
  if (loads.empty() || loads.size() > 16) {
    std::fprintf(stderr, "[LZ4] unsupported PT_LOAD count %zu: %s\n",
                 loads.size(), path.c_str());
    return false;
  }

  uint32_t loadStart = loads.front()->p_vaddr;
  uint32_t loadEnd = loadStart + loads.front()->p_memsz;
  uint32_t fileEnd = loadStart + loads.front()->p_filesz;
  for (const Elf32_Phdr *ph : loads) {
    loadStart = std::min(loadStart, ph->p_vaddr);
    loadEnd = std::max(loadEnd, ph->p_vaddr + ph->p_memsz);
    fileEnd = std::max(fileEnd, ph->p_vaddr + ph->p_filesz);
  }
  if (loadEnd <= loadStart) {
    std::fprintf(stderr, "[LZ4] invalid PT_LOAD address range: %s\n",
                 path.c_str());
    return false;
  }
  // Anonymous mmap pages are zero-filled. Keep trailing BSS out of the
  // compressed stream when the loader's 64 KiB mapping round-up still covers
  // the complete memory image; internal gaps remain represented normally.
  const uint32_t memImageSize = loadEnd - loadStart;
  const uint32_t fileImageSize = fileEnd - loadStart;
  uint32_t imageSize = fileImageSize;
  const uint32_t roundedMap = (fileImageSize + 0x10000u) & ~0xffffu;
  if (roundedMap < memImageSize)
    imageSize = memImageSize;
  std::vector<char> image(imageSize, 0);
  for (const Elf32_Phdr *ph : loads) {
    if (ph->p_vaddr < loadStart ||
        ph->p_vaddr - loadStart + ph->p_filesz > image.size() ||
        ph->p_offset + ph->p_filesz > input.size()) {
      std::fprintf(stderr, "[LZ4] invalid PT_LOAD segment: %s\n", path.c_str());
      return false;
    }
    std::memcpy(image.data() + (ph->p_vaddr - loadStart),
                input.data() + ph->p_offset, ph->p_filesz);
  }

  int bound = LZ4_compressBound(static_cast<int>(image.size()));
  std::vector<char> compressed(bound);
  int compressedSize =
      LZ4_compress_HC(image.data(), compressed.data(), image.size(), bound, 12);
  if (compressedSize <= 0) {
    std::fprintf(stderr, "[LZ4] compression failed: %s\n", path.c_str());
    return false;
  }
  compressed.resize(compressedSize);

  std::vector<unsigned char> loader(std::begin(loaderTemplate),
                                    std::end(loaderTemplate));
  uint32_t mmapSize = (imageSize + 0x10000u) & ~0xffffu;
  uint32_t destAddr = eh->e_entry;
  uint32_t dataLen = compressedSize;
  std::memcpy(loader.data() + loaderLen - sizeof(uint32_t) * 3, &mmapSize,
              sizeof(uint32_t));
  std::memcpy(loader.data() + loaderLen - sizeof(uint32_t) * 2, &destAddr,
              sizeof(uint32_t));
  std::memcpy(loader.data() + loaderLen - sizeof(uint32_t), &dataLen,
              sizeof(uint32_t));

  uint32_t loaderVaddr =
      (((0x10000u + imageSize) + 0x10000u) & ~0xffffu) +
      sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr);

  Elf32_Ehdr outEh = {};
  unsigned char ident[EI_NIDENT] = {ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3,
                                    ELFCLASS32, ELFDATA2LSB, EV_CURRENT, 0};
  std::memcpy(outEh.e_ident, ident, EI_NIDENT);
  outEh.e_type = ET_EXEC;
  outEh.e_machine = EM_RISCV;
  outEh.e_version = EV_CURRENT;
  outEh.e_entry = loaderVaddr;
  outEh.e_phoff = sizeof(Elf32_Ehdr);
  outEh.e_shoff = 0;
  outEh.e_flags = eh->e_flags;
  outEh.e_ehsize = sizeof(Elf32_Ehdr);
  outEh.e_phentsize = sizeof(Elf32_Phdr);
  outEh.e_phnum = 1;
  outEh.e_shentsize = 0;
  outEh.e_shnum = 0;
  outEh.e_shstrndx = 0;

  Elf32_Phdr outPh = {};
  outPh.p_type = PT_LOAD;
  outPh.p_offset = sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr);
  outPh.p_vaddr = loaderVaddr;
  outPh.p_paddr = loaderVaddr;
  outPh.p_filesz = loaderLen + compressed.size();
  outPh.p_memsz = loaderLen + compressed.size();
  outPh.p_flags = PF_R | PF_W | PF_X;
  outPh.p_align = 0x1000;

  std::vector<char> out;
  out.reserve(sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr) + loader.size() +
              compressed.size());
  append(out, outEh);
  append(out, outPh);
  appendBytes(out, loader.data(), loader.size());
  appendBytes(out, compressed.data(), compressed.size());

  std::string tmp = path + ".lz4.tmp";
  if (!writeFile(tmp, out)) {
    ::unlink(tmp.c_str());
    return false;
  }
  if (::chmod(tmp.c_str(), inputStat.st_mode & 07777) != 0) {
    std::fprintf(stderr, "[LZ4] chmod %s failed: %s\n", tmp.c_str(),
                 std::strerror(errno));
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    std::fprintf(stderr, "[LZ4] rename %s -> %s failed: %s\n", tmp.c_str(),
                 path.c_str(), std::strerror(errno));
    ::unlink(tmp.c_str());
    return false;
  }

  std::printf("[LZ4] packed %s: %zu -> %zu bytes\n", path.c_str(),
              input.size(), out.size());
  return true;
}
