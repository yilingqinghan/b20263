#include "Config.h"
#include "InputSection.h"
#include "OutputSections.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/CommonLinkerContext.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace lld;
using namespace lld::elf;

namespace lld::elf {

struct RISCVGPPlacement {
  OutputSection *section = nullptr;
  uint64_t value = 0x800;
  uint64_t score = 0;
};

static bool isRISCVGPDataSection(const OutputSection *sec) {
  return sec && (sec->name == ".data" || sec->name == ".sdata" ||
                 sec->name == ".sbss" || sec->name == ".bss");
}

static uint64_t countCoveredRefs(ArrayRef<uint64_t> addrs, uint64_t gpVA) {
  uint64_t covered = 0;
  for (uint64_t addr : addrs)
    if (llvm::isInt<12>(static_cast<int64_t>(addr - gpVA)))
      ++covered;
  return covered;
}

RISCVGPPlacement calculateRISCVGP(
    llvm::SmallVectorImpl<OutputSection *> &outputSections) {
  RISCVGPPlacement result;

  OutputSection *defaultSec = nullptr;
  llvm::DenseSet<const OutputSection *> dataSections;
  for (OutputSection *sec : outputSections) {
    if (!isRISCVGPDataSection(sec))
      continue;
    dataSections.insert(sec);
    if (!defaultSec || sec->name == ".sdata")
      defaultSec = sec;
  }

  if (!defaultSec)
    return result;

  result.section = defaultSec;
  result.value = 0x800;
  const uint64_t defaultGpVA = defaultSec->addr + result.value;

  SmallVector<uint64_t, 0> addrs;
  addrs.reserve(256);
  for (InputSectionBase *base : ctx.inputSections) {
    auto *isec = dyn_cast<InputSection>(base);
    if (!isec || !isec->isLive())
      continue;
    for (const Relocation &reloc : isec->relocations) {
      if (reloc.type != llvm::ELF::R_RISCV_HI20 &&
          reloc.type != llvm::ELF::R_RISCV_LO12_I &&
          reloc.type != llvm::ELF::R_RISCV_LO12_S)
        continue;
      auto *sym = dyn_cast<Defined>(reloc.sym);
      if (!sym || !sym->section)
        continue;
      auto *targetSec = sym->section->getOutputSection();
      if (!targetSec || !dataSections.contains(targetSec))
        continue;
      addrs.push_back(sym->getVA(reloc.addend));
    }
  }

  if (addrs.empty())
    return result;

  llvm::sort(addrs);
  const uint64_t defaultScore = countCoveredRefs(addrs, defaultGpVA);
  result.score = defaultScore;

  size_t bestL = 0;
  size_t l = 0;
  size_t bestCount = 0;
  for (size_t r = 0; r < addrs.size(); ++r) {
    while (addrs[r] - addrs[l] > 0xfff)
      ++l;
    const size_t count = r - l + 1;
    if (count > bestCount) {
      bestCount = count;
      bestL = l;
    }
  }

  if (bestCount <= defaultScore)
    return result;

  const uint64_t bestGpVA = addrs[bestL] + 0x800;
  for (OutputSection *sec : outputSections) {
    if (!isRISCVGPDataSection(sec) || bestGpVA < sec->addr ||
        bestGpVA >= sec->addr + sec->size)
      continue;
    result.section = sec;
    result.value = bestGpVA - sec->addr;
    result.score = bestCount;
    llvm::errs() << "[GP-ADAPT] default=0x" << llvm::format_hex(defaultGpVA, 10)
                 << " score=" << defaultScore << " new=0x"
                 << llvm::format_hex(bestGpVA, 10) << " score=" << bestCount
                 << "\n";
    return result;
  }

  return result;
}

} // namespace lld::elf
