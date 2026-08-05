//===- CSGORFE.h ------------------------------------------------*- C++ -*-===//
//
// Split a monolithic relocatable .text section into per-function input
// sections so the existing --gc-sections pass can discard unused functions.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_CSGORFE_H
#define LLD_ELF_CSGORFE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace lld::elf {

using namespace llvm::ELF;

class Riscv32FunctionSectionSplitter {
public:
  explicit Riscv32FunctionSectionSplitter(std::string input)
      : inputPath(std::move(input)) {}

  bool run(std::string &outputPath) {
    if (!load() || !validateHeader() || !findCoreSections())
      return false;

    std::vector<FunctionInfo> funcs = collectFunctions();
    if (funcs.size() <= 1 || !validateFunctions(funcs))
      return false;

    std::vector<uint32_t> relocSectionIndices = collectTextRelaSections();
    if (!relocSectionIndices.empty() &&
        !validateRelocations(relocSectionIndices))
      return false;

    // Existing section headers must be the final file contents so appending new
    // headers keeps e_shoff contiguous and valid.
    if (sectionTableEnd() != currentSize)
      return false;

    size_t extraHeaders = funcs.size() - 1;
    for (uint32_t secIndex : relocSectionIndices)
      extraHeaders += countRelocGroups(secIndex, funcs);
    data.resize(currentSize + extraHeaders * sizeof(Elf32_Shdr));

    splitText(funcs);
    splitRelocations(funcs, relocSectionIndices);
    updateSymbols(funcs);
    ehdr()->e_shnum = sectionCount;

    outputPath = inputPath;
    if (llvm::StringRef(outputPath).ends_with(".o"))
      outputPath.insert(outputPath.size() - 2, ".csgorfe");
    else
      outputPath += ".csgorfe.o";

    std::ofstream out(outputPath, std::ios::binary);
    if (!out)
      return false;
    out.write(reinterpret_cast<const char *>(data.data()), currentSize);
    return static_cast<bool>(out);
  }

private:
  struct FunctionInfo {
    uint32_t symIndex = 0;
    uint32_t start = 0;
    uint32_t size = 0;
    uint16_t sectionIndex = 0;
  };

  std::string inputPath;
  std::vector<uint8_t> data;
  size_t currentSize = 0;
  uint32_t textIndex = 0;
  uint32_t symtabIndex = 0;
  uint32_t sectionCount = 0;

  bool load() {
    std::ifstream in(inputPath, std::ios::binary | std::ios::ate);
    if (!in)
      return false;
    std::streamoff size = in.tellg();
    if (size <= 0)
      return false;
    currentSize = static_cast<size_t>(size);
    data.resize(currentSize);
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char *>(data.data()), currentSize);
    return static_cast<size_t>(in.gcount()) == currentSize;
  }

  bool hasRange(uint64_t off, uint64_t size) const {
    return off <= currentSize && size <= currentSize - off;
  }

  Elf32_Ehdr *ehdr() {
    return reinterpret_cast<Elf32_Ehdr *>(data.data());
  }

  const Elf32_Ehdr *ehdr() const {
    return reinterpret_cast<const Elf32_Ehdr *>(data.data());
  }

  Elf32_Shdr *section(uint32_t index) {
    return reinterpret_cast<Elf32_Shdr *>(data.data() + ehdr()->e_shoff +
                                          index * ehdr()->e_shentsize);
  }

  const Elf32_Shdr *section(uint32_t index) const {
    return reinterpret_cast<const Elf32_Shdr *>(data.data() + ehdr()->e_shoff +
                                                index * ehdr()->e_shentsize);
  }

  Elf32_Sym *symbol(uint32_t index) {
    const Elf32_Shdr *symtab = section(symtabIndex);
    return reinterpret_cast<Elf32_Sym *>(data.data() + symtab->sh_offset +
                                         index * symtab->sh_entsize);
  }

  const Elf32_Sym *symbol(uint32_t index) const {
    const Elf32_Shdr *symtab = section(symtabIndex);
    return reinterpret_cast<const Elf32_Sym *>(data.data() + symtab->sh_offset +
                                               index * symtab->sh_entsize);
  }

  size_t sectionTableEnd() const {
    return ehdr()->e_shoff +
           static_cast<size_t>(ehdr()->e_shnum) * ehdr()->e_shentsize;
  }

  bool validateHeader() {
    if (currentSize < sizeof(Elf32_Ehdr))
      return false;
    const Elf32_Ehdr *h = ehdr();
    if (!h->checkMagic())
      return false;
    if (h->e_ident[EI_CLASS] != ELFCLASS32 ||
        h->e_ident[EI_DATA] != ELFDATA2LSB || h->e_type != ET_REL ||
        h->e_machine != EM_RISCV)
      return false;
    if (h->e_shentsize != sizeof(Elf32_Shdr) || h->e_shnum == 0)
      return false;
    return hasRange(h->e_shoff,
                    static_cast<uint64_t>(h->e_shnum) * h->e_shentsize);
  }

  bool findCoreSections() {
    bool foundText = false;
    bool foundSymtab = false;
    sectionCount = ehdr()->e_shnum;
    for (uint32_t i = 0; i < sectionCount; ++i) {
      const Elf32_Shdr *sec = section(i);
      if (sec->sh_type != SHT_NOBITS &&
          !hasRange(sec->sh_offset, sec->sh_size))
        return false;
      if (sec->sh_type == SHT_PROGBITS && (sec->sh_flags & SHF_ALLOC) &&
          (sec->sh_flags & SHF_EXECINSTR)) {
        if (foundText)
          return false;
        foundText = true;
        textIndex = i;
      }
      if (sec->sh_type == SHT_SYMTAB) {
        if (foundSymtab)
          return false;
        foundSymtab = true;
        symtabIndex = i;
      }
    }
    if (!foundText || !foundSymtab)
      return false;
    const Elf32_Shdr *symtab = section(symtabIndex);
    if (symtab->sh_entsize != sizeof(Elf32_Sym) ||
        symtab->sh_link >= sectionCount)
      return false;
    return symtab->sh_size % symtab->sh_entsize == 0;
  }

  std::vector<FunctionInfo> collectFunctions() const {
    const Elf32_Shdr *text = section(textIndex);
    const Elf32_Shdr *symtab = section(symtabIndex);
    uint32_t symCount = symtab->sh_size / symtab->sh_entsize;
    std::vector<FunctionInfo> funcs;
    for (uint32_t i = 0; i < symCount; ++i) {
      const Elf32_Sym *sym = symbol(i);
      if (sym->getType() != STT_FUNC || sym->st_shndx != textIndex ||
          sym->st_size == 0)
        continue;
      if (sym->st_value > text->sh_size ||
          sym->st_size > text->sh_size - sym->st_value)
        return {};
      funcs.push_back({i, sym->st_value, sym->st_size, 0});
    }
    std::sort(funcs.begin(), funcs.end(),
              [](const FunctionInfo &a, const FunctionInfo &b) {
                if (a.start != b.start)
                  return a.start < b.start;
                if (a.size != b.size)
                  return a.size > b.size;
                return a.symIndex < b.symIndex;
              });

    std::vector<FunctionInfo> unique;
    for (const FunctionInfo &func : funcs) {
      if (!unique.empty() && unique.back().start == func.start) {
        unique.back().size = std::max(unique.back().size, func.size);
        continue;
      }
      unique.push_back(func);
    }
    return unique;
  }

  bool validateFunctions(const std::vector<FunctionInfo> &funcs) const {
    const Elf32_Shdr *text = section(textIndex);
    if (funcs.front().start != 0)
      return false;
    for (size_t i = 0; i < funcs.size(); ++i) {
      if (funcs[i].start + funcs[i].size > text->sh_size)
        return false;
      if (i + 1 != funcs.size() &&
          funcs[i].start + funcs[i].size > funcs[i + 1].start)
        return false;
    }
    return true;
  }

  std::vector<uint32_t> collectTextRelaSections() const {
    std::vector<uint32_t> relas;
    for (uint32_t i = 0; i < sectionCount; ++i) {
      const Elf32_Shdr *sec = section(i);
      if (sec->sh_type == SHT_RELA && sec->sh_info == textIndex)
        relas.push_back(i);
    }
    return relas;
  }

  bool validateRelocations(const std::vector<uint32_t> &relocSections) const {
    for (uint32_t secIndex : relocSections) {
      const Elf32_Shdr *sec = section(secIndex);
      if (sec->sh_entsize != sizeof(Elf32_Rela) ||
          sec->sh_size % sec->sh_entsize != 0)
        return false;
      uint32_t count = sec->sh_size / sec->sh_entsize;
      uint32_t lastOffset = 0;
      for (uint32_t i = 0; i < count; ++i) {
        const Elf32_Rela *rel =
            reinterpret_cast<const Elf32_Rela *>(data.data() + sec->sh_offset +
                                                 i * sec->sh_entsize);
        if (i != 0 && rel->r_offset < lastOffset)
          return false;
        lastOffset = rel->r_offset;
      }
    }
    return true;
  }

  uint32_t functionForOffset(const std::vector<FunctionInfo> &funcs,
                             uint32_t offset) const {
    auto it = std::upper_bound(
        funcs.begin(), funcs.end(), offset,
        [](uint32_t value, const FunctionInfo &func) {
          return value < func.start;
        });
    if (it == funcs.begin())
      return 0;
    return static_cast<uint32_t>((it - funcs.begin()) - 1);
  }

  uint32_t countRelocGroups(uint32_t secIndex,
                            const std::vector<FunctionInfo> &funcs) const {
    const Elf32_Shdr *sec = section(secIndex);
    uint32_t count = sec->sh_size / sec->sh_entsize;
    uint32_t groups = 0;
    int32_t lastFunc = -1;
    for (uint32_t i = 0; i < count; ++i) {
      const Elf32_Rela *rel =
          reinterpret_cast<const Elf32_Rela *>(data.data() + sec->sh_offset +
                                               i * sec->sh_entsize);
      int32_t func = functionForOffset(funcs, rel->r_offset);
      if (func != lastFunc) {
        ++groups;
        lastFunc = func;
      }
    }
    // The original relocation section is reused for the first group.
    return groups == 0 ? 0 : groups - 1;
  }

  Elf32_Shdr *appendSectionHeader(const Elf32_Shdr &src) {
    Elf32_Shdr *dst = reinterpret_cast<Elf32_Shdr *>(data.data() + currentSize);
    std::memcpy(dst, &src, sizeof(Elf32_Shdr));
    currentSize += sizeof(Elf32_Shdr);
    ++sectionCount;
    return dst;
  }

  void splitText(std::vector<FunctionInfo> &funcs) {
    Elf32_Shdr *text = section(textIndex);
    Elf32_Shdr originalText = *text;
    funcs[0].sectionIndex = textIndex;
    for (size_t i = 1; i < funcs.size(); ++i) {
      Elf32_Shdr *newText = appendSectionHeader(originalText);
      funcs[i].sectionIndex = sectionCount - 1;
      newText->sh_offset = originalText.sh_offset + funcs[i].start;
      newText->sh_size = funcs[i].size;
    }
    text = section(textIndex);
    text->sh_offset = originalText.sh_offset + funcs[0].start;
    text->sh_size = funcs[0].size;
  }

  void splitRelocations(const std::vector<FunctionInfo> &funcs,
                        const std::vector<uint32_t> &relocSections) {
    for (uint32_t secIndex : relocSections) {
      Elf32_Shdr *rela = section(secIndex);
      Elf32_Shdr originalRela = *rela;
      uint32_t count = originalRela.sh_size / originalRela.sh_entsize;
      if (count == 0) {
        rela->sh_info = funcs[0].sectionIndex;
        continue;
      }

      uint64_t groupOffset = originalRela.sh_offset;
      uint32_t currentFunc = UINT32_MAX;
      Elf32_Shdr *currentRela = nullptr;
      for (uint32_t i = 0; i < count; ++i) {
        Elf32_Rela *rel =
            reinterpret_cast<Elf32_Rela *>(data.data() + originalRela.sh_offset +
                                           i * originalRela.sh_entsize);
        uint32_t func = functionForOffset(funcs, rel->r_offset);
        if (func != currentFunc) {
          if (currentRela == nullptr) {
            currentRela = rela;
            currentRela->sh_size = 0;
          } else {
            currentRela = appendSectionHeader(originalRela);
            currentRela->sh_offset = groupOffset;
            currentRela->sh_size = 0;
          }
          currentFunc = func;
          currentRela->sh_info = funcs[func].sectionIndex;
        }
        rel->r_offset -= funcs[func].start;
        currentRela->sh_size += originalRela.sh_entsize;
        groupOffset += originalRela.sh_entsize;
      }
    }
  }

  void updateSymbols(const std::vector<FunctionInfo> &funcs) {
    const Elf32_Shdr *symtab = section(symtabIndex);
    uint32_t symCount = symtab->sh_size / symtab->sh_entsize;
    for (uint32_t i = 0; i < symCount; ++i) {
      Elf32_Sym *sym = symbol(i);
      if (sym->st_shndx != textIndex)
        continue;
      uint32_t func = functionForOffset(funcs, sym->st_value);
      sym->st_value -= funcs[func].start;
      sym->st_shndx = funcs[func].sectionIndex;
    }
  }
};

inline bool splitRiscv32ObjectFunctions(llvm::StringRef inputPath,
                                        std::string &outputPath) {
  if (!inputPath.ends_with(".o") || inputPath.ends_with(".csgorfe.o") ||
      inputPath.contains("crt") || inputPath.contains("clang"))
    return false;
  Riscv32FunctionSectionSplitter splitter(inputPath.str());
  return splitter.run(outputPath);
}

} // namespace lld::elf

#endif
