#define DEBUG_TYPE "llvm-dec"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/DC/DCInstrSema.h"
#include "llvm/DC/DCRegisterSema.h"
#include "llvm/DC/DCTranslator.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAtom.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCFunction.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCModule.h"
#include "llvm/MC/MCObjectDisassembler.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringRefMemoryObject.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace object;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("Input object file"), cl::Required);

static cl::opt<std::string>
TripleName("triple", cl::desc("Target triple to disassemble for, "
                              "see -version for available targets"));

static cl::opt<bool>
AnnotateIROutput("annot", cl::desc("Enable IR output anotations"),
                 cl::init(false));

static cl::opt<unsigned>
TransOptLevel("O",
              cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                       "(default = '-O0')"),
              cl::Prefix,
              cl::init(0u));

static StringRef ToolName;

static const Target *getTarget(const ObjectFile *Obj) {
  // Figure out the target triple.
  Triple TheTriple("unknown-unknown-unknown");
  if (TripleName.empty()) {
  if (Obj) {
    TheTriple.setArch(Triple::ArchType(Obj->getArch()));
    // TheTriple defaults to ELF, and COFF doesn't have an environment:
    // the best we can do here is indicate that it is mach-o.
    if (Obj->isMachO())
      TheTriple.setObjectFormat(Triple::MachO);
  }
  } else {
    TheTriple.setTriple(TripleName);
  }

  // Get the Target.
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget("", TheTriple, Error);
  if (!TheTarget) {
    errs() << ToolName << ": " << Error;
    return 0;
  }

  // Update the triple name and return the found target.
  TripleName = TheTriple.getTriple();
  return TheTarget;
}

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);
  llvm_shutdown_obj Y;

  InitializeAllTargetInfos();
  InitializeAllTargetDCs();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  InitializeAllDisassemblers();

  cl::ParseCommandLineOptions(argc, argv, "Function disassembler\n");

  ToolName = argv[0];

  ErrorOr<Binary *> BinaryOrErr = createBinary(InputFilename);
  if (error_code ec = BinaryOrErr.getError()) {
    errs() << ToolName << ": '" << InputFilename << "': "
           << ec.message() << ".\n";
    return 1;
  }
  std::unique_ptr<Binary> binary(BinaryOrErr.get());

  ObjectFile *Obj;
  if (!(Obj = dyn_cast<ObjectFile>(binary.get())))
    errs() << ToolName << ": '" << InputFilename << "': "
           << "Unrecognized file type.\n";

  const Target *TheTarget = getTarget(Obj);

  std::unique_ptr<const MCRegisterInfo> MRI(
    TheTarget->createMCRegInfo(TripleName));
  if (!MRI) {
    errs() << "error: no register info for target " << TripleName << "\n";
    return 1;
  }

  // Set up disassembler.
  std::unique_ptr<const MCAsmInfo> MAI(
    TheTarget->createMCAsmInfo(*MRI, TripleName));
  if (!MAI) {
    errs() << "error: no assembly info for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<const MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(TripleName, "", ""));
  if (!STI) {
    errs() << "error: no subtarget info for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<const MCInstrInfo> MII(TheTarget->createMCInstrInfo());
  if (!MII) {
    errs() << "error: no instruction info for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<MCDisassembler> DisAsm(
    TheTarget->createMCDisassembler(*STI));
  if (!DisAsm) {
    errs() << "error: no disassembler for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<MCInstPrinter> MIP(
      TheTarget->createMCInstPrinter(0, *MAI, *MII, *MRI, *STI));
  if (!MIP) {
    errs() << "error: no instprinter for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<const MCInstrAnalysis> MIA(
      TheTarget->createMCInstrAnalysis(MII.get()));

  std::unique_ptr<MCObjectDisassembler> OD(
      new MCObjectDisassembler(*Obj, *DisAsm, *MIA));
  std::unique_ptr<MCModule> MCM(OD->buildModule(/* withCFG */ true));

  if (!MCM)
    return 1;

  TransOpt::Level TOLvl;
  switch (TransOptLevel) {
  default:
    errs() << ToolName << ": invalid optimization level.\n";
    return 1;
  case 0: TOLvl = TransOpt::None; break;
  case 1: TOLvl = TransOpt::Less; break;
  case 2: TOLvl = TransOpt::Default; break;
  case 3: TOLvl = TransOpt::Aggressive; break;
  }

  std::unique_ptr<DCRegisterSema> DRS(
      TheTarget->createDCRegisterSema(TripleName, *MRI, *MII));
  if (!DRS) {
    errs() << "error: no dc register sema for target " << TripleName << "\n";
    return 1;
  }
  std::unique_ptr<DCInstrSema> DIS(
      TheTarget->createDCInstrSema(TripleName, *DRS, *MRI, *MII));
  if (!DIS) {
    errs() << "error: no dc instruction sema for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<DCTranslator> DT(
    new DCTranslator(getGlobalContext(), TOLvl, *DIS, *DRS, *MIP, *MCM,
                     OD.get(), AnnotateIROutput));

  DT->getMainFunction();
  DT->print(outs());
  return 0;
}
