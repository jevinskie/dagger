#include "llvm/DC/DCRegisterSema.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace llvm;

DCRegisterSema::DCRegisterSema(const MCRegisterInfo &MRI,
                               const MCInstrInfo &MII,
                               InitSpecialRegSizesFnTy InitSpecialRegSizesFn)
    : MRI(MRI), MII(MII), NumRegs(MRI.getNumRegs()), NumLargest(0),
      RegSizes(NumRegs), RegLargestSupers(NumRegs),
      RegOffsetsInSet(NumRegs, -1), LargestRegs(), TheModule(0), Ctx(0),
      RegSetType(0), Builder(0), RegPtrs(NumRegs), RegAllocas(NumRegs),
      RegInits(NumRegs), RegAssignments(NumRegs), TheFunction(0),
      RegVals(NumRegs), CurrentInst(0) {

  // First, determine the (spill) size of each register, in bits.
  // FIXME: the best (only) way to know the size of a reg is to find a
  // containing RC.
  // FIXME: What about using the VTs?
  for (MCRegisterInfo::regclass_iterator RCI = MRI.regclass_begin(),
                                         RCE = MRI.regclass_end();
       RCI != RCE; ++RCI) {
    unsigned SizeInBits = RCI->getSize() * 8;
    for (MCRegisterClass::iterator RI = RCI->begin(), RE = RCI->end(); RI != RE;
         ++RI) {
      if (SizeInBits > RegSizes[*RI])
        RegSizes[*RI] = SizeInBits;
    }
  }

  // Let the target-specific implementation set the size of special registers..
  if (InitSpecialRegSizesFn)
    InitSpecialRegSizesFn(RegSizes);

  // Now we have all the sizes we need, determine the largest super registers.
  for (unsigned RI = 1, RE = getNumRegs(); RI != RE; ++RI) {
    if (RegSizes[RI] == 0)
      continue;
    unsigned &Largest = RegLargestSupers[RI];
    Largest = RI;
    for (MCSuperRegIterator SRI(RI, &MRI); SRI.isValid(); ++SRI) {
      if (RegSizes[*SRI] == 0)
        continue;
      if (RegSizes[Largest] < RegSizes[*SRI])
        Largest = *SRI;
    }
  }

  LargestRegs.resize(RegLargestSupers.size());
  std::copy(RegLargestSupers.begin(), RegLargestSupers.end(),
            LargestRegs.begin());
  std::sort(LargestRegs.begin(), LargestRegs.end());
  LargestRegs.erase(std::unique(LargestRegs.begin(), LargestRegs.end()),
                    LargestRegs.end());
  // Now we have a sorted, uniqued vector of the largest registers to keep,
  // starting with register index 0, which we again don't care about.
  NumLargest = LargestRegs.size();

  for (unsigned I = 1, E = getNumLargest(); I != E; ++I) {
    assert(RegSizes[LargestRegs[I]] != 0 &&
           "Largest super-register doesn't have a type!");
    RegOffsetsInSet[LargestRegs[I]] = I - 1;
  }
}

DCRegisterSema::~DCRegisterSema() {}

void DCRegisterSema::SwitchToModule(Module *Mod) {
  TheModule = Mod;
  Ctx = &TheModule->getContext();
  Builder.reset(new DCIRBuilder(*Ctx));

  std::vector<Type *> LargestRegTypes(getNumLargest() - 1);
  for (unsigned I = 1, E = getNumLargest(); I != E; ++I)
    LargestRegTypes[I - 1] = getRegType(LargestRegs[I]);

  RegSetType = StructType::create(LargestRegTypes, "regset");
}

void DCRegisterSema::SwitchToFunction(Function *Fn) { TheFunction = Fn; }

void DCRegisterSema::SwitchToBasicBlock(BasicBlock *TheBB) {
  // Clear all local values.
  for (unsigned RI = 1, RE = MRI.getNumRegs(); RI != RE; ++RI) {
    RegVals[RI] = 0;
  }
  Builder->SetInsertPoint(TheBB);
}

void DCRegisterSema::SwitchToInst(const MCDecodedInst &DecodedInst) {
  CurrentInst = &DecodedInst;
}

void DCRegisterSema::saveAllLocalRegs(BasicBlock *BB, BasicBlock::iterator IP) {
  DCIRBuilder LocalBuilder(BB, IP);

  for (unsigned RI = 1, RE = getNumRegs(); RI != RE; ++RI) {
    if (!RegAllocas[RI])
      continue;
    int OffsetInSet = RegOffsetsInSet[RI];
    if (OffsetInSet != -1)
      LocalBuilder.CreateStore(LocalBuilder.CreateLoad(RegAllocas[RI]),
                               RegPtrs[RI]);
  }
}

void DCRegisterSema::restoreLocalRegs(BasicBlock *BB, BasicBlock::iterator IP) {
  SwitchToBasicBlock(BB);
  Builder->SetInsertPoint(BB, IP);

  for (unsigned RI = 1, RE = getNumRegs(); RI != RE; ++RI) {
    if (!RegAllocas[RI])
      continue;
    int OffsetInSet = RegOffsetsInSet[RI];
    if (OffsetInSet != -1)
      setReg(RI, Builder->CreateLoad(RegPtrs[RI]));
  }
  FinalizeBasicBlock();
}

void DCRegisterSema::FinalizeFunction(BasicBlock *ExitBB) {
  saveAllLocalRegs(ExitBB, ExitBB->getTerminator());

  for (unsigned RI = 1, RE = getNumRegs(); RI != RE; ++RI) {
    RegAllocas[RI] = 0;
    RegPtrs[RI] = 0;
    RegInits[RI] = 0;
  }
}

void DCRegisterSema::FinalizeBasicBlock() {
  if (Instruction *TI = Builder->GetInsertBlock()->getTerminator())
    Builder->SetInsertPoint(TI);
  for (unsigned RI = 1, RE = getNumRegs(); RI != RE; ++RI) {
    onRegisterGet(RI);
    if (!RegVals[RI])
      continue;
    if (RegAllocas[RI])
      Builder->CreateStore(RegVals[RI], RegAllocas[RI]);
    RegVals[RI] = 0;
  }
}

Value *DCRegisterSema::getReg(unsigned RegNo) {
  if (RegNo == 0 || RegNo > MRI.getNumRegs())
    return 0;

  getRegNoCallback(RegNo);
  onRegisterGet(RegNo);
  return RegVals[RegNo];
}

Value *DCRegisterSema::getRegNoCallback(unsigned RegNo) {
  Value *&RV = RegVals[RegNo];

  // First, look for a value in this basic block.
  if (RV)
    return RV;

  // Ensure the reg has an alloca; extract it from a super if it has one.
  createLocalValueForReg(RegNo);

  // Now, we have an alloca; if we don't have the reg in this BB, load it here!
  if (!RV)
    RV = Builder->CreateLoad(RegAllocas[RegNo]);
  setRegValWithName(RegNo, RV);
  onRegisterSet(RegNo, RV);
  return RV;
}

void DCRegisterSema::setRegValWithName(unsigned RegNo, Value *Val) {
  RegVals[RegNo] = Val;
  if (!Val->hasName())
    Val->setName((Twine(MRI.getName(RegNo)) + "_" +
                  utostr(RegAssignments[RegNo]++)).str());
}

void DCRegisterSema::createLocalValueForReg(unsigned RegNo) {
  StringRef RegName = MRI.getName(RegNo);
  Value *&RV = RegVals[RegNo];
  Value *&RA = RegAllocas[RegNo];
  Value *&RP = RegPtrs[RegNo];
  Value *&RI = RegInits[RegNo];

  // If we already have an alloca, nothing to do here.
  if (RA)
    return;
  assert(RP == 0 && "Register has a pointer but no local value!");
  assert(RI == 0 && "Register has a start value but no local value!");
  IRBuilderBase::InsertPoint CurIP = Builder->saveIP();
  BasicBlock *EntryBB = &TheFunction->getEntryBlock();
  unsigned LargestSuper = RegLargestSupers[RegNo];
  if (LargestSuper != RegNo) {
    // If the reg has a super-register, extract from it.
    RV = extractSubRegFromSuper(LargestSuper, RegNo);
    // Also extract from the super reg to initialize the alloca.
    Builder->SetInsertPoint(EntryBB, EntryBB->getTerminator());
    assert(RegInits[LargestSuper] != 0 && "Super-register non initialized!");
    RI = extractSubRegFromSuper(LargestSuper, RegNo, RegInits[LargestSuper]);
  } else {
    // Else, it should be in the regset, load it from there.
    Builder->SetInsertPoint(EntryBB, EntryBB->getTerminator());
    // First, extract the register's value from the incoming regset.
    Value *RegSetArg = &TheFunction->getArgumentList().front();
    int OffsetInRegSet = RegOffsetsInSet[RegNo];
    assert(OffsetInRegSet != -1 && "Getting a register not in the regset!");
    Value *Idx[] = { Builder->getInt32(0), Builder->getInt32(OffsetInRegSet) };
    RP = Builder->CreateInBoundsGEP(RegSetArg, Idx);
    RP->setName((RegName + "_ptr").str());
    RI = Builder->CreateLoad(RP);
  }
  RI->setName((RegName + "_init").str());
  // Then, create an alloca for the register.
  RA = Builder->CreateAlloca(RI->getType());
  RA->setName(RegName);
  // Finally, initialize the local copy of the register.
  Builder->CreateStore(RI, RA);
  Builder->restoreIP(CurIP);
}

Value *DCRegisterSema::extractBitsFromValue(unsigned LoBit, unsigned NumBits,
                                            Value *Val) {
  Value *LShr =
      (LoBit == 0 ? Val : Builder->CreateLShr(
                              Val, ConstantInt::get(Val->getType(), LoBit)));
  return Builder->CreateTruncOrBitCast(LShr, IntegerType::get(*Ctx, NumBits));
}

Value *DCRegisterSema::insertBitsInValue(Value *FullVal, Value *ToInsert,
                                         unsigned Offset, bool ClearOldValue) {
  IntegerType *ValType = cast<IntegerType>(FullVal->getType());
  IntegerType *ToInsertType = cast<IntegerType>(ToInsert->getType());

  Value *Cast = Builder->CreateZExtOrBitCast(ToInsert, ValType);
  if (Offset)
    Cast = Builder->CreateShl(Cast, ConstantInt::get(Cast->getType(), Offset));

  // If we clear FullVal, then this is enough, we don't need to use it.
  if (ClearOldValue)
    return Cast;

  APInt Mask = ~APInt::getBitsSet(ValType->getBitWidth(), Offset,
                                  Offset + ToInsertType->getBitWidth());
  return Builder->CreateOr(Cast, Builder->CreateAnd(
                                     FullVal, ConstantInt::get(ValType, Mask)));
}

Value *DCRegisterSema::extractSubRegFromSuper(unsigned Super, unsigned Sub,
                                              Value *SRV) {
  unsigned Idx = MRI.getSubRegIndex(Super, Sub);
  assert(Idx && "Superreg's subreg doesn't have an index?");
  unsigned Offset = MRI.getSubRegIdxOffset(Idx),
           Size = MRI.getSubRegIdxSize(Idx);
  if (Offset == (unsigned)-1 || Size == (unsigned)-1)
    llvm_unreachable("Used subreg index doesn't cover a bit range?");

  // If no SuperValue was provided, get the current one.
  if (SRV == 0)
    SRV = getReg(Super);

  return extractBitsFromValue(Offset, Size, SRV);
}

Value *DCRegisterSema::recreateSuperRegFromSub(unsigned Super, unsigned Sub) {
  unsigned Idx = MRI.getSubRegIndex(Super, Sub);
  assert(Idx && "Superreg's subreg doesn't have an index?");
  unsigned Offset = MRI.getSubRegIdxOffset(Idx),
           Size = MRI.getSubRegIdxSize(Idx);
  if (Offset == (unsigned)-1 || Size == (unsigned)-1)
    llvm_unreachable("Used subreg index doesn't cover a bit range?");

  Value *RV = getReg(Sub);
  Value *SRV = getReg(Super);

  return insertBitsInValue(SRV, RV, Offset, doesSubRegIndexClearSuper(Idx));
}

void DCRegisterSema::defineAllSubSuperRegs(unsigned RegNo) {
  for (MCSuperRegIterator SRI(RegNo, &MRI); SRI.isValid(); ++SRI) {
    setRegNoSubSuper(*SRI, recreateSuperRegFromSub(*SRI, RegNo));
  }

  for (MCSubRegIterator SRI(RegNo, &MRI); SRI.isValid(); ++SRI) {
    setRegNoSubSuper(*SRI, extractSubRegFromSuper(RegNo, *SRI));
  }
}

void DCRegisterSema::setRegNoSubSuper(unsigned RegNo, Value *Val) {
  // FIXME: This will needlessly extract the subreg from the largest super.
  createLocalValueForReg(RegNo);
  setRegValWithName(RegNo, Val);
  onRegisterSet(RegNo, Val);
}

void DCRegisterSema::setReg(unsigned RegNo, Value *Val) {
  setRegNoSubSuper(RegNo, Val);
  defineAllSubSuperRegs(RegNo);
}

Type *DCRegisterSema::getRegType(unsigned RegNo) {
  return IntegerType::get(*Ctx, RegSizes[RegNo]);
}

void DCRegisterSema::getRegOffsetInRegSet(const DataLayout *DL, unsigned RegNo,
                                          unsigned &SizeInBytes,
                                          unsigned &Offset) const {
  SizeInBytes = RegSizes[RegNo] / 8;

  const StructLayout *SL = DL->getStructLayout(RegSetType);
  unsigned Largest = RegLargestSupers[RegNo];
  unsigned Idx = RegOffsetsInSet[Largest];
  Offset = SL->getElementOffset(Idx);
  if (Largest == RegNo) {
    return;
  }

  unsigned SubRegIdx = MRI.getSubRegIndex(Largest, RegNo);
  assert(SubRegIdx &&
         "Couldn't determine register's offset in super-register.");
  unsigned OffsetInSuper = MRI.getSubRegIdxOffset(SubRegIdx);
  assert(((OffsetInSuper % 8) == 0) && "Register isn't byte aligned!");
  Offset += (OffsetInSuper / 8);
}
