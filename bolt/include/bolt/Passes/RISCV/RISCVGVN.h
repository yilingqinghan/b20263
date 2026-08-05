//
// Created by yz on 25-4-16.
//

#ifndef RISCVGVN_H
#define RISCVGVN_H


#include "bolt/Passes/BinaryPasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/MCInstrDesc.h"

namespace llvm {
namespace bolt {

class RISCVGVN : public BinaryFunctionPass {
  void runOnFunction(BinaryFunction &BF);

public:
  explicit RISCVGVN(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-gvn"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm


#endif //RISCVGVN_H
