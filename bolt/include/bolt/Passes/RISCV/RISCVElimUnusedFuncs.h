#ifndef RISCVELIMUNSEDFUNCS
#define RISCVELIMUNSEDFUNCS

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class RISCVElimUnusedFuncs : public BinaryFunctionPass {
public:
  explicit RISCVElimUnusedFuncs(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}

  const char *getName() const override { return "riscv-elim-unused-funcs"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif // RISCVELIMUNSEDFUNCS
