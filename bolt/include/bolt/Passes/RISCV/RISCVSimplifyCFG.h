//
// Created by yz on 25-4-16.
//

#ifndef RISCVSIMPLIFYCFG_H
#define RISCVSIMPLIFYCFG_H



#include "bolt/Passes/BinaryPasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/MCInstrDesc.h"

namespace llvm {
namespace bolt {

class RISCVSimplifyCFG : public BinaryFunctionPass {
  void runOnFunction(BinaryFunction &BF);

public:
  explicit RISCVSimplifyCFG(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}
  const char *getName() const override { return "riscv-simplify-cfg"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm


#endif //RISCVSIMPLIFYCFG_H
