#include "bolt/Passes/RISCV/RISCVElimUnusedFuncs.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/BinarySection.h"
#include "bolt/Core/MCPlusBuilder.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace llvm {
namespace bolt {

namespace opts {
} // namespace opts

namespace {

using FunctionSet = SmallPtrSet<BinaryFunction *, 16>;
using EdgeMap = DenseMap<BinaryFunction *, SmallVector<BinaryFunction *, 4>>;
using AliasMap = DenseMap<BinaryFunction *, BinaryFunction *>;
bool isRISCVDirectControlRelocation(uint64_t Type) {
  switch (Type) {
  case ELF::R_RISCV_CALL:
  case ELF::R_RISCV_CALL_PLT:
  case ELF::R_RISCV_JAL:
  case ELF::R_RISCV_BRANCH:
  case ELF::R_RISCV_RVC_BRANCH:
  case ELF::R_RISCV_RVC_JUMP:
    return true;
  default:
    return false;
  }
}

uint64_t getRelocationAddress(const BinarySection &Section,
                              const Relocation &Rel) {
  if (Section.containsAddress(Rel.Offset))
    return Rel.Offset;
  return Section.getAddress() + Rel.Offset;
}

BinaryFunction *resolveRelocationTarget(BinaryContext &BC,
                                        const Relocation &Rel) {
  if (Rel.Symbol) {
    if (BinaryFunction *BF = BC.getFunctionForSymbol(Rel.Symbol))
      return BF;
  }

  // For absolute relocations the extracted value is often the final target.
  // PC-relative encodings are intentionally ignored here because Rel.Value is
  // not an absolute address for those forms.
  if (Rel.Value && !Rel.isPCRelative())
    return BC.getBinaryFunctionContainingAddress(Rel.Value,
                                                 /*CheckPastEnd=*/true,
                                                 /*UseMaxSize=*/true);

  return nullptr;
}

BinaryFunction *canonicalize(BinaryFunction *BF, const AliasMap &Aliases) {
  while (BF) {
    auto It = Aliases.find(BF);
    if (It == Aliases.end())
      return BF;
    BF = It->second;
  }
  return nullptr;
}

bool sameOriginSection(const BinaryFunction &A, const BinaryFunction &B) {
  const BinarySection *AS = A.getOriginSection();
  const BinarySection *BS = B.getOriginSection();
  return AS && BS && AS == BS;
}

AliasMap computeOverlappingAliases(BinaryContext &BC) {
  AliasMap Aliases;
  SmallVector<BinaryFunction *, 0> Functions;
  for (auto &Entry : BC.getBinaryFunctions())
    Functions.push_back(&Entry.second);

  for (unsigned I = 0; I < Functions.size(); ++I) {
    BinaryFunction *BF = Functions[I];
    if (!BF->getSize() || BF->isInjected() || BF->isPseudo())
      continue;

    BinaryFunction *BestOuter = nullptr;
    uint64_t BestEnd = 0;
    for (unsigned J = 0; J < I; ++J) {
      BinaryFunction *Outer = Functions[J];
      if (!Outer->getSize() || Outer->isInjected() || Outer->isPseudo())
        continue;
      if (!sameOriginSection(*Outer, *BF))
        continue;
      if (Outer->getAddress() >= BF->getAddress())
        continue;

      const uint64_t OuterEnd = Outer->getAddress() + Outer->getSize();
      if (OuterEnd <= BF->getAddress())
        continue;

      if (!BestOuter || OuterEnd > BestEnd ||
          (OuterEnd == BestEnd && Outer->getAddress() < BestOuter->getAddress())) {
        BestOuter = Outer;
        BestEnd = OuterEnd;
      }
    }

    if (BestOuter)
      Aliases[BF] = BestOuter;
  }

  return Aliases;
}

bool isConservativeRoot(const BinaryFunction &BF) {
  if (BF.isIgnored() || BF.isPseudo() || BF.isFolded() || BF.isFragment())
    return true;
  if (!BF.isSimple() || !BF.hasCFG())
    return true;
  if (BF.isMultiEntry() || BF.hasUnknownControlFlow() || !BF.hasExternalRefRelocations())
    return true;
  if (BF.hasEHRanges() || BF.hasCFI() || BF.hasJumpTables() ||
      BF.getPersonalityFunction())
    return true;
  return false;
}

bool canClearBody(const BinaryFunction &BF, bool RequireExternalRefRelocs) {
  if (BF.isIgnored() || BF.isPseudo() || BF.isFolded() || BF.isFragment())
    return false;
  if (!BF.hasCFG())
    return false;
  if (!RequireExternalRefRelocs)
    return BF.hasNonPseudoInstructions();
  if (!BF.isSimple())
    return false;
  if (BF.isMultiEntry() || BF.hasUnknownControlFlow())
    return false;
  if (RequireExternalRefRelocs && !BF.hasExternalRefRelocations())
    return false;
  if (BF.hasEHRanges() || BF.hasCFI() || BF.getPersonalityFunction())
    return false;
  return BF.hasNonPseudoInstructions();
}

void addEdge(EdgeMap &Edges, BinaryFunction *From, BinaryFunction *To,
             const AliasMap &Aliases) {
  From = canonicalize(From, Aliases);
  To = canonicalize(To, Aliases);
  if (!From || !To || From == To)
    return;
  Edges[From].push_back(To);
}

void collectInstructionEdges(BinaryContext &BC, EdgeMap &Edges,
                             const AliasMap &Aliases) {
  for (auto &Entry : BC.getBinaryFunctions()) {
    BinaryFunction &BF = Entry.second;
    if (!BF.isSimple() || !BF.hasCFG())
      continue;

    BinaryFunction *From = canonicalize(&BF, Aliases);
    for (BinaryBasicBlock &BB : BF) {
      uint64_t Offset = BB.getOffset();
      for (const MCInst &Inst : BB) {
        if (BC.MIB->isPseudo(Inst))
          continue;

        const uint64_t Size = BC.computeInstructionSize(Inst);
        const uint64_t InstOffset = BC.MIB->getOffset(Inst).value_or(Offset);
        const uint64_t InstAddress = BF.getAddress() + InstOffset;

        if (BC.MIB->isCall(Inst) || BC.MIB->isBranch(Inst)) {
          if (const MCSymbol *TargetSymbol = BC.MIB->getTargetSymbol(Inst)) {
            if (BinaryFunction *Target = BC.getFunctionForSymbol(TargetSymbol))
              addEdge(Edges, From, Target, Aliases);
          } else {
            uint64_t TargetAddress = 0;
            if (BC.MIB->evaluateBranch(Inst, InstAddress, Size, TargetAddress)) {
              if (BinaryFunction *Target = BC.getBinaryFunctionContainingAddress(
                      TargetAddress, /*CheckPastEnd=*/true,
                      /*UseMaxSize=*/true))
                addEdge(Edges, From, Target, Aliases);
            }
          }
        }

        Offset += Size;
      }
    }
  }
}

void collectRelocationEdgesAndRoots(BinaryContext &BC, EdgeMap &Edges,
                                    FunctionSet &Roots,
                                    FunctionSet &Protected,
                                    const AliasMap &Aliases) {
  auto markAddressTaken = [&](BinaryFunction *Target) {
    if (!Target)
      return;
    Protected.insert(Target);
    if (BinaryFunction *Canonical = canonicalize(Target, Aliases))
      Roots.insert(Canonical);
  };

  for (const BinarySection &Section : BC.allocatableSections()) {
    const bool FromText = Section.isText();
    for (const Relocation &Rel : Section.relocations()) {
      BinaryFunction *Target = resolveRelocationTarget(BC, Rel);
      if (!Target)
        continue;

      const uint64_t RelAddress = getRelocationAddress(Section, Rel);
      BinaryFunction *Source = FromText
                                   ? BC.getBinaryFunctionContainingAddress(
                                         RelAddress, /*CheckPastEnd=*/false,
                                         /*UseMaxSize=*/true)
                                   : nullptr;

      if (FromText && Source && isRISCVDirectControlRelocation(Rel.Type)) {
        addEdge(Edges, Source, Target, Aliases);
        continue;
      }

      markAddressTaken(Target);
    }

    for (const Relocation &Rel : Section.dynamicRelocations())
      markAddressTaken(resolveRelocationTarget(BC, Rel));
  }
}

void clearFunctionBody(BinaryFunction &BF) {
  for (BinaryBasicBlock &BB : BF)
    BB.clear();
}

} // namespace

void RISCVElimUnusedFuncs::runOnFunctions(BinaryContext &BC) {
  AliasMap Aliases = computeOverlappingAliases(BC);
  EdgeMap Edges;
  FunctionSet Roots;
  FunctionSet Protected;
  FunctionSet Live;
  SmallVector<BinaryFunction *, 32> Worklist;
  auto addRoot = [&](BinaryFunction *BF, bool Protect = false) {
    if (!BF)
      return;
    if (Protect)
      Protected.insert(BF);
    BinaryFunction *Canonical = canonicalize(BF, Aliases);
    if (!Canonical)
      return;
    Roots.insert(Canonical);
  };

  if (BC.StartFunctionAddress) {
    addRoot(BC.getBinaryFunctionContainingAddress(*BC.StartFunctionAddress,
                                                  /*CheckPastEnd=*/true,
                                                  /*UseMaxSize=*/true),
            /*Protect=*/true);
  }
  if (BC.FiniFunctionAddress) {
    addRoot(BC.getBinaryFunctionContainingAddress(*BC.FiniFunctionAddress,
                                                  /*CheckPastEnd=*/true,
                                                  /*UseMaxSize=*/true),
            /*Protect=*/true);
  }

  for (auto &Entry : BC.getBinaryFunctions()) {
    BinaryFunction &BF = Entry.second;
    if (isConservativeRoot(BF))
      addRoot(&BF, /*Protect=*/true);
  }

  collectInstructionEdges(BC, Edges, Aliases);
  collectRelocationEdgesAndRoots(BC, Edges, Roots, Protected, Aliases);

  auto markLive = [&](BinaryFunction *BF) {
    if (!BF || !Live.insert(BF).second)
      return;
    Worklist.push_back(BF);
  };

  for (BinaryFunction *Root : Roots)
    markLive(Root);
  for (BinaryFunction *BF : Protected)
    markLive(BF);

  while (!Worklist.empty()) {
    BinaryFunction *BF = Worklist.pop_back_val();
    auto It = Edges.find(canonicalize(BF, Aliases));
    if (It == Edges.end())
      continue;
    for (BinaryFunction *Target : It->second)
      markLive(Target);
  }

  unsigned NumAliasCleared = 0;
  unsigned NumUnreachableCleared = 0;
  uint64_t BytesCleared = 0;

  for (auto &Entry : BC.getBinaryFunctions()) {
    BinaryFunction &BF = Entry.second;
    if (!canClearBody(BF, /*RequireExternalRefRelocs=*/true))
      continue;

    const bool IsAlias = Aliases.contains(&BF);
    const bool IsProtected = Protected.contains(&BF);
    const bool IsLive = Live.contains(&BF);
    if ((!IsAlias || IsProtected) && IsLive)
      continue;

    BytesCleared += BF.getSize();
    clearFunctionBody(BF);
    if (IsAlias && !IsProtected)
      ++NumAliasCleared;
    else
      ++NumUnreachableCleared;
  }

  if (NumAliasCleared || NumUnreachableCleared) {
    outs() << "BOLT-INFO: riscv-elim-unused-funcs cleared "
           << NumAliasCleared << " overlapping aliases and "
           << NumUnreachableCleared << " unreachable functions (input bytes "
           << BytesCleared << ")"
           << "\n";
  }
}

} // namespace bolt
} // namespace llvm
