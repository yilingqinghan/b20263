//
// Created by yz on 25-4-3.
//

#ifndef RISCVREUSELUIRELATIVE_H
#define RISCVREUSELUIRELATIVE_H

#include "bolt/Passes/BinaryPasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/MCInstrDesc.h"

namespace llvm {
namespace bolt {

class RISCVReuseLUIRelative : public BinaryFunctionPass {
    void runOnFunction(BinaryFunction &BF);

public:
    explicit RISCVReuseLUIRelative(const cl::opt<bool> &PrintPass)
        : BinaryFunctionPass(PrintPass) {}
    const char *getName() const override { return "riscv-reuse-lui-relative"; }

    void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif //RISCVREUSELUIRELATIVE_H
