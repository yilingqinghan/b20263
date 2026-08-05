#include "bolt/Passes/RISCV/RISCVGPRelax.h"
#include "MCTargetDesc/RISCVMCExpr.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryData.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"
#include <iterator>

#define DEBUG_TYPE "riscv-gp-relax"

using namespace llvm;

namespace opts {
extern cl::OptionCategory BoltOptCategory;

static cl::opt<bool>
    RISCVGPRelaxOpt("riscv-gp-relax",
                    cl::desc("relax RISC-V absolute HI20/LO12 pairs to gp-relative addressing"),
                    cl::init(true), cl::cat(BoltOptCategory));
} // namespace opts

namespace llvm {
namespace bolt {

namespace {

struct SymbolAddend {
  const MCSymbol *Symbol = nullptr;
  int64_t Addend = 0;
};

static bool isLUI(const MCInst &Inst) { return Inst.getOpcode() == RISCV::LUI; }

static bool isAbsoluteHI(const MCOperand &Op) {
  if (!Op.isExpr())
    return false;
  const auto *RV = dyn_cast<RISCVMCExpr>(Op.getExpr());
  return RV && RV->getKind() == RISCVMCExpr::VK_RISCV_HI;
}

static bool isAbsoluteLO(const MCOperand &Op) {
  if (!Op.isExpr())
    return false;
  const auto *RV = dyn_cast<RISCVMCExpr>(Op.getExpr());
  return RV && RV->getKind() == RISCVMCExpr::VK_RISCV_LO;
}

static bool extractSymbolAddend(const MCExpr *Expr, SymbolAddend &Out,
                                int64_t Scale = 1) {
  if (const auto *RV = dyn_cast<RISCVMCExpr>(Expr))
    return extractSymbolAddend(RV->getSubExpr(), Out, Scale);

  if (const auto *Sym = dyn_cast<MCSymbolRefExpr>(Expr)) {
    if (Out.Symbol && Out.Symbol != &Sym->getSymbol())
      return false;
    Out.Symbol = &Sym->getSymbol();
    return true;
  }

  if (const auto *Const = dyn_cast<MCConstantExpr>(Expr)) {
    Out.Addend += Scale * Const->getValue();
    return true;
  }

  if (const auto *Bin = dyn_cast<MCBinaryExpr>(Expr)) {
    switch (Bin->getOpcode()) {
    case MCBinaryExpr::Add:
      return extractSymbolAddend(Bin->getLHS(), Out, Scale) &&
             extractSymbolAddend(Bin->getRHS(), Out, Scale);
    case MCBinaryExpr::Sub:
      return extractSymbolAddend(Bin->getLHS(), Out, Scale) &&
             extractSymbolAddend(Bin->getRHS(), Out, -Scale);
    default:
      return false;
    }
  }

  return false;
}

static bool getRISCVExprSymbolAddend(const MCOperand &Op, SymbolAddend &Out) {
  if (!Op.isExpr())
    return false;
  Out = SymbolAddend();
  return extractSymbolAddend(Op.getExpr(), Out) && Out.Symbol;
}

static bool isIFormatGPSink(const MCInst &Inst) {
  switch (Inst.getOpcode()) {
  default:
    return false;
  case RISCV::ADDI:
  case RISCV::LB:
  case RISCV::LBU:
  case RISCV::LH:
  case RISCV::LHU:
  case RISCV::LW:
  case RISCV::LWU:
  case RISCV::LD:
  case RISCV::FLW:
  case RISCV::FLD:
    return Inst.getNumOperands() >= 3;
  }
}

static bool isSFormatGPSink(const MCInst &Inst) {
  switch (Inst.getOpcode()) {
  default:
    return false;
  case RISCV::SB:
  case RISCV::SH:
  case RISCV::SW:
  case RISCV::SD:
  case RISCV::FSW:
  case RISCV::FSD:
    return Inst.getNumOperands() >= 3;
  }
}

static bool isGPSink(const MCInst &Inst) {
  return isIFormatGPSink(Inst) || isSFormatGPSink(Inst);
}

static bool definesReg(const MCInst &Inst, MCPhysReg Reg) {
  return isIFormatGPSink(Inst) && Inst.getOperand(0).isReg() &&
         Inst.getOperand(0).getReg() == Reg;
}

static bool readsReg(const MCInst &Inst, MCPhysReg Reg,
                     const MCInstrInfo &MCII) {
  const MCInstrDesc &Desc = MCII.get(Inst.getOpcode());
  const unsigned NumDefs =
      std::min<unsigned>(Desc.getNumDefs(), Inst.getNumOperands());

  for (unsigned I = NumDefs, E = Inst.getNumOperands(); I != E; ++I)
    if (Inst.getOperand(I).isReg() && Inst.getOperand(I).getReg() == Reg)
      return true;

  return is_contained(Desc.implicit_uses(), Reg);
}

static bool writesReg(const MCInst &Inst, MCPhysReg Reg,
                      const MCInstrInfo &MCII) {
  const MCInstrDesc &Desc = MCII.get(Inst.getOpcode());
  const unsigned NumDefs =
      std::min<unsigned>(Desc.getNumDefs(), Inst.getNumOperands());

  for (unsigned I = 0; I != NumDefs; ++I)
    if (Inst.getOperand(I).isReg() && Inst.getOperand(I).getReg() == Reg)
      return true;

  return is_contained(Desc.implicit_defs(), Reg);
}

static bool isRegRedefinedBeforeUse(BinaryBasicBlock::iterator Begin,
                                    BinaryBasicBlock::iterator End,
                                    MCPhysReg Reg, const MCInstrInfo &MCII) {
  for (auto It = Begin; It != End; ++It) {
    // A read-modify-write instruction still consumes the old LUI value.
    if (readsReg(*It, Reg, MCII))
      return false;
    if (writesReg(*It, Reg, MCII))
      return true;
  }
  return false;
}

static MCInst makeGPRelaxedInst(const MCInst &Inst, int64_t GPOffset) {
  MCInst NewInst = Inst;
  NewInst.getOperand(1) = MCOperand::createReg(RISCV::X3);
  NewInst.getOperand(2) = MCOperand::createImm(GPOffset);
  return NewInst;
}

static const BinaryData *getGlobalPointerData(BinaryContext &BC) {
  if (const BinaryData *BD = BC.getBinaryDataByName("__global_pointer$"))
    return BD;
  if (const BinaryData *BD = BC.getBinaryDataByName("__global_pointer$/1"))
    return BD;

  for (const auto &It : BC.getBinaryData()) {
    const BinaryData *BD = It.second;
    if (BD->getName().startswith("__global_pointer$"))
      return BD;
  }
  return nullptr;
}

} // namespace

void RISCVGPRelax::runOnFunction(BinaryFunction &BF) {
  BinaryContext &BC = BF.getBinaryContext();

  const BinaryData *GPData = getGlobalPointerData(BC);
  if (!GPData)
    return;
  const uint64_t GP = GPData->getAddress();
  const MCInstrInfo &MCII = *BC.MII;

  for (BinaryBasicBlock &BB : BF) {
    for (auto II = BB.begin(); II != BB.end();) {
      auto Next = std::next(II);
      if (Next == BB.end() || !isLUI(*II)) {
        ++II;
        continue;
      }

      if (II->getNumOperands() < 2 || !isAbsoluteHI(II->getOperand(1)) ||
          !II->getOperand(0).isReg()) {
        ++II;
        continue;
      }

      const MCPhysReg LUIReg = II->getOperand(0).getReg();
      SymbolAddend HiRef;
      if (!getRISCVExprSymbolAddend(II->getOperand(1), HiRef)) {
        ++II;
        continue;
      }

      SmallVector<std::pair<BinaryBasicBlock::iterator, MCInst>, 4> Relaxed;
      bool KillsBaseReg = false;
      auto Scan = Next;
      for (; Scan != BB.end(); ++Scan) {
        if (!isGPSink(*Scan) || !Scan->getOperand(1).isReg() ||
            Scan->getOperand(1).getReg() != LUIReg ||
            !isAbsoluteLO(Scan->getOperand(2)))
          break;

        // For stores, operand 0 is the value being stored. If it is the same
        // register as the address materialization, deleting LUI changes dataflow.
        if (isSFormatGPSink(*Scan) && Scan->getOperand(0).isReg() &&
            Scan->getOperand(0).getReg() == LUIReg) {
          Relaxed.clear();
          break;
        }

        SymbolAddend LoRef;
        if (!getRISCVExprSymbolAddend(Scan->getOperand(2), LoRef) ||
            HiRef.Symbol != LoRef.Symbol) {
          break;
        }

        ErrorOr<uint64_t> SymbolValue = BC.getSymbolValue(*LoRef.Symbol);
        if (!SymbolValue)
          break;

        const int64_t Target =
            static_cast<int64_t>(*SymbolValue) + LoRef.Addend;
        const int64_t GPOffset = Target - static_cast<int64_t>(GP);
        if (!isInt<12>(GPOffset))
          break;

        Relaxed.push_back({Scan, makeGPRelaxedInst(*Scan, GPOffset)});
        if (definesReg(*Scan, LUIReg)) {
          KillsBaseReg = true;
          ++Scan;
          break;
        }
      }

      // If the LUI result remains live after the converted memory/address
      // operations, deleting it would change following code. Besides sinks that
      // overwrite the base register directly, allow cases where the register is
      // redefined later in the same block before any read.
      if (Relaxed.empty() ||
          (!KillsBaseReg &&
           !isRegRedefinedBeforeUse(Scan, BB.end(), LUIReg, MCII))) {
        ++II;
        continue;
      }

      for (auto &Entry : Relaxed)
        *Entry.first = Entry.second;
      II = BB.eraseInstruction(II);
    }
  }
}

void RISCVGPRelax::runOnFunctions(BinaryContext &BC) {
  if (!opts::RISCVGPRelaxOpt)
    return;

  uint64_t Changed = 0;
  for (auto &BFIt : BC.getBinaryFunctions()) {
    const uint64_t Before = BFIt.second.getNumNonPseudos();
    runOnFunction(BFIt.second);
    const uint64_t After = BFIt.second.getNumNonPseudos();
    if (Before > After)
      Changed += Before - After;
  }

  if (opts::Verbosity > 0 && Changed)
    outs() << "BOLT-INFO: RISC-V GP relaxed " << Changed
           << " instruction pairs\n";
}

} // namespace bolt
} // namespace llvm
