//
// Created by yz on 25-4-11.
//

#ifndef RISCVREUSELUIAGGRESSIVE_H
#define RISCVREUSELUIAGGRESSIVE_H

#include "bolt/Passes/BinaryPasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/MCInstrDesc.h"

namespace llvm {
namespace bolt {

class RISCVReuseLUIAggressive : public BinaryFunctionPass {
  void runOnFunction(BinaryFunction &BF);

public:
  explicit RISCVReuseLUIAggressive(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-reuse-lui-aggressive"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif //RISCVREUSELUIAGGRESSIVE_H
