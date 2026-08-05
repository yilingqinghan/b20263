//===-- RISCVAnalyze.cpp - RISC-V 专用分析 Pass 示例 --------------===//
//
//   本示例演示如何：
//   1. 从 BinaryFunction 所属的 BinaryContext 中直接拿到 MCContext、
//      MCSubtargetInfo、MCInstrInfo、MCRegisterInfo 等对象指针；
//   2. 通过 getLayout().blocks() 遍历所有 BasicBlock（而非私有的 layout()）；
//   3. 遍历每条 MCInst，打印 opcode、操作数类型，并查询调度延迟（Latency）；
//   4. 演示了如何在 LLVM 18.1.2 及以后版本中正确使用 BOLT API，
//      并在没有调度模型时跳过延迟计算，避免断言崩溃。
//
//===----------------------------------------------------------------------===//
/*
 * ---------------------------------------------------------------------------
 * NOTE: This file was created and/or refined with the assistance of AI tools
 *       (e.g., ChatGPT). The author has reviewed and validated the content,
 *       and remains fully responsible for its accuracy and completeness.
 * ---------------------------------------------------------------------------
 */

#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/BinaryBasicBlock.h"

// LLVM MC 相关头
#include "llvm/MC/MCInst.h"
#include "bolt/Passes/RISCV/RISCVAnalyze.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Format.h"

using namespace llvm;
using namespace bolt;

namespace llvm {
namespace bolt {

// 对单个函数执行 RISC-V 专用的分析
void RISCVAnalyze::runOnFunction(BinaryFunction &BF) {
  // ——————————————————————————————————————————————————
  // 1. 从 BinaryFunction 拿到所属的 BinaryContext
  // ——————————————————————————————————————————————————
  BinaryContext &BC = BF.getBinaryContext();

  // ——————————————————————————————————————————————————————————
  // 2. 直接从 BinaryContext 中获取 MCInstrInfo、MCSubtargetInfo、MCRegisterInfo
  //    它们在 BinaryContext 中作为 unique_ptr<const ...> 存储
  // ——————————————————————————————————————————————————————————
  const MCInstrInfo    *MCII = BC.MII.get();
  const MCSubtargetInfo *STI  = BC.STI.get();
  const MCRegisterInfo  *MRI  = BC.MRI.get();

  // 由于 STI 是指向 const MCSubtargetInfo，因此下面要用 const 引用
  const MCSchedModel &SM = STI->getSchedModel();

  // 打印一下正在分析的函数名称（通常会是第一个 symbol）
  outs() << "=== 分析函数: " << BF.getPrintName() << " ===\n";

  // ——————————————————————————————————————————————————
  // 3. 遍历函数中的每个 BasicBlock
  //    用 getLayout().blocks()，因为 layout() 是私有成员
  // ——————————————————————————————————————————————————
  for (BinaryBasicBlock *BB : BF.getLayout().blocks()) {
    // 打印当前 BasicBlock 在可执行文件中的偏移地址（InputOffset）
    outs() << "  BasicBlock @ offset 0x"
           << format_hex(BB->getInputOffset(), 10)
           << "  (#instr=" << BB->size() << ")\n";

    // ————————————————————————————————————————————————
    // 4. 遍历 BasicBlock 内的每条 MCInst
    //    直接用 MCInst，不再使用旧的 BinaryInstruction
    // ————————————————————————————————————————————————
    for (const MCInst &Inst : *BB) {
      // 4.1. 获取 opcode，并打印指令名
      unsigned Opcode    = Inst.getOpcode();
      StringRef Mnemonic = MCII->getName(Opcode);
      outs() << "    Instr: " << Mnemonic
             << " (opcode=" << Opcode << ")\n";

      // 4.2. 遍历并打印每个操作数 (MCOperand)
      for (unsigned i = 0, e = Inst.getNumOperands(); i < e; ++i) {
        const MCOperand &MO = Inst.getOperand(i);
        if (MO.isReg()) {
          unsigned Reg = MO.getReg();
          outs() << "      Operand #" << i
                 << " = register: " << MRI->getName(Reg) << "\n";
        } else if (MO.isImm()) {
          int64_t Imm = MO.getImm();
          outs() << "      Operand #" << i
                 << " = immediate: " << Imm << "\n";
        } else if (MO.isSFPImm()) {
          // 从 LLVM15+ 或 14+ 开始，isFPImm/getFPImm 改为了 isSFPImm/getSFPImm
          double FP = MO.getSFPImm();
          outs() << "      Operand #" << i
                 << " = FP immediate: " << FP << "\n";
        } else if (MO.isExpr()) {
          outs() << "      Operand #" << i
                 << " = expression/reloc\n";
        } else {
          outs() << "      Operand #" << i
                 << " = other kind\n";
        }
      }

      // 4.3. 查询“调度延迟”（Latency）
      // 首先获取调度类 ID
      unsigned SchedClassID = MCII->get(Opcode).getSchedClass();

      // 检查是否存在指令调度模型。如果没有，跳过延迟计算，避免断言崩溃
      if (SM.hasInstrSchedModel() && SchedClassID != 0) {
        // computeInstrLatency(...) 会返回估计延迟
        int Latency = SM.computeInstrLatency(*STI, SchedClassID);
        outs() << "      Estimated Latency ≈ " << Latency << "\n";
      } else {
        if (!SM.hasInstrSchedModel()) {
          outs() << "      无指令调度模型，跳过延迟计算\n";
        } else {
          outs() << "      SchedClassID = Invalid (0)，无法估计延迟\n";
        }
      }
    } // for each MCInst
  }   // for each BasicBlock
}

// 对 BinaryContext 中所有 BinaryFunction 执行 runOnFunction
void RISCVAnalyze::runOnFunctions(BinaryContext &BC) {
  // getBinaryFunctions() 返回 std::map<uint64_t, BinaryFunction>
  for (auto &Entry : BC.getBinaryFunctions()) {
    BinaryFunction &BF = Entry.second;
    runOnFunction(BF);
  }
}

} // namespace bolt
} // namespace llvm