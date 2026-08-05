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
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace llvm {
namespace bolt {

namespace opts {
static cl::opt<std::string> RISCVElimRuntimeTrace(
    "riscv-elim-runtime-trace",
    cl::desc("qemu -d exec trace used to remove functions not executed by the "
             "current benchmark run"),
    cl::Hidden, cl::init(""));

static cl::opt<bool> RISCVElimUseOldTextOnly(
    "riscv-elim-use-old-text-only",
    cl::desc("let eligible functions keep executing from the original .text "
             "instead of being re-emitted by BOLT"),
    cl::Hidden, cl::init(false));

static cl::opt<unsigned> RISCVElimUseOldTextMaxTraceCount(
    "riscv-elim-use-old-text-max-trace-count",
    cl::desc("with -riscv-elim-use-old-text-only, keep re-emitting functions "
             "with more trace hits than this threshold"),
    cl::Hidden, cl::init(0));
} // namespace opts

namespace {

using FunctionSet = SmallPtrSet<BinaryFunction *, 16>;
using EdgeMap = DenseMap<BinaryFunction *, SmallVector<BinaryFunction *, 4>>;
using AliasMap = DenseMap<BinaryFunction *, BinaryFunction *>;
using TraceCountMap = DenseMap<BinaryFunction *, uint64_t>;

struct RuntimeTraceInfo {
  FunctionSet Executed;
  TraceCountMap Counts;
  uint64_t NumAddresses = 0;
};

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

bool parseHexAddress(StringRef Hex, uint64_t &Address) {
  Hex = Hex.trim();
  Hex.consume_front("0x");
  Hex.consume_front("0X");
  if (Hex.empty() || Hex.size() > 16)
    return false;
  return !Hex.getAsInteger(16, Address);
}

void markTraceAddress(BinaryContext &BC, uint64_t Address,
                      const AliasMap &Aliases, RuntimeTraceInfo &Trace) {
  BinaryFunction *BF = BC.getBinaryFunctionContainingAddress(
      Address, /*CheckPastEnd=*/false, /*UseMaxSize=*/true);
  if (!BF)
    return;
  if (BinaryFunction *Canonical = canonicalize(BF, Aliases)) {
    Trace.Executed.insert(Canonical);
    ++Trace.Counts[Canonical];
  }
}

RuntimeTraceInfo loadRuntimeTrace(BinaryContext &BC, const AliasMap &Aliases) {
  RuntimeTraceInfo Trace;
  if (opts::RISCVElimRuntimeTrace.empty())
    return Trace;

  ErrorOr<std::unique_ptr<MemoryBuffer>> BufferOrErr =
      MemoryBuffer::getFile(opts::RISCVElimRuntimeTrace);
  if (!BufferOrErr) {
    errs() << "BOLT-WARNING: cannot read RISC-V runtime trace "
           << opts::RISCVElimRuntimeTrace << ": "
           << BufferOrErr.getError().message() << "\n";
    return Trace;
  }

  std::unique_ptr<MemoryBuffer> Buffer = std::move(*BufferOrErr);
  SmallVector<StringRef, 4> Parts;

  for (StringRef Line : llvm::make_range(line_iterator(*Buffer, false),
                                         line_iterator())) {
    size_t Pos = 0;
    while ((Pos = Line.find('[', Pos)) != StringRef::npos) {
      const size_t End = Line.find(']', Pos + 1);
      if (End == StringRef::npos)
        break;

      StringRef Token = Line.slice(Pos + 1, End);
      uint64_t Address = 0;
      if (Token.contains('/')) {
        Parts.clear();
        Token.split(Parts, '/');
        if (Parts.size() >= 2 && parseHexAddress(Parts[1], Address)) {
          markTraceAddress(BC, Address, Aliases, Trace);
          ++Trace.NumAddresses;
        }
      } else if (parseHexAddress(Token, Address)) {
        markTraceAddress(BC, Address, Aliases, Trace);
        ++Trace.NumAddresses;
      }

      Pos = End + 1;
    }
  }

  outs() << "BOLT-INFO: riscv-elim-unused-funcs loaded "
         << Trace.Executed.size() << " executed functions from "
         << Trace.NumAddresses
         << " trace addresses\n";
  return Trace;
}

} // namespace

void RISCVElimUnusedFuncs::runOnFunctions(BinaryContext &BC) {
  AliasMap Aliases = computeOverlappingAliases(BC);
  EdgeMap Edges;
  FunctionSet Roots;
  FunctionSet Protected;
  FunctionSet Live;
  SmallVector<BinaryFunction *, 32> Worklist;
  RuntimeTraceInfo Trace = loadRuntimeTrace(BC, Aliases);
  const bool UseRuntimeTrace = !Trace.Executed.empty();
  const bool UseOldTextOnly =
      UseRuntimeTrace && opts::RISCVElimUseOldTextOnly;

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
    if (UseRuntimeTrace && !canClearBody(BF, /*RequireExternalRefRelocs=*/false))
      Protected.insert(&BF);
    else if (!UseRuntimeTrace && isConservativeRoot(BF))
      addRoot(&BF, /*Protect=*/true);
  }

  if (UseRuntimeTrace) {
    if (UseOldTextOnly) {
      for (auto &Entry : Trace.Counts) {
        if (Entry.second > opts::RISCVElimUseOldTextMaxTraceCount)
          Roots.insert(Entry.first);
      }
    } else {
      for (BinaryFunction *BF : Trace.Executed)
        Roots.insert(BF);
    }
  } else {
    collectInstructionEdges(BC, Edges, Aliases);
    collectRelocationEdgesAndRoots(BC, Edges, Roots, Protected, Aliases);
  }

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
    if (!canClearBody(BF, /*RequireExternalRefRelocs=*/!UseRuntimeTrace))
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
           << (UseOldTextOnly ? " using original .text only"
                              : (UseRuntimeTrace ? " using runtime trace" : ""))
           << (UseOldTextOnly ? Twine(" (max trace count ") +
                                     Twine(opts::RISCVElimUseOldTextMaxTraceCount) +
                                     Twine(")")
                               : Twine(""))
           << "\n";
  }
}

} // namespace bolt
} // namespace llvm
