#include "MCTargetDesc/RISCVMCExpr.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Passes/RISCV/RISCVStackPushOpt.h"
#include <iterator>
#include <bitset>

using namespace llvm;

namespace llvm {
namespace bolt {

void RISCVStackPushOpt::runOnFunction(BinaryFunction &BF) {
  auto &BC = BF.getBinaryContext();
  for (auto &BB : BF) {
    auto II = BB.begin();
    while (II != BB.end()) {
      // 基本指令类型检查
      if (II->getOpcode() != RISCV::SB ||
          II->getOperand(0).getReg() != RISCV::X0 ||
          !II->getOperand(2).isImm()) {
        ++II;
        continue;
      }

      // 核心参数提取
      const MCPhysReg BaseReg = II->getOperand(1).getReg();
      const int64_t FirstOffset = II->getOperand(2).getImm();
      const int64_t AlignedBase = FirstOffset & ~0x3;

      // 收集同一4字节块内的SB指令
      std::vector<decltype(II)> candidates;
      std::bitset<4> offset_mask;  // 标记0-3偏移是否覆盖

      for (auto Scan = II; Scan != BB.end() && candidates.size() < 4; ++Scan) {
        // 指令基础检查
        if (Scan->getOpcode() != RISCV::SB ||
            Scan->getOperand(0).getReg() != RISCV::X0 ||
            Scan->getOperand(1).getReg() != BaseReg ||
            !Scan->getOperand(2).isImm()) break;

        const int64_t curr_offset = Scan->getOperand(2).getImm();

        // 地址块验证
        if ((curr_offset & ~0x3) != AlignedBase) break;

        // 记录偏移特征
        const int offset_mod = curr_offset % 4;
        if (!offset_mask.test(offset_mod)) {
          offset_mask.set(offset_mod);
          candidates.push_back(Scan);
        }
      }

      // 满足4字节覆盖条件
      if (offset_mask.all()) {
        // 创建SW指令
        MCInst SWInst;
        SWInst.setOpcode(RISCV::SW);
        SWInst.addOperand(MCOperand::createReg(RISCV::X0));
        SWInst.addOperand(MCOperand::createReg(BaseReg));
        SWInst.addOperand(MCOperand::createImm(AlignedBase));

        // 替换第一个SB
        BB.replaceInstruction(candidates.front(), {SWInst});

        // 删除后续三个SB（逆序防迭代器失效）
        for (auto it = candidates.rbegin(); it != candidates.rend()-1; ++it) {
          if (*it != candidates.front()) {
            BB.eraseInstruction(*it);
          }
        }

        // 更新迭代器到新SW之后
        II = std::next(candidates.front());
      } else {
        ++II;  // 未满足条件则正常推进
      }
    }
  }
}

void RISCVStackPushOpt::runOnFunctions(BinaryContext &BC) {
    for (auto &BFIt : BC.getBinaryFunctions()) {
        runOnFunction(BFIt.second);
    }
}

} // namespace bolt
} // namespace llvm
