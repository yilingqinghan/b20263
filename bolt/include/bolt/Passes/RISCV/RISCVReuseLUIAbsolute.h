//
// Created by yz on 25-4-3.
//

#ifndef RISCVREUSELUIABSOLUTE_H
#define RISCVREUSELUIABSOLUTE_H

#include "bolt/Passes/BinaryPasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/MCInstrDesc.h"

namespace llvm {
namespace bolt {

class RISCVReuseLUIAbsolute : public BinaryFunctionPass {
  void runOnFunction(BinaryFunction &BF);

public:
  explicit RISCVReuseLUIAbsolute(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-reuse-lui-absolute"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif //RISCVREUSELUIABSOLUTE_H
