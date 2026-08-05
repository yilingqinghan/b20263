#ifndef RISCVGPRELAX_H
#define RISCVGPRELAX_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class RISCVGPRelax : public BinaryFunctionPass {
  void runOnFunction(BinaryFunction &BF);

public:
  explicit RISCVGPRelax(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-gp-relax"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif // RISCVGPRELAX_H
