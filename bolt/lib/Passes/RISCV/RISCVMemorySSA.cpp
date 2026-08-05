#include "MCTargetDesc/RISCVMCExpr.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Passes/RISCV/RISCVMemorySSA.h"
#include "llvm/MC/MCInst.h"
#include <iterator>

using namespace llvm;

namespace llvm {
namespace bolt {

static bool isSW(const MCInst &I) { return I.getOpcode() == RISCV::SW; }
static bool isLW(const MCInst &I) { return I.getOpcode() == RISCV::LW; }

void RISCVMemorySSA::runOnFunction(BinaryFunction &BF) {
  // Only forward an immediately following full-word load. Keeping the store
  // makes the replacement valid even when the stack slot is read later.
  for (auto &BB : BF) {
    for (auto Store = BB.begin(); Store != BB.end(); ++Store) {
      auto Load = std::next(Store);
      if (Load == BB.end() || !isSW(*Store) || !isLW(*Load))
        continue;

      if (Store->getNumOperands() < 3 || Load->getNumOperands() < 3 ||
          !Store->getOperand(0).isReg() || !Store->getOperand(1).isReg() ||
          !Store->getOperand(2).isImm() || !Load->getOperand(0).isReg() ||
          !Load->getOperand(1).isReg() || !Load->getOperand(2).isImm())
        continue;

      const MCPhysReg Source = Store->getOperand(0).getReg();
      const MCPhysReg Base = Store->getOperand(1).getReg();
      const MCPhysReg Dest = Load->getOperand(0).getReg();
      if (Base != Load->getOperand(1).getReg() ||
          Store->getOperand(2).getImm() != Load->getOperand(2).getImm())
        continue;

      if (Dest == RISCV::X0 || Dest == Source) {
        BB.eraseInstruction(Load);
        continue;
      }
      if (Source == RISCV::X0)
        continue;

      MCInst Move;
      Move.setOpcode(RISCV::C_MV);
      Move.addOperand(MCOperand::createReg(Dest));
      Move.addOperand(MCOperand::createReg(Source));
      BB.replaceInstruction(Load, {Move});
    }
  }
}



void RISCVMemorySSA::runOnFunctions(BinaryContext &BC) {
  for (auto &BFIt : BC.getBinaryFunctions()) {
    runOnFunction(BFIt.second);
  }
}


} // namespace bolt
} // namespace llvm
