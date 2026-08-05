#ifndef BOLT_PASSES_RISCV_CALLSEQUENCEOUTLINER_H
#define BOLT_PASSES_RISCV_CALLSEQUENCEOUTLINER_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class RISCVCallSequenceOutliner : public BinaryFunctionPass {
  uint64_t runOnFunction(BinaryFunction &BF);

public:
  explicit RISCVCallSequenceOutliner(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}

  const char *getName() const override {
    return "riscv-call-sequence-outliner";
  }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif // BOLT_PASSES_RISCV_CALLSEQUENCEOUTLINER_H
