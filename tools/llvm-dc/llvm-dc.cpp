#define DEBUG_TYPE "llvm-dc"
#include "llvm/DC/DCInstrSema.h"
#include "llvm/DC/DCRegisterSema.h"
#include "llvm/DC/DCTranslator.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/StringExtras.h"
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
#include "llvm/MC/MCModuleYAML.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringRefMemoryObject.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace object;


static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("Input YAML MCModule file"), cl::Required);

static cl::opt<std::string>
TripleName("triple", cl::desc("Target triple to disassemble for, "
                              "see -version for available targets"),
           cl::Required);

static cl::opt<unsigned>
TransOptLevel("O",
              cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                       "(default = '-O0')"),
              cl::Prefix,
              cl::init(0u));

static StringRef ToolName;

static const Target *getTarget() {
  // Figure out the target triple.
  Triple TheTriple(TripleName);

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
  InitializeAllTargetMCs();
  InitializeAllTargetDCs();
  InitializeAllAsmParsers();
  InitializeAllDisassemblers();

  cl::ParseCommandLineOptions(argc, argv, "LLVM DC sandbox\n");

  ToolName = argv[0];

  const Target *TheTarget = getTarget();

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

  std::unique_ptr<MCDisassembler> DisAsm(TheTarget->createMCDisassembler(*STI));
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

  std::unique_ptr<const MCInstrAnalysis>
    MIA(TheTarget->createMCInstrAnalysis(MII.get()));

  std::unique_ptr<MemoryBuffer> FileBuf;
  if (error_code ec = MemoryBuffer::getFileOrSTDIN(InputFilename, FileBuf)) {
    errs() << " error: unable to read file " << InputFilename
           << ": " << ec.message() <<"\n";
    return 1;
  }
  std::unique_ptr<MCModule> MCM;
  StringRef ErrMsg = yaml2mcmodule(MCM, FileBuf->getBuffer(), *MII, *MRI);
  if (!ErrMsg.empty()) {
    errs() << "error: unable to read yaml mcmodule: " << ErrMsg << "\n";
    return 1;
  }

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

  std::unique_ptr<DCTranslator> DT(new DCTranslator(getGlobalContext(), TOLvl,
                                                    *DIS, *DRS, *MIP,
                                                    *MCM, /* MCOD= */ 0));

  DT->print(outs());
  return 0;
}
