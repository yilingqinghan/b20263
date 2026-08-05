//
// Created by yz on 25-4-16.
//

#ifndef RISCVANALYZE_H
#define RISCVANALYZE_H


#include "bolt/Passes/BinaryPasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/MCInstrDesc.h"

namespace llvm {
namespace bolt {

class RISCVAnalyze : public BinaryFunctionPass {
  void runOnFunction(BinaryFunction &BF);

public:
  explicit RISCVAnalyze(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-analyze"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm


#endif //RISCVANALYZE_H
