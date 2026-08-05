#include "bolt/Passes/RISCV/RISCVCallSequenceOutliner.h"
#include "bolt/Core/BinaryBasicBlock.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Passes/BinaryFunctionCallGraph.h"
#include "bolt/Passes/RegAnalysis.h"
#include "bolt/Passes/CallGraph.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"
#include <map>
#include <string>
#include <vector>

#define DEBUG_TYPE "riscv-call-sequence-outliner"

using namespace llvm;

namespace opts {
extern cl::OptionCategory BoltOptCategory;

static cl::opt<bool> RISCVCallSequenceOutlinerOpt(
    "riscv-call-sequence-outliner",
    cl::desc("outline repeated RISC-V direct-call sequences"), cl::init(true),
    cl::cat(BoltOptCategory));

static cl::opt<unsigned> RISCVCallSequenceOutlinerMinCalls(
    "riscv-call-sequence-outliner-min-calls",
    cl::desc("minimum number of calls in an outlineable RISC-V sequence"),
    cl::init(4), cl::cat(BoltOptCategory));
} // namespace opts

namespace llvm {
namespace bolt {

namespace {

struct CallUnit {
  BinaryBasicBlock::iterator LUI;
  BinaryBasicBlock::iterator ADDI;
  BinaryBasicBlock::iterator Call;
  const MCSymbol *Target = nullptr;
  MCPhysReg Temp = RISCV::NoRegister;
  unsigned LUIOpcode = 0;
  int64_t HighValue = 0;
  int64_t LowValue = 0;
};

struct CallRun {
  BinaryFunction *Function = nullptr;
  BinaryBasicBlock *BB = nullptr;
  size_t StartIndex = 0;
  std::vector<CallUnit> Units;
};

static bool getLUIValue(const MCInst &Inst, int64_t &Value) {
  if ((Inst.getOpcode() != RISCV::LUI && Inst.getOpcode() != RISCV::C_LUI) ||
      Inst.getNumOperands() < 2 || !Inst.getOperand(0).isReg() ||
      !Inst.getOperand(1).isImm())
    return false;

  const int64_t Imm = Inst.getOperand(1).getImm();
  if (Imm == 0)
    return false;
  if (Inst.getOpcode() == RISCV::LUI &&
      (Imm < -(1 << 19) || Imm > (1 << 20) - 1))
    return false;
  if (Inst.getOpcode() == RISCV::C_LUI && (Imm < -32 || Imm > 31))
    return false;

  Value = SignExtend64<32>(static_cast<uint64_t>(Imm & 0xfffff) << 12);
  return true;
}

static bool isCallerSavedGPR(MCPhysReg Reg) {
  switch (Reg) {
  default:
    return false;
  case RISCV::X5:
  case RISCV::X6:
  case RISCV::X7:
  case RISCV::X10:
  case RISCV::X11:
  case RISCV::X12:
  case RISCV::X13:
  case RISCV::X14:
  case RISCV::X15:
  case RISCV::X16:
  case RISCV::X17:
  case RISCV::X28:
  case RISCV::X29:
  case RISCV::X30:
  case RISCV::X31:
    return true;
  }
}

static bool parseUnit(BinaryContext &BC, BinaryBasicBlock &BB,
                      BinaryBasicBlock::iterator It, CallUnit &Unit) {
  auto ADDIIt = std::next(It);
  auto CallIt = ADDIIt == BB.end() ? BB.end() : std::next(ADDIIt);
  if (ADDIIt == BB.end() || CallIt == BB.end())
    return false;

  int64_t HighValue = 0;
  if (!getLUIValue(*It, HighValue) ||
      It->getOperand(0).getReg() == RISCV::X0)
    return false;

  if (ADDIIt->getOpcode() != RISCV::ADDI || ADDIIt->getNumOperands() < 3 ||
      !ADDIIt->getOperand(0).isReg() || !ADDIIt->getOperand(1).isReg() ||
      !ADDIIt->getOperand(2).isImm())
    return false;

  const MCPhysReg Temp = It->getOperand(0).getReg();
  if (!isCallerSavedGPR(Temp) || ADDIIt->getOperand(0).getReg() != RISCV::X12 ||
      ADDIIt->getOperand(1).getReg() != Temp ||
      !isInt<12>(ADDIIt->getOperand(2).getImm()))
    return false;

  if (CallIt->getOpcode() != RISCV::JAL || CallIt->getNumOperands() < 2 ||
      !CallIt->getOperand(0).isReg() ||
      CallIt->getOperand(0).getReg() != RISCV::X1)
    return false;

  const MCSymbol *Target = BC.MIB->getTargetSymbol(*CallIt);
  if (!Target)
    return false;

  Unit = {It,
          ADDIIt,
          CallIt,
          Target,
          Temp,
          It->getOpcode(),
          HighValue,
          ADDIIt->getOperand(2).getImm()};
  return true;
}

static bool isOutlineable(const CallRun &Run) {
  if (Run.Units.size() < opts::RISCVCallSequenceOutlinerMinCalls)
    return false;

  const int64_t BaseLow = Run.Units.front().LowValue;
  for (const CallUnit &Unit : Run.Units) {
    if (Unit.HighValue != Run.Units.front().HighValue ||
        !isInt<12>(Unit.LowValue - BaseLow))
      return false;
  }
  return true;
}

static bool startsAfterUnit(BinaryContext &BC, BinaryBasicBlock &BB,
                            BinaryBasicBlock::iterator It) {
  if (It == BB.begin())
    return false;

  auto CallIt = It;
  --CallIt;
  if (CallIt == BB.begin())
    return false;
  auto ADDIIt = CallIt;
  --ADDIIt;
  if (ADDIIt == BB.begin())
    return false;
  auto LUIIt = ADDIIt;
  --LUIIt;

  CallUnit Previous;
  return parseUnit(BC, BB, LUIIt, Previous) && Previous.Call == CallIt;
}

static std::string getRunKey(const CallRun &Run) {
  std::string Key;
  raw_string_ostream OS(Key);
  const int64_t BaseLow = Run.Units.front().LowValue;
  OS << Run.Units.front().HighValue << ';';
  for (const CallUnit &Unit : Run.Units)
    OS << Unit.Target->getName() << ':' << Unit.Temp << ':' << Unit.LUIOpcode
       << ':' << (Unit.LowValue - BaseLow) << ';';
  return OS.str();
}

static MCInst makeADDI(MCPhysReg Dst, MCPhysReg Src, int64_t Imm) {
  MCInst Inst;
  Inst.setOpcode(RISCV::ADDI);
  Inst.addOperand(MCOperand::createReg(Dst));
  Inst.addOperand(MCOperand::createReg(Src));
  Inst.addOperand(MCOperand::createImm(Imm));
  return Inst;
}

static MCInst makeMove(MCPhysReg Dst, MCPhysReg Src) {
  return makeADDI(Dst, Src, 0);
}

static MCInst makeDirectCall(const MCSymbol *Target, MCContext &Ctx) {
  MCInst Inst;
  Inst.setOpcode(RISCV::JAL);
  Inst.addOperand(MCOperand::createReg(RISCV::X1));
  Inst.addOperand(MCOperand::createExpr(
      MCSymbolRefExpr::create(Target, MCSymbolRefExpr::VK_None, Ctx)));
  return Inst;
}

static MCInst makeIndirectReturn(MCPhysReg ReturnRegister) {
  MCInst Inst;
  Inst.setOpcode(RISCV::JALR);
  Inst.addOperand(MCOperand::createReg(RISCV::X0));
  Inst.addOperand(MCOperand::createReg(ReturnRegister));
  Inst.addOperand(MCOperand::createImm(0));
  return Inst;
}

struct HelperRegisters {
  MCPhysReg Base;
  MCPhysReg Return;
};

static std::optional<HelperRegisters>
chooseHelperRegisters(BinaryContext &BC, const CallRun &Run, RegAnalysis &RA) {
  // The helper must leave the caller's stack pointer unchanged. Keep the
  // first address in a caller-saved register that every outlined callee leaves
  // untouched. Unknown or externally visible callees are rejected by the
  // register analysis rather than relying on an ABI guess.
  static constexpr MCPhysReg Candidates[] = {
      RISCV::X5,  RISCV::X6,  RISCV::X7,  RISCV::X28,
      RISCV::X29, RISCV::X30, RISCV::X31,
  };

  for (const MCPhysReg Base : Candidates) {
    for (const MCPhysReg Return : Candidates) {
      if (Base == Return)
        continue;

      bool UsedAsMaterializationTemp = false;
      for (const CallUnit &Unit : Run.Units)
        UsedAsMaterializationTemp |=
            Unit.Temp == Base || Unit.Temp == Return;
      if (UsedAsMaterializationTemp)
        continue;

      bool Safe = true;
      for (const CallUnit &Unit : Run.Units) {
        const BinaryFunction *Target =
            BC.getFunctionForSymbol(Unit.Target);
        if (!Target || RA.getFunctionClobberList(Target).test(Base) ||
            RA.getFunctionClobberList(Target).test(Return)) {
          Safe = false;
          break;
        }
      }
      if (Safe)
        return HelperRegisters{Base, Return};
    }
  }
  return std::nullopt;
}

static void addHelperBody(BinaryContext &BC, BinaryFunction &Helper,
                          const CallRun &Run, HelperRegisters Registers) {
  BinaryBasicBlock *BB = Helper.addBasicBlock();
  BB->setCFIState(0);

  // Do not change sp: the outlined callees must observe exactly the same
  // stack pointer as they did at the original call site.
  BB->addInstruction(makeMove(Registers.Base, RISCV::X12));
  BB->addInstruction(makeMove(Registers.Return, RISCV::X1));
  const int64_t BaseLow = Run.Units.front().LowValue;
  for (size_t I = 0; I != Run.Units.size(); ++I) {
    const CallUnit &Unit = Run.Units[I];
    if (I != 0) {
      const int64_t Delta = Unit.LowValue - BaseLow;
      if (Unit.Temp != RISCV::X12) {
        MCInst LUI = *Unit.LUI;
        BB->addInstruction(LUI);
      }
      BB->addInstruction(makeADDI(RISCV::X12, Registers.Base, Delta));
    }

    BB->addInstruction(*Unit.Call);
  }

  BB->addInstruction(makeIndirectReturn(Registers.Return));
}

static uint64_t replaceRun(BinaryContext &BC, CallRun &Run,
                           const MCSymbol *HelperSymbol) {
  BinaryBasicBlock &BB = *Run.BB;
  *Run.Units.front().Call = makeDirectCall(HelperSymbol, *BC.Ctx);

  for (size_t I = Run.Units.size(); I-- > 1;) {
    BB.eraseInstruction(Run.Units[I].Call);
    BB.eraseInstruction(Run.Units[I].ADDI);
    BB.eraseInstruction(Run.Units[I].LUI);
  }
  return Run.Units.size() - 1;
}

} // namespace

uint64_t RISCVCallSequenceOutliner::runOnFunction(BinaryFunction &BF) {
  // Collection is performed globally so identical sequences from different
  // basic blocks and functions can share one injected helper.
  (void)BF;
  return 0;
}

void RISCVCallSequenceOutliner::runOnFunctions(BinaryContext &BC) {
  if (!opts::RISCVCallSequenceOutlinerOpt || !BC.isRISCV32())
    return;

  BinaryFunctionCallGraph CG = buildCallGraph(BC);
  RegAnalysis RA(BC, &BC.getBinaryFunctions(), &CG);

  std::map<std::string, std::vector<CallRun>> Groups;
  for (auto &BFIt : BC.getBinaryFunctions()) {
    BinaryFunction &BF = BFIt.second;
    for (BinaryBasicBlock &BB : BF) {
      for (auto It = BB.begin(); It != BB.end(); ++It) {
        // Scanning every instruction also finds all suffixes of a maximal run.
        // Keep only the maximal run so later edits cannot invalidate another
        // candidate's iterators.
        if (startsAfterUnit(BC, BB, It))
          continue;

        CallRun Run;
        Run.Function = &BF;
        Run.BB = &BB;
        Run.StartIndex = std::distance(BB.begin(), It);
        auto Scan = It;
        while (Scan != BB.end()) {
          CallUnit Unit;
          if (!parseUnit(BC, BB, Scan, Unit))
            break;
          Run.Units.push_back(Unit);
          Scan = std::next(Unit.Call);
        }

        if (isOutlineable(Run))
          Groups[getRunKey(Run)].push_back(std::move(Run));
      }
    }
  }

  uint64_t OutlinedRuns = 0;
  uint64_t RemovedCalls = 0;
  unsigned HelperIndex = 0;
  struct OutlineWork {
    CallRun *Run;
    const MCSymbol *HelperSymbol;
  };
  std::vector<OutlineWork> Work;

  for (auto &Entry : Groups) {
    std::vector<CallRun> &Runs = Entry.second;
    if (Runs.size() < 2)
      continue;

    const std::string Name =
        "__bolt_riscv_call_sequence_" + std::to_string(HelperIndex++);
    std::optional<HelperRegisters> Registers =
        chooseHelperRegisters(BC, Runs.front(), RA);
    if (!Registers)
      continue;

    BinaryFunction *Helper = BC.createInjectedBinaryFunction(Name, true);
    addHelperBody(BC, *Helper, Runs.front(), *Registers);
    for (CallRun &Run : Runs)
      Work.push_back({&Run, Helper->getSymbol()});
  }

  // Instructions are stored in vectors, so erasing from a block invalidates
  // iterators at and after the erase point. Rewrite from the end of each block
  // towards the beginning, including candidates from different groups.
  llvm::stable_sort(Work, [](const OutlineWork &A, const OutlineWork &B) {
    if (A.Run->BB != B.Run->BB)
      return std::less<BinaryBasicBlock *>()(A.Run->BB, B.Run->BB);
    return A.Run->StartIndex > B.Run->StartIndex;
  });
  for (OutlineWork &Item : Work) {
    RemovedCalls += replaceRun(BC, *Item.Run, Item.HelperSymbol);
    ++OutlinedRuns;
  }

  if (opts::Verbosity > 0 && OutlinedRuns)
    outs() << "BOLT-INFO: RISC-V outlined " << OutlinedRuns
           << " repeated call sequences, removed " << RemovedCalls
           << " duplicated calls\n";
}

} // namespace bolt
} // namespace llvm
