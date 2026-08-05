#include "MCTargetDesc/RISCVMCExpr.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Passes/RISCV/RISCVReuseLUIRelative.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCRegisterInfo.h"
#include <iterator>
#include <map>
#include <utility>


using namespace llvm;

namespace llvm {
namespace bolt {

struct ReusePattern {
  MCPhysReg BaseReg;      // 目标寄存器 xN
  uint64_t Imm;           // C_LUI 立即数
  MCPhysReg SubLHS;       // 通常是 s0
  MCPhysReg SubRHS;       // 也是 BaseReg（xN）
  BinaryBasicBlock *BB;
};

void RISCVReuseLUIRelative::runOnFunction(BinaryFunction &BF) {
  auto &BC = BF.getBinaryContext();
  errs() << "RISCVReuseLUIRelative: Processing Function: " << BF.getNames()[0] << "\n";

  for (auto &BB : BF) {
    dbgs() << "\n🧱 BasicBlock: " << BB.getName() << "\n";

    // 用于保存当前基本块内第一个匹配模式
    MCPhysReg LastTargetReg = 0;
    uint64_t LastLuiImm = 0;
    MCPhysReg LastSubLHS = 0;
    MCPhysReg LastSubRHS = 0;
    bool HasSeenValidPattern = false;

    for (auto II = BB.begin(); II != BB.end(); ) {
      dbgs() << "Instr: ";
      BC.printInstruction(dbgs(), *II, 0, &BF);

      if (II->getOpcode() == RISCV::C_LUI) {
        auto LuiII = II;
        auto SubII = std::next(LuiII);
        auto LwII = (SubII != BB.end()) ? std::next(SubII) : BB.end();

        if (SubII == BB.end() || LwII == BB.end()) {
          ++II;
          continue;
        }

        if (SubII->getOpcode() != RISCV::SUB || LwII->getOpcode() != RISCV::LW) {
          ++II;
          continue;
        }

        auto &Lui = *LuiII;
        auto &Sub = *SubII;
        auto &Lw  = *LwII;

        MCPhysReg Reg = Lui.getOperand(0).getReg();
        uint64_t Imm = Lui.getOperand(1).getImm();

        // SUB目标应为 Reg，SubRHS 也应为 Reg
        if (Sub.getOperand(0).getReg() != Reg || Sub.getOperand(2).getReg() != Reg) {
          ++II;
          continue;
        }

        // LW 的基址应为 Reg
        if (Lw.getOperand(1).getReg() != Reg) {
          ++II;
          continue;
        }

        // ✅ 匹配到了完整 C_LUI+SUB+LW 三条指令
        if (HasSeenValidPattern &&
            LastTargetReg == Reg &&
            LastLuiImm == Imm &&
            LastSubLHS == Sub.getOperand(1).getReg() &&
            LastSubRHS == Reg) {

          dbgs() << "🔥 Redundant C_LUI+SUB+LW pattern found, removing C_LUI & SUB:\n";
          BC.printInstruction(dbgs(), Lui, 0, &BF);
          BC.printInstruction(dbgs(), Sub, 0, &BF);
          BC.printInstruction(dbgs(), Lw, 0, &BF);

          // 删除 SUB 和 C_LUI
          BB.eraseInstruction(SubII);
          II = BB.eraseInstruction(LuiII);

          continue;
        } else {
          // 第一次见到这个 pattern，记录它
          LastTargetReg = Reg;
          LastLuiImm = Imm;
          LastSubLHS = Sub.getOperand(1).getReg();
          LastSubRHS = Reg;
          HasSeenValidPattern = true;

          dbgs() << "✅ First valid C_LUI+SUB+LW pattern recorded:\n";
          BC.printInstruction(dbgs(), Lui, 0, &BF);
          BC.printInstruction(dbgs(), Sub, 0, &BF);
          BC.printInstruction(dbgs(), Lw, 0, &BF);

          II = std::next(LwII);  // skip over 3 instructions
          continue;
        }
      }

      ++II;
    }
  }
}

void RISCVReuseLUIRelative::runOnFunctions(BinaryContext &BC) {
  for (auto &BFIt : BC.getBinaryFunctions()) {
    runOnFunction(BFIt.second);
  }
}

} // namespace bolt
} // namespace llvm