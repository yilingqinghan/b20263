//
// Created by yz on 25-4-16.
//

#ifndef RISCVMEMORYSSA_H
#define RISCVMEMORYSSA_H



#include "bolt/Passes/BinaryPasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/MCInstrDesc.h"

namespace llvm {
namespace bolt {

class RISCVMemorySSA : public BinaryFunctionPass {
  void runOnFunction(BinaryFunction &BF);
public:
  explicit RISCVMemorySSA(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-memory-ssa"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm



#endif //RISCVMEMORYSSA_H
