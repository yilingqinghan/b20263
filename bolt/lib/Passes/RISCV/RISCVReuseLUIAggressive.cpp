#include "MCTargetDesc/RISCVMCExpr.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Passes/RISCV/RISCVReuseLUIAggressive.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "riscv-reuse-lui-aggressive"

using namespace llvm;

namespace llvm {
namespace bolt {

static bool isValidInst(const MCInst &I)   { return I.getNumOperands() && I.getOpcode(); }
static bool isLUI(const MCInst &I)         { return isValidInst(I) && I.getOpcode() == RISCV::LUI; }
static bool isADDI(const MCInst &I)        { return isValidInst(I) && I.getOpcode() == RISCV::ADDI; }
static bool isLW_SW(const MCInst &I)       { return I.getOpcode() == RISCV::LW /*|| I.getOpcode() == RISCV::SW*/; }

static bool isHI(const MCOperand &Op) {
  if (!Op.isExpr()) return false;
  auto *RV = dyn_cast<RISCVMCExpr>(Op.getExpr());
  return RV && RV->getKind() == RISCVMCExpr::VK_RISCV_HI;
}

static bool isLO(const MCOperand &Op) {
  if (!Op.isExpr()) return false;
  auto *RV = dyn_cast<RISCVMCExpr>(Op.getExpr());
  return RV && (RV->getKind() == RISCVMCExpr::VK_RISCV_LO ||
                RV->getKind() == RISCVMCExpr::VK_RISCV_PCREL_LO);
}

static const MCSymbolRefExpr *getSym(const MCOperand &Op) {
  if (!Op.isExpr()) return nullptr;
  auto *RV = dyn_cast<RISCVMCExpr>(Op.getExpr());
  if (!RV) return nullptr;
  return dyn_cast<MCSymbolRefExpr>(RV->getSubExpr());
}

// Enhanced chain detection and optimization pass
// Rewritten to handle full LUI+ADDI chains correctly
void RISCVReuseLUIAggressive::runOnFunction(BinaryFunction &BF) {
  auto &BC = BF.getBinaryContext();
  using BBIter = BinaryFunction::iterator;
  using InstIter = BinaryBasicBlock::iterator;

  static std::string LastFuncPrinted;

  auto nextInst = [&](BBIter &BBIt, InstIter &It) -> bool {
    ++It;
    while (BBIt != BF.end() && It == BBIt->end()) {
      ++BBIt;
      if (BBIt == BF.end()) return false;
      It = BBIt->begin();
    }
    return BBIt != BF.end();
  };

  for (auto BBIt = BF.begin(); BBIt != BF.end(); ) {
    for (auto II = BBIt->begin(); BBIt != BF.end() && II != BBIt->end(); ) {
      if (!isLUI(*II) || !isHI(II->getOperand(1))) {
        if (!nextInst(BBIt, II)) return;
        continue;
      }

      const auto *HiSym = getSym(II->getOperand(1));
      if (!HiSym) {
        if (!nextInst(BBIt, II)) return;
        continue;
      }

      uint64_t HiAddr = *BC.getSymbolValue(HiSym->getSymbol());
      MCPhysReg HiReg = II->getOperand(0).getReg();

      auto LuiBB = BBIt;
      auto LuiII = II;
      uint64_t LuiOffset = LuiBB->getInputOffset() + std::distance(LuiBB->begin(), LuiII) * 4;

      auto AddiBB = BBIt;
      auto AddiII = II;
      if (!nextInst(AddiBB, AddiII)) {
        if (!nextInst(BBIt, II)) return;
        continue;
      }

      if (!isADDI(*AddiII) /*&& !isLW_SW(*AddiII)*/) {
        if (!nextInst(BBIt, II)) return;
        continue;
      }

      const auto &LOOp = AddiII->getOperand(2);
      if (!isLO(LOOp)) {
        if (!nextInst(BBIt, II)) return;
        continue;
      }

      const auto *LoSym = getSym(LOOp);
      if (!LoSym || LoSym->getSymbol().getName() != HiSym->getSymbol().getName()) {
        if (!nextInst(BBIt, II)) return;
        continue;
      }

      uint64_t LoAddr = *BC.getSymbolValue(LoSym->getSymbol());
      int64_t BaseVal = LoAddr;
      MCPhysReg CurrReg = AddiII->getOperand(0).getReg();

      std::string FuncName = std::string(BF.getOneName());
      if (FuncName != LastFuncPrinted) {
        LastFuncPrinted = FuncName;
        LLVM_DEBUG(outs() << "\n>>> Function: " << FuncName << "\n");
      }
      LLVM_DEBUG(outs() << "  [Chain Start @ 0x" << Twine::utohexstr(BF.getAddress() + LuiOffset) << "]\n");
      LLVM_DEBUG(BC.printInstruction(outs(), *LuiII, LuiOffset, &BF));
      uint64_t AddiOffset = AddiBB->getInputOffset() + std::distance(AddiBB->begin(), AddiII) * 4;
      LLVM_DEBUG(BC.printInstruction(outs(), *AddiII, AddiOffset, &BF));

      auto ChainBB = AddiBB;
      auto ChainII = AddiII;

      while (true) {
        auto LuiNextBB = ChainBB;
        auto LuiNextII = ChainII;
        if (!nextInst(LuiNextBB, LuiNextII)) break;

        if (!isLUI(*LuiNextII) || !isHI(LuiNextII->getOperand(1))) break;
        if (LuiNextII->getOperand(0).getReg() != HiReg) break;

        auto AddiNextBB = LuiNextBB;
        auto AddiNextII = LuiNextII;
        if (!nextInst(AddiNextBB, AddiNextII)) break;

        if (!isADDI(*AddiNextII) /*&& !isLW_SW(*AddiNextII)*/) break;

        const auto &NextLOOp = AddiNextII->getOperand(2);
        if (!isLO(NextLOOp)) break;
        const auto *NextSym = getSym(NextLOOp);
        if (!NextSym) break;

        uint64_t TargetVal = *BC.getSymbolValue(NextSym->getSymbol());
        int64_t Diff = static_cast<int64_t>(TargetVal) - static_cast<int64_t>(BaseVal);
        if (Diff < -2048 || Diff > 2047) break;

        MCPhysReg NextReg = AddiNextII->getOperand(0).getReg();

        LLVM_DEBUG(
          outs() << "  [Chain Link @ 0x" << Twine::utohexstr(BF.getAddress() + LuiNextBB->getInputOffset()) << "]\n";
          BC.printInstruction(outs(), *LuiNextII, LuiNextBB->getInputOffset(), &BF);
          BC.printInstruction(outs(), *AddiNextII, AddiNextBB->getInputOffset(), &BF);
        );

        MCInst NewAddi;
        NewAddi.setOpcode(RISCV::ADDI);
        NewAddi.addOperand(MCOperand::createReg(NextReg));
        NewAddi.addOperand(MCOperand::createReg(CurrReg));
        NewAddi.addOperand(MCOperand::createImm(Diff));

        auto ReplacePos = AddiNextBB->replaceInstruction(AddiNextII, {NewAddi});
        LuiNextBB->eraseInstruction(LuiNextII);

        CurrReg = NextReg;
        BaseVal = TargetVal;
        ChainBB = AddiNextBB;
        ChainII = ReplacePos;

        if (ChainII == ChainBB->end()) {
          ++ChainBB;
          if (ChainBB == BF.end()) break;
          ChainII = ChainBB->begin();
        }
      }

      BBIt = ChainBB;
      II = ChainII;
      if (II == BBIt->end()) {
        ++BBIt;
        if (BBIt != BF.end()) II = BBIt->begin();
      }
    }
  }
}


void RISCVReuseLUIAggressive::runOnFunctions(BinaryContext &BC) {
  for (auto &BFIt : BC.getBinaryFunctions()) {
    runOnFunction(BFIt.second);
  }
}

} // namespace bolt
} // namespace llvm

