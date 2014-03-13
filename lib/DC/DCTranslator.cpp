#define DEBUG_TYPE "dctranslator"
#include "llvm/DC/DCTranslator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/DC/DCInstrSema.h"
#include "llvm/DC/DCRegisterSema.h"
#include "llvm/MC/MCAtom.h"
#include "llvm/MC/MCFunction.h"
#include "llvm/MC/MCModule.h"
#include "llvm/MC/MCObjectDisassembler.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/StringRefMemoryObject.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>
#include <iterator>
#include <vector>

using namespace llvm;
using namespace object;

DCTranslator::DCTranslator(LLVMContext &Ctx, TransOpt::Level TransOptLevel,
                           DCInstrSema &DIS, DCRegisterSema &DRS,
                           MCInstPrinter &IP, MCModule &MCM,
                           MCObjectDisassembler *MCOD)
    : TheModule("output", Ctx), MCOD(MCOD), MCM(MCM), FPM(&TheModule),
      DTIT(), DIS(DIS), OptLevel(TransOptLevel) {

  if (OptLevel >= TransOpt::Less)
    FPM.add(createPromoteMemoryToRegisterPass());
  if (OptLevel >= TransOpt::Default)
    FPM.add(createDeadCodeEliminationPass());
  if (OptLevel >= TransOpt::Aggressive)
    FPM.add(createInstructionCombiningPass());

  DIS.SwitchToModule(&TheModule);
  MCObjectDisassembler::AddressSetTy DummyTailCallTargets;
  for (MCModule::const_func_iterator FI = MCM.func_begin(), FE = MCM.func_end();
       FI != FE; ++FI)
    translateFunction(*FI, DummyTailCallTargets);
  DIS.FinalizeModule();
  (void)DRS;
}

DCTranslator::~DCTranslator() {}

Function *DCTranslator::getMainFunction() {
  return DIS.createMainFunction(getFunctionAt(getEntrypoint()));
}

Function *DCTranslator::getInitRegSetFunction() {
  return DIS.getInitRegSetFunction();
}

Function *DCTranslator::getFiniRegSetFunction() {
  return DIS.getFiniRegSetFunction();
}

Function *DCTranslator::getFunctionAt(uint64_t Addr) {
  SmallSetVector<uint64_t, 16> WorkList;
  WorkList.insert(Addr);
  for (size_t i = 0; i < WorkList.size(); ++i) {
    uint64_t Addr = WorkList[i];
    Function *F = TheModule.getFunction("fn_" + utohexstr(Addr));
    if (F && !F->isDeclaration())
      continue;

    DEBUG(dbgs() << "Translating function at " << utohexstr(Addr) << "\n");

    if (!MCOD) {
      llvm_unreachable(("Unable to translate unknown function at " +
                        utohexstr(Addr) + " without a disassembler!").c_str());
    }

    MCObjectDisassembler::AddressSetTy CallTargets, TailCallTargets;
    MCFunction *MCFN =
        MCOD->createFunction(&MCM, Addr, CallTargets, TailCallTargets);

    // If the function is empty, it is the declaration of an external function.
    if (MCFN->empty()) {
      StringRef ExtFnName = MCFN->getName();
      assert(!ExtFnName.empty() && "Unnamed function declaration!");
      DEBUG(dbgs() << "Found external function: " << ExtFnName << "\n");
      DIS.createExternalWrapperFunction(Addr, ExtFnName);
      continue;
    }

    translateFunction(MCFN, TailCallTargets);
    typedef MCObjectDisassembler::AddressSetTy::iterator It;
    for (It CTI = CallTargets.begin(), CTE = CallTargets.end(); CTI != CTE;
         ++CTI)
      WorkList.insert(*CTI);
  }
  return TheModule.getFunction("fn_" + utohexstr(Addr));
}

static bool BBBeginAddrLess(const MCBasicBlock *LHS, const MCBasicBlock *RHS) {
  return LHS->getInsts()->getBeginAddr() < RHS->getInsts()->getBeginAddr();
}

void DCTranslator::translateFunction(
    MCFunction *MCFN,
    const MCObjectDisassembler::AddressSetTy &TailCallTargets) {
  uint64_t StartAddr = MCFN->getEntryBlock()->getInsts()->getBeginAddr();

  DIS.SwitchToFunction(StartAddr);

  // First, make sure all basic blocks are created, and sorted.
  typedef std::vector<const MCBasicBlock *> BasicBlockListTy;
  BasicBlockListTy BasicBlocks;
  std::copy(MCFN->begin(), MCFN->end(), std::back_inserter(BasicBlocks));
  std::sort(BasicBlocks.begin(), BasicBlocks.end(), BBBeginAddrLess);
  for (BasicBlockListTy::const_iterator BBI = BasicBlocks.begin(),
                                        BBE = BasicBlocks.end();
       BBI != BBE; ++BBI)
    DIS.getOrCreateBasicBlock((*BBI)->getInsts()->getBeginAddr());

  for (MCFunction::const_iterator BBI = MCFN->begin(), BBE = MCFN->end();
       BBI != BBE; ++BBI) {
    const MCTextAtom *TA = (*BBI)->getInsts();
    DEBUG(dbgs() << "Translating atom " << TA->getName() << ", size "
                 << TA->size() << ", address " << utohexstr(TA->getBeginAddr())
                 << "\n");
    DIS.SwitchToBasicBlock(TA->getBeginAddr(), TA->getEndAddr());
    for (MCTextAtom::const_iterator II = TA->begin(), IE = TA->end(); II != IE;
         ++II) {
      DEBUG(dbgs() << "Translating instruction:\n ";
            dbgs() << II->Inst << "\n";);
      DCTranslatedInst TI(*II);
      if (!DIS.translateInst(*II, TI)) {
        errs() << "Cannot translate instruction: \n  ";
        errs() << II->Inst << "\n";
        llvm_unreachable("Couldn't translate instruction\n");
      }
      DTIT.trackInst(TI);
    }
    DIS.FinalizeBasicBlock();
  }

  typedef MCObjectDisassembler::AddressSetTy::const_iterator AddrIt;
  for (AddrIt TCI = TailCallTargets.begin(), TCE = TailCallTargets.end();
       TCI != TCE; ++TCI)
    DIS.createExternalTailCallBB(*TCI);

  Function *Fn = DIS.FinalizeFunction();
  {
    // ValueToValueMapTy VMap;
    // Function *OrigFn = CloneFunction(Fn, VMap, false);
    // OrigFn->setName(Fn->getName() + "_orig");
    // TheModule.getFunctionList().push_back(OrigFn);
    FPM.run(*Fn);
  }
}

void DCTranslator::print(raw_ostream &OS) {
  TheModule.print(OS, 0);
}
