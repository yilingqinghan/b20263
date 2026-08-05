#ifndef RISCVSTACKMEMFORWARD_H
#define RISCVSTACKMEMFORWARD_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class RISCVStackMemForward : public BinaryFunctionPass {
  void runOnFunction(BinaryFunction &BF, uint64_t &ForwardedLoads,
                     uint64_t &ErasedStores);

public:
  explicit RISCVStackMemForward(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-stack-mem-forward"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif // RISCVSTACKMEMFORWARD_H
