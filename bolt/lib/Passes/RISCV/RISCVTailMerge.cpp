#include "bolt/Passes/RISCV/RISCVTailMerge.h"
#include "bolt/Core/BinaryBasicBlock.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/FunctionLayout.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <string>

#define DEBUG_TYPE "riscv-tail-merge"

using namespace llvm;

namespace opts {
extern cl::OptionCategory BoltOptCategory;

static cl::opt<bool> RISCVTailMergeOpt(
    "riscv-tail-merge",
    cl::desc("merge identical terminal RISC-V epilogue suffixes"),
    cl::init(true), cl::cat(BoltOptCategory));

static cl::opt<unsigned> RISCVTailMergeMaxSuffix(
    "riscv-tail-merge-max-suffix",
    cl::desc("maximum number of instructions in a RISC-V tail-merge suffix"),
    cl::init(12), cl::cat(BoltOptCategory));
} // namespace opts

namespace llvm {
namespace bolt {

namespace {

static bool appendOperandKey(raw_ostream &OS, const MCOperand &Op) {
  if (Op.isReg()) {
    OS << 'r' << Op.getReg();
    return true;
  }
  if (Op.isImm()) {
    OS << 'i' << Op.getImm();
    return true;
  }
  if (Op.isSFPImm()) {
    OS << 's' << Op.getSFPImm();
    return true;
  }
  if (Op.isDFPImm()) {
    OS << 'd' << Op.getDFPImm();
    return true;
  }

  // Avoid merging code whose semantics depends on symbolic operands or nested
  // MCInst operands. Return epilogues normally do not need either.
  return false;
}

static bool appendInstructionKey(raw_ostream &OS, const MCInst &Inst) {
  OS << Inst.getOpcode() << ':' << Inst.getFlags() << ':'
     << Inst.getNumOperands();
  for (const MCOperand &Op : Inst) {
    OS << ':';
    if (!appendOperandKey(OS, Op))
      return false;
  }
  OS << ';';
  return true;
}

static bool isTailMergeCandidate(const BinaryBasicBlock &BB,
                                 const BinaryContext &BC) {
  if (!BB.isValid() || BB.empty() || !BB.succ_empty())
    return false;
  if (BB.isEntryPoint() || BB.isLandingPad() || BB.hasJumpTable())
    return false;

  const MCInst *Last = BB.getLastNonPseudoInstr();
  if (!Last || !BC.MIB->isReturn(*Last))
    return false;

  for (const MCInst &Inst : BB) {
    if (BC.MIB->isPseudo(Inst))
      return false;
    if (&Inst != Last && BC.MIB->isTerminator(Inst))
      return false;
  }

  return true;
}

static unsigned countNonPseudo(const BinaryBasicBlock &BB,
                               const BinaryContext &BC) {
  unsigned Count = 0;
  for (const MCInst &Inst : BB)
    if (!BC.MIB->isPseudo(Inst))
      ++Count;
  return Count;
}

static BinaryBasicBlock::iterator getSuffixBegin(BinaryBasicBlock &BB,
                                                 unsigned SuffixLen) {
  auto It = BB.end();
  for (unsigned I = 0; I != SuffixLen; ++I)
    --It;
  return It;
}

static BinaryBasicBlock::const_iterator
getSuffixBegin(const BinaryBasicBlock &BB, unsigned SuffixLen) {
  auto It = BB.end();
  for (unsigned I = 0; I != SuffixLen; ++I)
    --It;
  return It;
}

static bool getSuffixKey(const BinaryBasicBlock &BB, const BinaryContext &BC,
                         unsigned SuffixLen, std::string &Key,
                         uint64_t &SuffixSize) {
  if (!isTailMergeCandidate(BB, BC) || countNonPseudo(BB, BC) < SuffixLen)
    return false;

  const MCInst *Last = BB.getLastNonPseudoInstr();
  std::string Storage;
  raw_string_ostream OS(Storage);
  OS << (BB.isCold() ? "cold;" : "hot;");
  SuffixSize = 0;

  for (auto It = getSuffixBegin(BB, SuffixLen); It != BB.end(); ++It) {
    const MCInst &Inst = *It;
    if (BC.MIB->isPseudo(Inst) || BC.MIB->isCall(Inst))
      return false;
    if (&Inst != Last && BC.MIB->isTerminator(Inst))
      return false;
    if (!appendInstructionKey(OS, Inst))
      return false;
    SuffixSize += BC.computeInstructionSize(Inst);
  }

  Key = OS.str();
  return true;
}

static uint64_t getBranchSize(BinaryContext &BC, const MCSymbol *Target) {
  MCInst Branch;
  {
    auto L = BC.scopeLock();
    BC.MIB->createUncondBranch(Branch, Target, BC.Ctx.get());
  }
  return BC.computeInstructionSize(Branch);
}

static BinaryBasicBlock *
chooseFallthroughSource(ArrayRef<BinaryBasicBlock *> Blocks) {
  BinaryBasicBlock *Best = Blocks.front();
  for (BinaryBasicBlock *BB : Blocks)
    if (BB->getKnownExecutionCount() > Best->getKnownExecutionCount())
      Best = BB;
  return Best;
}

static uint64_t sumKnownExecutionCount(ArrayRef<BinaryBasicBlock *> Blocks) {
  uint64_t Count = 0;
  for (const BinaryBasicBlock *BB : Blocks)
    Count += BB->getKnownExecutionCount();
  return Count;
}

static void eraseSuffix(BinaryBasicBlock &BB, unsigned SuffixLen) {
  for (unsigned I = 0; I != SuffixLen; ++I)
    BB.eraseInstruction(std::prev(BB.end()));
}

} // namespace

uint64_t RISCVTailMerge::runOnFunction(BinaryFunction &BF) {
  BinaryContext &BC = BF.getBinaryContext();
  BF.getLayout().updateLayoutIndices();

  SmallVector<BinaryBasicBlock *, 32> Candidates;
  unsigned MaxBlockInstrs = 0;
  for (BinaryBasicBlock &BB : BF) {
    if (!isTailMergeCandidate(BB, BC))
      continue;
    Candidates.push_back(&BB);
    MaxBlockInstrs = std::max(MaxBlockInstrs, countNonPseudo(BB, BC));
  }

  if (Candidates.size() < 2)
    return 0;

  SmallPtrSet<BinaryBasicBlock *, 32> Used;
  uint64_t MergedSources = 0;
  const unsigned MaxSuffixLen =
      std::min(MaxBlockInstrs, opts::RISCVTailMergeMaxSuffix.getValue());

  for (unsigned SuffixLen = MaxSuffixLen; SuffixLen >= 2; --SuffixLen) {
    std::map<std::string, SmallVector<BinaryBasicBlock *, 4>> Groups;
    std::map<std::string, uint64_t> SuffixSizes;

    for (BinaryBasicBlock *BB : Candidates) {
      if (Used.contains(BB))
        continue;

      std::string Key;
      uint64_t SuffixSize = 0;
      if (!getSuffixKey(*BB, BC, SuffixLen, Key, SuffixSize))
        continue;

      Groups[Key].push_back(BB);
      SuffixSizes[Key] = SuffixSize;
    }

    for (auto &Entry : Groups) {
      SmallVector<BinaryBasicBlock *, 4> &Blocks = Entry.second;
      if (Blocks.size() < 2)
        continue;

      const uint64_t SuffixSize = SuffixSizes[Entry.first];
      if (SuffixSize == 0)
        continue;

      std::vector<std::unique_ptr<BinaryBasicBlock>> NewBlocks;
      NewBlocks.emplace_back(
          BF.createBasicBlock(BC.Ctx->createNamedTempSymbol("tail-merge")));
      BinaryBasicBlock *Tail = NewBlocks.back().get();
      Tail->addInstructions(getSuffixBegin(*Blocks.front(), SuffixLen),
                            Blocks.front()->end());
      Tail->setExecutionCount(sumKnownExecutionCount(Blocks));
      Tail->setIsCold(Blocks.front()->isCold());

      const uint64_t BranchSize = getBranchSize(BC, Tail->getLabel());
      if (SuffixSize <= BranchSize)
        continue;

      BinaryBasicBlock *FallthroughSource = chooseFallthroughSource(Blocks);
      for (BinaryBasicBlock *BB : Blocks) {
        eraseSuffix(*BB, SuffixLen);
        BB->addSuccessor(Tail, BB->getKnownExecutionCount(), 0);
        Used.insert(BB);
      }

      BF.insertBasicBlocks(FallthroughSource, std::move(NewBlocks));
      MergedSources += Blocks.size();
    }
  }

  if (MergedSources)
    BF.fixBranches();

  return MergedSources;
}

void RISCVTailMerge::runOnFunctions(BinaryContext &BC) {
  if (!opts::RISCVTailMergeOpt)
    return;

  uint64_t MergedSources = 0;
  uint64_t ModifiedFunctions = 0;
  for (auto &BFIt : BC.getBinaryFunctions()) {
    BinaryFunction &BF = BFIt.second;
    if (!shouldOptimize(BF))
      continue;

    const uint64_t FunctionMergedSources = runOnFunction(BF);
    MergedSources += FunctionMergedSources;
    ModifiedFunctions += FunctionMergedSources != 0;
  }

  if (opts::Verbosity > 0 && MergedSources)
    outs() << "BOLT-INFO: RISC-V tail-merged " << MergedSources
           << " epilogue sources in " << ModifiedFunctions << " functions\n";
}

} // namespace bolt
} // namespace llvm
