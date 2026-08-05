#include "MCTargetDesc/RISCVMCExpr.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Passes/RISCV/RISCVReuseLUIAbsolute.h"
#include <iterator>

using namespace llvm;

namespace llvm {
namespace bolt {

static bool isLUI(const MCInst &Inst) {
    return Inst.getOpcode() == RISCV::LUI;
}

void RISCVReuseLUIAbsolute::runOnFunction(BinaryFunction &BF) {
  auto &BC = BF.getBinaryContext();

  for (auto &BB : BF) {
    for (auto II = BB.begin(); II != BB.end(); ++II) {
      if (!isLUI(*II))
        continue;

      MCPhysReg BaseReg = II->getOperand(0).getReg();
      const MCOperand &Op = II->getOperand(1);

      if (!Op.isExpr())
        continue;

      const MCExpr *Expr = Op.getExpr();
      const RISCVMCExpr *RVExpr = dyn_cast<RISCVMCExpr>(Expr);
      if (!RVExpr)
        continue;

      const MCSymbolRefExpr *SymRef = dyn_cast<MCSymbolRefExpr>(RVExpr->getSubExpr());
      if (!SymRef)
        continue;

      StringRef SymName = SymRef->getSymbol().getName();
      auto BinaryData = BC.getBinaryDataByName(SymName);  // 获取 BinaryData

      if (!BinaryData) {
        continue; // 如果符号不存在，跳过
      }

      uint64_t Addr = *BC.getSymbolValue(SymRef->getSymbol()); // 获取符号地址
      uint64_t HighPage = Addr >> 12;

      // 查找后续 LUI
      auto ScanII = std::next(II);
      while (ScanII != BB.end()) {
        if (!isLUI(*ScanII))
          break;

        MCPhysReg DupReg = ScanII->getOperand(0).getReg();
        const MCOperand &DupOp = ScanII->getOperand(1);
        if (!DupOp.isExpr())
          break;

        const MCExpr *DupExpr = DupOp.getExpr();
        const RISCVMCExpr *DupRVExpr = dyn_cast<RISCVMCExpr>(DupExpr);
        if (!DupRVExpr)
          break;

        const MCSymbolRefExpr *DupSymRef = dyn_cast<MCSymbolRefExpr>(DupRVExpr->getSubExpr());
        if (!DupSymRef)
          break;

        StringRef DupSymName = DupSymRef->getSymbol().getName();
        auto DupBinaryData = BC.getBinaryDataByName(DupSymName); // 获取第二个符号的 BinaryData
        if (!DupBinaryData)
          break;

        uint64_t DupAddr = *BC.getSymbolValue(DupSymRef->getSymbol()); // 获取第二个符号地址
        uint64_t DupPage = DupAddr >> 12;

        if (DupPage != HighPage)
          break;

        // Emit mv
        MCInst MV;
        MV.setOpcode(RISCV::C_MV);
        MV.addOperand(MCOperand::createReg(DupReg));
        MV.addOperand(MCOperand::createReg(BaseReg));

        dbgs() << "RISCV-HI20-ALIAS: replaced redundant lui for symbol "
                          << DupSymName << " with mv from " << BaseReg << " to " << DupReg << "\n";

        std::vector<MCInst> MVSeq = { MV };
        ScanII = BB.replaceInstruction(ScanII, MVSeq.begin(), MVSeq.end());

        II = ScanII;
        break; // 只处理一个
      }
    }
  }
}

void RISCVReuseLUIAbsolute::runOnFunctions(BinaryContext &BC) {
    for (auto &BFIt : BC.getBinaryFunctions()) {
        runOnFunction(BFIt.second);
    }
}

} // namespace bolt
} // namespace llvm
