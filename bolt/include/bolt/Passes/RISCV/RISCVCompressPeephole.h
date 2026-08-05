#ifndef RISCVCOMPRESSPEEPHOLE_H
#define RISCVCOMPRESSPEEPHOLE_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class RISCVCompressPeephole : public BinaryFunctionPass {
  uint64_t runOnFunction(BinaryFunction &BF);

public:
  explicit RISCVCompressPeephole(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-compress-peephole"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif // RISCVCOMPRESSPEEPHOLE_H
