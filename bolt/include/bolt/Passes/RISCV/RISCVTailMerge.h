#ifndef RISCVTAILMERGE_H
#define RISCVTAILMERGE_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class RISCVTailMerge : public BinaryFunctionPass {
  uint64_t runOnFunction(BinaryFunction &BF);

public:
  explicit RISCVTailMerge(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-tail-merge"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif // RISCVTAILMERGE_H
