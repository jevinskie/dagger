#include "llvm/DC/DCAnnotationWriter.h"
#include "llvm/MC/MCAtom.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/FormattedStream.h"

using namespace llvm;

DCAnnotationWriter::DCAnnotationWriter(const DCTranslatedInstTracker &DTIT,
                                       const MCRegisterInfo &MRI,
                                       MCInstPrinter &IP)
    : AssemblyAnnotationWriter(), DTIT(DTIT), MRI(MRI), IP(IP) {}

void DCAnnotationWriter::printInfoComment(const Value &V,
                                          formatted_raw_ostream &OS) {
  if (!isa<Instruction>(&V))
    return;

  const SmallVectorImpl<DCTranslatedInst::ValueInfo> *Infos = 0;
  DTIT.getInstsForValue(V, Infos);

  if (Infos == 0)
    return;

  for (int vii = 0, vie = Infos->size(); vii != vie; ++vii) {

    if (vii)
      OS << "\n";

    const MCDecodedInst *MCDI = (*Infos)[vii].DecodedInst;
    uint64_t Addr = MCDI->Address;

    bool printMI = false;

    const DCTranslatedInst::ValueInfo &VI = (*Infos)[vii];
    OS.PadToColumn(48) << "  ; ";
    switch (VI.OpKind) {
    default:
      llvm_unreachable("Unknown translated operand type!");
    case DCTranslatedInst::ValueInfo::ImpUseKind: {
      OS << "imp-use ";
      OS << MRI.getName(VI.RegNo);
      break;
    }
    case DCTranslatedInst::ValueInfo::ImpDefKind: {
      OS << "imp-def ";
      OS << MRI.getName(VI.RegNo);
      break;
    }
    case DCTranslatedInst::ValueInfo::RegUseKind: {
      OS << "    use ";
      printMI = true;
      break;
    }
    case DCTranslatedInst::ValueInfo::RegDefKind: {
      OS << "    def ";
      printMI = true;
      break;
    }
    case DCTranslatedInst::ValueInfo::ImmOpKind: {
      OS << "    imm ";
      printMI = true;
      break;
    }
    case DCTranslatedInst::ValueInfo::CustomOpKind: {
      OS << " op-use ";
      if (MCDI)
        IP.printMachineOperand(&MCDI->Inst, VI.CustomOpType,VI.MIOperandNo,OS);
      break;
    }
    }
    if (printMI) {
      if (MCDI) {
        const MCOperand &MO = MCDI->Inst.getOperand(VI.MIOperandNo);
        if (MO.isReg())
          OS << MRI.getName(MO.getReg());
        else if (MO.isImm())
          OS << MO.getImm();
      }
      OS.PadToColumn(72);
      OS << " MO#" << VI.MIOperandNo;
    }
    OS.PadToColumn(79);
    OS << " @";
    OS.write_hex(Addr);

    if (MCDI)
      OS << ": ";
    IP.printInst(&MCDI->Inst, OS.PadToColumn(90), "");
  }
}

void DCAnnotationWriter::emitInstructionAnnot(const Instruction *,
                                              formatted_raw_ostream &OS) {}
