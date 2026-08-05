#ifndef RISCVCONSTLUIREUSE_H
#define RISCVCONSTLUIREUSE_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class RISCVConstLUIReuse : public BinaryFunctionPass {
  void runOnFunction(BinaryFunction &BF, uint64_t &ReusedMaterializations,
                     uint64_t &ReusedMemory, uint64_t &ReusedLUIs);

public:
  explicit RISCVConstLUIReuse(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-const-lui-reuse"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif // RISCVCONSTLUIREUSE_H
