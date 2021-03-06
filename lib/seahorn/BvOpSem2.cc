#include "seahorn/BvOpSem2.hh"

#include "llvm/CodeGen/IntrinsicLowering.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MathExtras.h"

#include "seahorn/Support/CFG.hh"
#include "seahorn/Support/SeaDebug.h"
#include "seahorn/Support/SeaLog.hh"
#include "seahorn/Support/Stats.hh"
#include "seahorn/Transforms/Instrumentation/ShadowMemDsa.hh"

#include "ufo/ExprLlvm.hpp"

#include "BvOpSem2Context.hh"

#include <fstream>
#include <memory>

using namespace seahorn;
using namespace llvm;

using gep_type_iterator = generic_gep_type_iterator<>;

static llvm::cl::opt<bool>
    UseLambdas("horn-bv2-lambdas",
               llvm::cl::desc("Use lambdas for array operations"),
               cl::init(false));

static llvm::cl::opt<unsigned>
    WordSize("horn-bv2-word-size", llvm::cl::desc("Word size in bytes: 1, 4"),
             cl::init(4));

static llvm::cl::opt<unsigned>
    PtrSize("horn-bv2-ptr-size", llvm::cl::desc("Pointer size in bytes: 4"),
            cl::init(4), cl::Hidden);

static llvm::cl::opt<bool> EnableUniqueScalars2(
    "horn-bv2-singleton-aliases",
    llvm::cl::desc("Treat singleton alias sets as scalar values"),
    cl::init(true));

static llvm::cl::opt<bool> InferMemSafety2(
    "horn-bv2-use-mem-safety",
    llvm::cl::desc("Rely on memory safety assumptions such as "
                   "successful load/store imply validity of their arguments"),
    cl::init(true), cl::Hidden);

static llvm::cl::opt<bool> IgnoreCalloc2(
    "horn-bv2-ignore-calloc",
    llvm::cl::desc(
        "Treat calloc same as malloc, ignore that memory is initialized"),
    cl::init(false), cl::Hidden);

static llvm::cl::opt<bool> EnableModelExternalCalls2(
    "horn-bv2-enable-external-calls",
    llvm::cl::desc("Model external function call as an uninterpreted function"),
    llvm::cl::init(false));

static llvm::cl::list<std::string> IgnoreExternalFunctions2(
    "horn-bv2-ignore-external-functions",
    llvm::cl::desc(
        "These functions are not modeled as uninterpreted functions"),
    llvm::cl::ZeroOrMore, llvm::cl::CommaSeparated);

static llvm::cl::opt<bool> SimplifyOnWrite(
    "horn-bv2-simplify",
    llvm::cl::desc("Simplify expressions as they are written to memory"),
    llvm::cl::init(false));

namespace {
const Value *extractUniqueScalar(CallSite &cs) {
  if (!EnableUniqueScalars2)
    return nullptr;
  else
    return seahorn::shadow_dsa::extractUniqueScalar(cs);
}

const Value *extractUniqueScalar(const CallInst *ci) {
  if (!EnableUniqueScalars2)
    return nullptr;
  else
    return seahorn::shadow_dsa::extractUniqueScalar(ci);
}

bool isShadowMem(const Value &V, const Value **out) {
  const Value *scalar;
  bool res = seahorn::shadow_dsa::isShadowMem(V, &scalar);
  if (EnableUniqueScalars2 && out)
    *out = scalar;
  return res;
}

const seahorn::details::Bv2OpSemContext &const_ctx(const OpSemContext &_ctx);

/// \brief Work-arround for a bug in llvm::CallSite::getCalledFunction
/// properly handle bitcast
Function *getCalledFunction(CallSite &CS) {
  Function *fn = CS.getCalledFunction();
  if (fn)
    return fn;

  Value *v = CS.getCalledValue();
  if (v)
    v = v->stripPointerCasts();
  fn = dyn_cast<Function>(v);

  return fn;
}

} // namespace
namespace seahorn {
namespace details {
struct OpSemVisitorBase {
  Bv2OpSemContext &m_ctx;
  ExprFactory &m_efac;
  Bv2OpSem &m_sem;

  Expr trueE;
  Expr falseE;
  Expr zeroE;
  Expr oneE;

  OpSemVisitorBase(Bv2OpSemContext &ctx, Bv2OpSem &sem)
      : m_ctx(ctx), m_efac(m_ctx.getExprFactory()), m_sem(sem) {

    trueE = m_ctx.m_trueE;
    falseE = m_ctx.m_falseE;
    zeroE = m_ctx.zeroE;
    oneE = m_ctx.oneE;

    // XXX AG: this is probably wrong since instances of OpSemVisitorBase are
    // created
    // XXX AG: for each instruction, not just once per function
    // XXX AG: but not an issue at this point since function calls are not
    // handled by the semantics
    // -- first two arguments are reserved for error flag,
    // -- the other is function activation
    // ctx.pushParameter(falseE);
    // ctx.pushParameter(falseE);
    // ctx.pushParameter(falseE);
  }

  unsigned ptrSzInBits() { return m_ctx.ptrSzInBits(); }

  Expr read(const Value &v) {
    if (m_sem.isSkipped(v))
      return Expr();

    Expr reg;
    if (reg = m_ctx.getRegister(v))
      return m_ctx.read(reg);

    if (const Constant *cv = dyn_cast<Constant>(&v)) {
      return m_ctx.getConstantValue(*cv);
    }

    reg = m_ctx.mkRegister(v);
    if (reg)
      return m_ctx.read(reg);

    errs() << "Error: failed to read a value for: " << v << "\n";
    llvm_unreachable(nullptr);
  }

  Expr lookup(const Value &v) { return m_sem.getOperandValue(v, m_ctx); }

  /// \brief Havocs the register corresponding to \p v.
  ///
  /// Creates a register for \p v if necessary. Writes a new fresh symbolic
  /// constant to the store for \p v.
  ///
  /// \return the fresh value that was written or null (empty Expr).
  Expr havoc(const Value &v) {
    if (m_sem.isSkipped(v))
      return Expr();

    assert(m_ctx.getMemManager());
    OpSemMemManager &memManager = *m_ctx.getMemManager();

    Expr reg;
    if (reg = m_ctx.getRegister(v)) {
      Expr h = memManager.coerce(reg, m_ctx.havoc(reg));
      m_ctx.write(reg, h);
      return h;
    }

    if (reg = m_ctx.mkRegister(v)) {
      Expr h = memManager.coerce(reg, m_ctx.havoc(reg));
      m_ctx.write(reg, h);
      return h;
    }

    errs() << "Error: failed to havoc: " << v << "\n";
    llvm_unreachable(nullptr);
  }

  void write(const Value &v, Expr val) {
    if (m_sem.isSkipped(v))
      return;

    Expr reg;
    if (reg = m_ctx.getRegister(v))
      m_ctx.write(reg, val);
    else {
      assert(!isa<Constant>(v));
      reg = m_ctx.mkRegister(v);
      if (reg)
        m_ctx.write(reg, val);
      else {
        errs() << "Error: failed to write: " << v << "\n";
        llvm_unreachable(nullptr);
      }
    }
  }

  void setValue(const Value &v, Expr e) {
    if (e)
      write(v, e);
    else {
      m_sem.unhandledValue(v, m_ctx);
      havoc(v);
    }
  }
};

class OpSemVisitor : public InstVisitor<OpSemVisitor>, OpSemVisitorBase {
public:
  OpSemVisitor(Bv2OpSemContext &ctx, Bv2OpSem &sem)
      : OpSemVisitorBase(ctx, sem) {}

  // Opcode Implementations
  void visitReturnInst(ReturnInst &I) {
    // -- skip return argument of main
    if (I.getParent()->getParent()->getName().equals("main"))
      return;

    // read the operand of return instruction so that the read is observable
    // b symstore
    if (I.getNumOperands() > 0)
      lookup(*I.getOperand(0));
  }
  void visitBranchInst(BranchInst &I) {
    if (I.isConditional())
      lookup(*I.getCondition());
  }

  void visitSwitchInst(SwitchInst &I) {
    llvm_unreachable("switch instructions are not supported. Must be lowered.");
  }
  void visitIndirectBrInst(IndirectBrInst &I) { llvm_unreachable(nullptr); }

  void visitBinaryOperator(BinaryOperator &I) {
    Type *ty = I.getOperand(0)->getType();
    Expr op0 = lookup(*I.getOperand(0));
    Expr op1 = lookup(*I.getOperand(1));
    Expr res;

    if (ty->isVectorTy()) {
      llvm_unreachable(nullptr);
    }

    if (op0 && op1) {
      switch (I.getOpcode()) {
      default:
        errs() << "Unknown binary operator: " << I << "\n";
        llvm_unreachable(nullptr);
        break;
      case Instruction::Add:
        res = m_ctx.alu().doAdd(op0, op1, ty->getScalarSizeInBits());
        break;
      case Instruction::Sub:
        res = m_ctx.alu().doSub(op0, op1, ty->getScalarSizeInBits());
        break;
      case Instruction::Mul:
        res = m_ctx.alu().doMul(op0, op1, ty->getScalarSizeInBits());
        break;
      case Instruction::FAdd:
        break;
      case Instruction::FSub:
        break;
      case Instruction::FMul:
        break;
      case Instruction::FDiv:
        break;
      case Instruction::FRem:
        break;
      case Instruction::UDiv:
        res = m_ctx.alu().doUDiv(op0, op1, ty->getScalarSizeInBits());
        break;
      case Instruction::SDiv:
        res = m_ctx.alu().doSDiv(op0, op1, ty->getScalarSizeInBits());
        break;
      case Instruction::URem:
        res = m_ctx.alu().doURem(op0, op1, ty->getScalarSizeInBits());
        break;
      case Instruction::SRem:
        res = m_ctx.alu().doSRem(op0, op1, ty->getScalarSizeInBits());
        break;
      case Instruction::And:
        res = m_ctx.alu().doAnd(op0, op1, ty->getScalarSizeInBits());
        break;
      case Instruction::Or:
        res = m_ctx.alu().doOr(op0, op1, ty->getScalarSizeInBits());
        break;
      case Instruction::Xor:
        res = m_ctx.alu().doXor(op0, op1, ty->getScalarSizeInBits());
        break;
      }
    }

    setValue(I, res);
  }

  void visitICmpInst(ICmpInst &I) {
    Type *ty = I.getOperand(0)->getType();
    Expr op0 = lookup(*I.getOperand(0));
    Expr op1 = lookup(*I.getOperand(1));
    Expr res;

    if (op0 && op1) {
      switch (I.getPredicate()) {
      case ICmpInst::ICMP_EQ:
        res = executeICMP_EQ(op0, op1, ty, m_ctx);
        break;
      case ICmpInst::ICMP_NE:
        res = executeICMP_NE(op0, op1, ty, m_ctx);
        break;
      case ICmpInst::ICMP_ULT:
        res = executeICMP_ULT(op0, op1, ty, m_ctx);
        break;
      case ICmpInst::ICMP_SLT:
        res = executeICMP_SLT(op0, op1, ty, m_ctx);
        break;
      case ICmpInst::ICMP_UGT:
        res = executeICMP_UGT(op0, op1, ty, m_ctx);
        break;
      case ICmpInst::ICMP_SGT:
        res = executeICMP_SGT(op0, op1, ty, m_ctx);
        break;
      case ICmpInst::ICMP_ULE:
        res = executeICMP_ULE(op0, op1, ty, m_ctx);
        break;
      case ICmpInst::ICMP_SLE:
        res = executeICMP_SLE(op0, op1, ty, m_ctx);
        break;
      case ICmpInst::ICMP_UGE:
        res = executeICMP_UGE(op0, op1, ty, m_ctx);
        break;
      case ICmpInst::ICMP_SGE:
        res = executeICMP_SGE(op0, op1, ty, m_ctx);
        break;
      default:
        errs() << "Unknown ICMP predicate" << I;
        llvm_unreachable(nullptr);
      }
    }
    setValue(I, res);
  }

  void visitFCmpInst(FCmpInst &I) { llvm_unreachable(nullptr); }

  void visitAllocaInst(AllocaInst &I) {
    Type *ty = I.getType()->getElementType();
    unsigned typeSz = (size_t)m_sem.getTD().getTypeAllocSize(ty);

    Expr addr;
    if (const Constant *cv = dyn_cast<const Constant>(I.getOperand(0))) {
      auto ogv = m_sem.getConstantValue(cv);
      if (!ogv.hasValue()) {
        llvm_unreachable(nullptr);
      }
      unsigned nElts = ogv.getValue().IntVal.getZExtValue();
      unsigned memSz = typeSz * nElts;
      LOG("opsem",
          errs() << "!3 Alloca of " << memSz << " bytes: " << I << "\n";);
      addr = m_ctx.mem().salloc(memSz);
    } else {
      Expr nElts = lookup(*I.getOperand(0));
      LOG("opsem", errs() << "!4 Alloca of (" << *nElts << " * " << typeSz
                          << ") bytes: " << I << "\n";);
      addr = m_ctx.mem().salloc(nElts, typeSz);
    }

    setValue(I, addr);
  }

  void visitLoadInst(LoadInst &I) {
    setValue(I, executeLoadInst(*I.getPointerOperand(), I.getAlignment(),
                                I.getType(), m_ctx));
  }
  void visitStoreInst(StoreInst &I) {
    executeStoreInst(*I.getValueOperand(), *I.getPointerOperand(),
                     I.getAlignment(), m_ctx);
  }

  void visitGetElementPtrInst(GetElementPtrInst &I) {
    Expr val = executeGEPOperation(*I.getPointerOperand(), gep_type_begin(I),
                                   gep_type_end(I), m_ctx);
    setValue(I, val);
  }

  void visitPHINode(PHINode &PN) {
    // -- there is a special visitor for PHINodes
    llvm_unreachable("PHI nodes are handled by a different visitor!");
  }
  void visitTruncInst(TruncInst &I) {
    setValue(I, executeTruncInst(*I.getOperand(0), *I.getType(), m_ctx));
  }
  void visitZExtInst(ZExtInst &I) {
    setValue(I, executeZExtInst(*I.getOperand(0), *I.getType(), m_ctx));
  }
  void visitSExtInst(SExtInst &I) {
    setValue(I, executeSExtInst(*I.getOperand(0), *I.getType(), m_ctx));
  }

  // floating point instructions
  void visitFPTruncInst(FPTruncInst &I) { llvm_unreachable(nullptr); }
  void visitFPExtInst(FPExtInst &I) { llvm_unreachable(nullptr); }
  void visitUIToFPInst(UIToFPInst &I) { llvm_unreachable(nullptr); }
  void visitSIToFPInst(SIToFPInst &I) { llvm_unreachable(nullptr); }
  void visitFPToUIInst(FPToUIInst &I) { llvm_unreachable(nullptr); }
  void visitFPToSIInst(FPToSIInst &I) { llvm_unreachable(nullptr); }

  void visitPtrToIntInst(PtrToIntInst &I) {
    setValue(I, executePtrToIntInst(*I.getOperand(0), I.getType(), m_ctx));
  }
  void visitIntToPtrInst(IntToPtrInst &I) {
    setValue(I, executeIntToPtrInst(*I.getOperand(0), I.getType(), m_ctx));
  }

  void visitBitCastInst(BitCastInst &I) {
    setValue(I, executeBitCastInst(*I.getOperand(0), I.getType(), m_ctx));
  }

  void visitSelectInst(SelectInst &I) {
    Type *ty = I.getOperand(0)->getType();
    Expr cond = lookup(*I.getCondition());
    Expr op0 = lookup(*I.getTrueValue());
    Expr op1 = lookup(*I.getFalseValue());

    Expr res = executeSelectInst(cond, op0, op1, ty, m_ctx);
    setValue(I, res);
  }

  void visitCallSite(CallSite CS) {
    if (!CS.isCall()) {
      llvm_unreachable("invoke instructions "
                       "are not supported and must be lowered");
    }

    const Function *f = getCalledFunction(CS);
    if (!f) {
      visitIndirectCall(CS);
      return;
    }

    // -- should be handled by visitIntrinsicInst
    assert(!f->isIntrinsic());

    if (f->getName().startswith("verifier.assume")) {
      visitVerifierAssumeCall(CS);
      return;
    }

    if (f->getName().equals("calloc")) {
      visitCallocCall(CS);
      return;
    }

    if (CS.getInstruction()->getMetadata("shadow.mem")) {
      visitShadowMemCall(CS);
      return;
    }

    if (f->getName().startswith("shadow.mem")) {
      WARN << "missing metadata on shadow.mem functions. "
              "Probably using old ShadowMem pass. "
              "Some features might not work as expected";
      visitShadowMemCall(CS);
      return;
    }

    if (f->isDeclaration()) {
      if (f->arg_empty() && (f->getName().startswith("nd") ||
                             f->getName().startswith("nondet.") ||
                             f->getName().startswith("verifier.nondet") ||
                             f->getName().startswith("__VERIFIER_nondet")))
        visitNondetCall(CS);
      else
        visitExternalCall(CS);
      return;
    }

    if (m_sem.hasFunctionInfo(*f)) {
      visitKnownFunctionCall(CS);
    }

    ERR << "unhandled call instruction: " << *CS.getInstruction();
    llvm_unreachable(nullptr);
  }

  void visitIndirectCall(CallSite CS) {
    // treat as non-det and issue a warning
    setValue(*CS.getInstruction(), Expr());
  }

  void visitVerifierAssumeCall(CallSite CS) {
    Function &f = *getCalledFunction(CS);

    Expr op = lookup(*CS.getArgument(0));
    assert(op);

    if (f.getName().equals("verifier.assume.not"))
      op = boolop::lneg(op);

    if (!isOpX<TRUE>(op)) {
      m_ctx.addScopedSide(boolop::lor(
          m_ctx.read(m_sem.errorFlag(*(CS.getInstruction()->getParent()))),
          op));
    }
  }

  void visitCallocCall(CallSite CS) {
    if (!m_ctx.getMemReadRegister() || !m_ctx.getMemReadRegister()) {
      LOG("opsem", WARN << "treating calloc() as nop";);
      return;
    }

    assert(!m_ctx.isMemScalar());

    if (IgnoreCalloc2) {
      LOG("opsem", WARN << "treating calloc() as malloc()";);
      m_ctx.addDef(m_ctx.read(m_ctx.getMemWriteRegister()),
                   m_ctx.read(m_ctx.getMemReadRegister()));
    } else {
      LOG("opsem", WARN << "allowing calloc() to "
                           "zero initialize ALL of its memory region\n";);
      // TODO: move into MemManager
      m_ctx.addDef(
          m_ctx.read(m_ctx.getMemWriteRegister()),
          op::array::constArray(m_ctx.mem().ptrSort(), m_ctx.mem().nullPtr()));
    }

    // get a fresh pointer
    const Instruction &inst = *CS.getInstruction();
    setValue(inst, havoc(inst));
  }

  void visitShadowMemCall(CallSite CS) {
    const Instruction &inst = *CS.getInstruction();

    const Function &F = *getCalledFunction(CS);
    if (F.getName().equals("shadow.mem.init")) {
      unsigned id = shadow_dsa::getShadowId(CS);
      assert(id >= 0);
      setValue(inst, havoc(inst));
      return;
    }

    if (F.getName().equals("shadow.mem.load")) {
      const Value &v = *CS.getArgument(1);
      Expr reg = m_ctx.mkRegister(v);
      m_ctx.read(reg);
      m_ctx.setMemReadRegister(reg);
      m_ctx.setMemScalar(extractUniqueScalar(CS) != nullptr);
      return;
    }

    if (F.getName().equals("shadow.mem.trsfr.load")) {
      const Value &v = *CS.getArgument(1);
      Expr reg = m_ctx.mkRegister(v);
      m_ctx.read(reg);
      m_ctx.setMemTrsfrReadReg(reg);
      if (extractUniqueScalar(CS) != nullptr) {
        WARN << "unexpected unique scalar in mem.trsfr.load: " << inst;
        llvm_unreachable(nullptr);
      }
      return;
    }

    if (F.getName().equals("shadow.mem.store")) {
      Expr memOut = m_ctx.mkRegister(inst);
      Expr memIn = m_ctx.getRegister(*CS.getArgument(1));
      m_ctx.read(memIn);
      setValue(inst, havoc(inst));

      m_ctx.setMemReadRegister(memIn);
      m_ctx.setMemWriteRegister(memOut);
      m_ctx.setMemScalar(extractUniqueScalar(CS) != nullptr);

      LOG("opsem.mem.store", errs() << "mem.store: " << inst << "\n";
          errs() << "arg1: " << *CS.getArgument(1) << "\n";
          errs() << "mem.store: memIn is " << *memIn << " memOut is " << *memOut
                 << "\n";);
      return;
    }

    if (F.getName().equals("shadow.mem.arg.ref")) {
      m_ctx.pushParameter(lookup(*CS.getArgument(1)));
      return;
    }

    if (F.getName().equals("shadow.mem.arg.mod")) {
      m_ctx.pushParameter(lookup(*CS.getArgument(1)));
      Expr reg = m_ctx.mkRegister(inst);
      assert(reg);
      m_ctx.pushParameter(m_ctx.havoc(reg));
      return;
    }

    if (F.getName().equals("shadow.mem.arg.new")) {
      Expr reg = m_ctx.mkRegister(inst);
      m_ctx.pushParameter(m_ctx.havoc(reg));
      return;
    }

    const Function &PF = *inst.getParent()->getParent();

    if (F.getName().equals("shadow.mem.in")) {
      if (PF.getName().equals("main"))
        setValue(inst, havoc(inst));
      else
        lookup(*CS.getArgument(1));
      return;
    }

    if (F.getName().equals("shadow.mem.out")) {
      if (PF.getName().equals("main"))
        setValue(inst, havoc(inst));
      else
        lookup(*CS.getArgument(1));
      return;
    }

    if (F.getName().equals("shadow.mem.arg.init")) {
      if (PF.getName().equals("main"))
        setValue(inst, havoc(inst));
      return;
    }

    if (F.getName().equals("shadow.mem.global.init")) {
      Expr memOut = m_ctx.mkRegister(inst);
      Expr memIn = m_ctx.getRegister(*CS.getArgument(1));
      m_ctx.read(memIn);
      setValue(inst, lookup(*CS.getArgument(1)));

      m_ctx.setMemReadRegister(memIn);
      m_ctx.setMemWriteRegister(memOut);

      LOG("opsem.mem.global.init", errs()
                                       << "mem.global.init: " << inst << "\n";
          errs() << "arg1: " << *CS.getArgument(1) << "\n";
          errs() << "memIn: " << *memIn << ", memOut: " << *memOut << "\n";);

      Value *gVal = (*CS.getArgument(2)).stripPointerCasts();
      if (auto *gv = dyn_cast<llvm::GlobalVariable>(gVal)) {
        auto gvVal = m_ctx.getGlobalVariableInitValue(*gv);
        if (gvVal.first) {
          m_ctx.MemFill(lookup(*gv), gvVal.first, gvVal.second);
        }
      } else {
        WARN << "skipping global var init of " << inst << " to " << *gVal
             << "\n";
      }
      return;
    }

    WARN << "unknown shadow.mem call: " << inst;
    llvm_unreachable(nullptr);
  }

  void visitNondetCall(CallSite CS) {
    const Instruction &inst = *CS.getInstruction();
    if (!inst.getType()->isVoidTy()) {
      setValue(inst, m_ctx.havoc(m_ctx.mkRegister(inst)));
    }
  }
  void visitExternalCall(CallSite CS) {
    Function &F = *getCalledFunction(CS);
    if (F.getFunctionType()->getReturnType()->isVoidTy())
      return;

    const Instruction &inst = *CS.getInstruction();

    if (!EnableModelExternalCalls2 ||
        std::find(IgnoreExternalFunctions2.begin(),
                  IgnoreExternalFunctions2.end(),
                  F.getName()) != IgnoreExternalFunctions2.end()) {
      setValue(inst, Expr());
      return;
    }

    // Treat the call as an uninterpreted function
    Expr res;
    ExprVector fargs;
    ExprVector sorts;
    fargs.reserve(CS.arg_size());
    sorts.reserve(CS.arg_size());

    bool is_typed = true;
    for (auto &a : CS.args()) {
      if (m_sem.isSkipped(*a))
        continue;

      Expr e = lookup(*a);
      if (!e)
        continue;
      fargs.push_back(e);
      Expr s = bind::typeOf(e);
      if (!s) {
        // bind::typeOf is partially defined
        is_typed = false;
        break;
      }
      sorts.push_back(s);
    }

    if (is_typed) {
      // return type of the function
      Expr symReg = m_ctx.mkRegister(inst);
      Expr ty = bind::typeOf(symReg);
      if (!ty) {
        is_typed = false;
      } else {
        sorts.push_back(ty);
      }
    }

    if (is_typed) {
      LOG("opsem", errs() << "Modelling " << inst
                          << " with an uninterpreted function\n";);
      Expr name = mkTerm<const Function *>(getCalledFunction(CS), m_efac);
      Expr d = bind::fdecl(name, sorts);
      res = bind::fapp(d, fargs);
    }

    setValue(inst, res);
  }

  void visitKnownFunctionCall(CallSite CS) {
    const Function &F = *getCalledFunction(CS);
    const FunctionInfo &fi = m_sem.getFunctionInfo(F);
    const Instruction &inst = *CS.getInstruction();
    const BasicBlock &BB = *inst.getParent();

    // enabled
    m_ctx.setParameter(0, m_ctx.getPathCond()); // path condition
    // error flag in
    m_ctx.setParameter(1, m_ctx.read(m_sem.errorFlag(BB)));
    // error flag out
    m_ctx.setParameter(2, m_ctx.havoc(m_sem.errorFlag(BB)));
    for (const Argument *arg : fi.args)
      m_ctx.pushParameter(lookup(*CS.getArgument(arg->getArgNo())));
    for (const GlobalVariable *gv : fi.globals)
      m_ctx.pushParameter(lookup(*gv));

    if (fi.ret) {
      Expr reg = m_ctx.mkRegister(inst);
      Expr v = m_ctx.havoc(reg);
      setValue(inst, v);
      m_ctx.pushParameter(v);
    }

    LOG("arg_error",

        if (m_ctx.getParameters().size() != bind::domainSz(fi.sumPred)) {
          const Instruction &I = *CS.getInstruction();
          const Function &PF = *BB.getParent();
          errs() << "Call instruction: " << I << "\n";
          errs() << "Caller: " << PF << "\n";
          errs() << "Callee: " << F << "\n";
          // errs () << "Sum predicate: " << *fi.sumPred << "\n";
          errs() << "m_ctx.getParameters().size: "
                 << m_ctx.getParameters().size() << "\n";
          errs() << "Domain size: " << bind::domainSz(fi.sumPred) << "\n";
          errs() << "m_ctx.getParameters()\n";
          for (auto r : m_ctx.getParameters())
            errs() << *r << "\n";
          errs() << "regions: " << fi.regions.size()
                 << " args: " << fi.args.size()
                 << " globals: " << fi.globals.size() << " ret: " << fi.ret
                 << "\n";
          errs() << "regions\n";
          for (auto r : fi.regions)
            errs() << *r << "\n";
          errs() << "args\n";
          for (auto r : fi.args)
            errs() << *r << "\n";
          errs() << "globals\n";
          for (auto r : fi.globals)
            errs() << *r << "\n";
          if (fi.ret)
            errs() << "ret: " << *fi.ret << "\n";
        });

    assert(m_ctx.getParameters().size() == bind::domainSz(fi.sumPred));
    m_ctx.addSide(bind::fapp(fi.sumPred, m_ctx.getParameters()));

    m_ctx.resetParameters();
    m_ctx.pushParameter(falseE);
    m_ctx.pushParameter(falseE);
    m_ctx.pushParameter(falseE);
  }

  void visitIntrinsicInst(IntrinsicInst &I) {
    switch (I.getIntrinsicID()) {
    case Intrinsic::bswap: {
      BasicBlock::iterator me(&I);
      auto *parent = I.getParent();
      bool atBegin(parent->begin() == me);
      if (!atBegin)
        --me;
      IntrinsicLowering IL(m_sem.getDataLayout());
      IL.LowerIntrinsicCall(&I);
      if (atBegin) {
        m_ctx.setInstruction(*parent->begin());
      } else {
        m_ctx.setInstruction(*me);
      }
    } break;
    default:
      // interpret by non-determinism (and a warning)
      if (!I.getType()->isVoidTy())
        setValue(I, Expr());
    }
  }

  void visitDbgDeclareInst(DbgDeclareInst &I) { /* nothing */
  }
  void visitDbgValueInst(DbgValueInst &I) { /* nothing */
  }
  void visitDbgInfoIntrinsic(DbgInfoIntrinsic &I) { /* nothing */
  }

  void visitMemSetInst(MemSetInst &I) {
    executeMemSetInst(*I.getDest(), *I.getValue(), *I.getLength(),
                      I.getAlignment(), m_ctx);
  }
  void visitMemCpyInst(MemCpyInst &I) {
    executeMemCpyInst(*I.getDest(), *I.getSource(), *I.getLength(),
                      I.getAlignment(), m_ctx);
  }

  void visitMemMoveInst(MemMoveInst &I) {
    LOG("opsem", errs() << "Skipping memmove: " << I << "\n";);
  }
  void visitMemTransferInst(MemTransferInst &I) {
    LOG("opsem", errs() << "Unknown memtransfer: " << I << "\n";);
    llvm_unreachable(nullptr);
  }

  void visitMemIntrinsic(MemIntrinsic &I) {
    LOG("opsem", errs() << "Unknown memory intrinsic: " << I << "\n";);
    llvm_unreachable(nullptr);
  }

  void visitVAStartInst(VAStartInst &I) { llvm_unreachable(nullptr); }
  void visitVAEndInst(VAEndInst &I) { llvm_unreachable(nullptr); }
  void visitVACopyInst(VACopyInst &I) { llvm_unreachable(nullptr); }

  void visitUnreachableInst(UnreachableInst &I) { /* do nothing */
  }

  void visitShl(BinaryOperator &I) {
    Type *ty = I.getType();
    if (ty->isVectorTy()) {
      llvm_unreachable(nullptr);
    }

    Expr op0 = lookup(*I.getOperand(0));
    Expr op1 = lookup(*I.getOperand(1));
    Expr res;

    if (op0 && op1) {
      res = mk<BSHL>(op0, op1);
    }

    setValue(I, res);
  }

  void visitLShr(BinaryOperator &I) {
    Type *ty = I.getType();
    if (ty->isVectorTy()) {
      llvm_unreachable(nullptr);
    }

    Expr op0 = lookup(*I.getOperand(0));
    Expr op1 = lookup(*I.getOperand(1));
    Expr res;

    if (op0 && op1) {
      res = mk<BLSHR>(op0, op1);
    }

    setValue(I, res);
  }

  void visitAShr(BinaryOperator &I) {
    Type *ty = I.getType();
    if (ty->isVectorTy()) {
      llvm_unreachable(nullptr);
    }

    Expr op0 = lookup(*I.getOperand(0));
    Expr op1 = lookup(*I.getOperand(1));
    Expr res;

    if (op0 && op1) {
      res = mk<BASHR>(op0, op1);
    }

    setValue(I, res);
  }

  void visitVAArgInst(VAArgInst &I) { llvm_unreachable(nullptr); }

  void visitExtractElementInst(ExtractElementInst &I) {
    llvm_unreachable(nullptr);
  }
  void visitInsertElementInst(InsertElementInst &I) {
    llvm_unreachable(nullptr);
  }
  void visitShuffleVectorInst(ShuffleVectorInst &I) {
    llvm_unreachable(nullptr);
  }

  // void visitExtractValueInst(ExtractValueInst &I);
  // void visitInsertValueInst(InsertValueInst &I);

  void visitInstruction(Instruction &I) {
    ERR << I;
    llvm_unreachable("No semantics to this instruction yet!");
  }

  Expr executeSelectInst(Expr cond, Expr op0, Expr op1, Type *ty,
                         Bv2OpSemContext &ctx) {
    if (ty->isVectorTy()) {
      llvm_unreachable(nullptr);
    }
    return cond && op0 && op1 ? bind::lite(cond, op0, op1) : Expr(0);
  }

  Expr executeTruncInst(const Value &v, const Type &ty, Bv2OpSemContext &ctx) {
    if (v.getType()->isVectorTy()) {
      llvm_unreachable(nullptr);
    }

    Expr op0 = lookup(v);
    if (!op0)
      return Expr();

    return ctx.alu().doTrunc(op0, m_sem.sizeInBits(ty));
  }

  Expr executeZExtInst(const Value &v, const Type &ty, Bv2OpSemContext &ctx) {
    if (v.getType()->isVectorTy()) {
      llvm_unreachable(nullptr);
    }

    Expr op0 = lookup(v);
    if (!op0)
      return Expr();
    return ctx.alu().doZext(op0, m_sem.sizeInBits(ty), m_sem.sizeInBits(v));
  }

  Expr executeSExtInst(const Value &v, const Type &ty, Bv2OpSemContext &ctx) {
    if (v.getType()->isVectorTy()) {
      llvm_unreachable(nullptr);
    }

    Expr op0 = lookup(v);
    if (!op0)
      return Expr();
    return ctx.alu().doSext(op0, m_sem.sizeInBits(ty), m_sem.sizeInBits(v));
  }

  Expr executeICMP_EQ(Expr op0, Expr op1, Type *ty, Bv2OpSemContext &ctx) {
    switch (ty->getTypeID()) {
    case Type::IntegerTyID:
      return ctx.alu().doEq(op0, op1, ty->getScalarSizeInBits());
    case Type::PointerTyID:
      return ctx.mem().ptrEq(op0, op1);
    default:
      errs() << "Unhandled ICMP_EQ predicate: " << *ty << "\n";
      llvm_unreachable(nullptr);
    }
    llvm_unreachable(nullptr);
  }

  Expr executeICMP_NE(Expr op0, Expr op1, Type *ty, Bv2OpSemContext &ctx) {
    switch (ty->getTypeID()) {
    case Type::IntegerTyID:
      return ctx.alu().doNe(op0, op1, ty->getScalarSizeInBits());
    case Type::PointerTyID:
      return ctx.mem().ptrNe(op0, op1);
    default:
      errs() << "Unhandled ICMP_NE predicate: " << *ty << "\n";
      llvm_unreachable(nullptr);
    }
    llvm_unreachable(nullptr);
  }

  Expr executeICMP_ULT(Expr op0, Expr op1, Type *ty, Bv2OpSemContext &ctx) {
    switch (ty->getTypeID()) {
    case Type::IntegerTyID:
      return ctx.alu().doUlt(op0, op1, ty->getScalarSizeInBits());
    case Type::PointerTyID:
      return ctx.mem().ptrUlt(op0, op1);
    default:
      errs() << "Unhandled ICMP_ULT predicate: " << *ty << "\n";
      llvm_unreachable(nullptr);
    }
    llvm_unreachable(nullptr);
  }

  Expr executeICMP_SLT(Expr op0, Expr op1, Type *ty, Bv2OpSemContext &ctx) {
    switch (ty->getTypeID()) {
    case Type::IntegerTyID:
      return ctx.alu().doSlt(op0, op1, ty->getScalarSizeInBits());
    case Type::PointerTyID:
      return ctx.mem().ptrSlt(op0, op1);
    default:
      errs() << "Unhandled ICMP_SLT predicate: " << *ty << "\n";
      llvm_unreachable(nullptr);
    }
    llvm_unreachable(nullptr);
  }

  Expr executeICMP_UGT(Expr op0, Expr op1, Type *ty, Bv2OpSemContext &ctx) {
    switch (ty->getTypeID()) {
    case Type::IntegerTyID:
      return ctx.alu().doUgt(op0, op1, ty->getScalarSizeInBits());
    case Type::PointerTyID:
      return ctx.mem().ptrUgt(op0, op1);
    default:
      errs() << "Unhandled ICMP_UGT predicate: " << *ty << "\n";
      llvm_unreachable(nullptr);
    }
    llvm_unreachable(nullptr);
  }

  Expr executeICMP_SGT(Expr op0, Expr op1, Type *ty, Bv2OpSemContext &ctx) {

    switch (ty->getTypeID()) {
    case Type::IntegerTyID:
      return ctx.alu().doSgt(op0, op1, ty->getScalarSizeInBits());
    case Type::PointerTyID:
      return ctx.mem().ptrSgt(op0, op1);
    default:
      errs() << "Unhandled ICMP_SGT predicate: " << *ty << "\n";
      llvm_unreachable(nullptr);
    }
    llvm_unreachable(nullptr);
  }

  Expr executeICMP_ULE(Expr op0, Expr op1, Type *ty, Bv2OpSemContext &ctx) {
    switch (ty->getTypeID()) {
    case Type::IntegerTyID:
      return ctx.alu().doUle(op0, op1, ty->getScalarSizeInBits());
    case Type::PointerTyID:
      return ctx.mem().ptrUle(op0, op1);
    default:
      errs() << "Unhandled ICMP_ULE predicate: " << *ty << "\n";
      llvm_unreachable(nullptr);
    }
    llvm_unreachable(nullptr);
  }

  Expr executeICMP_SLE(Expr op0, Expr op1, Type *ty, Bv2OpSemContext &ctx) {
    switch (ty->getTypeID()) {
    case Type::IntegerTyID:
      return ctx.alu().doSle(op0, op1, ty->getScalarSizeInBits());
    case Type::PointerTyID:
      return ctx.mem().ptrSle(op0, op1);
    default:
      errs() << "Unhandled ICMP_SLE predicate: " << *ty << "\n";
      llvm_unreachable(nullptr);
    }
    llvm_unreachable(nullptr);
  }

  Expr executeICMP_UGE(Expr op0, Expr op1, Type *ty, Bv2OpSemContext &ctx) {
    switch (ty->getTypeID()) {
    case Type::IntegerTyID:
      return ctx.alu().doUge(op0, op1, ty->getScalarSizeInBits());
    case Type::PointerTyID:
      return ctx.mem().ptrUge(op0, op1);
    default:
      errs() << "Unhandled ICMP_SLE predicate: " << *ty << "\n";
      llvm_unreachable(nullptr);
    }
    llvm_unreachable(nullptr);
  }

  Expr executeICMP_SGE(Expr op0, Expr op1, Type *ty, Bv2OpSemContext &ctx) {
    switch (ty->getTypeID()) {
    case Type::IntegerTyID:
      return ctx.alu().doSge(op0, op1, ty->getScalarSizeInBits());
    case Type::PointerTyID:
      return ctx.mem().ptrSge(op0, op1);
    default:
      errs() << "Unhandled ICMP_SGE predicate: " << *ty << "\n";
      llvm_unreachable(nullptr);
    }
    llvm_unreachable(nullptr);
  }

  Expr executePtrToIntInst(const Value &op, const Type *ty,
                           Bv2OpSemContext &ctx) {
    Expr res = lookup(op);
    if (!res)
      return Expr();
    return ctx.ptrtoint(res, *op.getType(), *ty);
  }

  Expr executeIntToPtrInst(const Value &op, const Type *ty,
                           Bv2OpSemContext &ctx) {
    Expr res = lookup(op);
    if (!res)
      return Expr();
    return ctx.inttoptr(res, *op.getType(), *ty);
  }

  Expr executeGEPOperation(const Value &ptr, gep_type_iterator it,
                           gep_type_iterator end, Bv2OpSemContext &ctx) {
    Expr addr = lookup(ptr);
    return addr ? ctx.gep(addr, it, end) : Expr();
  }

  Expr executeLoadInst(const Value &addr, unsigned alignment, const Type *ty,
                       Bv2OpSemContext &ctx) {
    Expr res;
    if (!ctx.getMemReadRegister())
      return res;

    if (ctx.isMemScalar()) {
      res = ctx.read(ctx.getMemReadRegister());
      if (ty->isIntegerTy(1))
        res = ctx.alu().bv1ToBool(res);
    } else if (Expr op0 = lookup(addr)) {
      res = ctx.loadValueFromMem(op0, *ty, alignment);
    }

    ctx.setMemReadRegister(Expr());
    return res;
  }

  Expr executeStoreInst(const Value &val, const Value &addr, unsigned alignment,
                        Bv2OpSemContext &ctx) {

    if (!ctx.getMemReadRegister() || !ctx.getMemWriteRegister() ||
        m_sem.isSkipped(val)) {
      LOG("opsem",
          errs() << "Skipping store to " << addr << " of " << val << "\n";);
      ctx.setMemReadRegister(Expr());
      ctx.setMemWriteRegister(Expr());
      return Expr();
    }

    Expr v = lookup(val);
    Expr res;
    if (v && ctx.isMemScalar()) {
      if (val.getType()->isIntegerTy(1))
        v = ctx.alu().boolToBv1(v);
      res = v;
      ctx.write(ctx.getMemWriteRegister(), res);
    } else {
      Expr p = lookup(addr);
      if (v && p)
        res = m_ctx.storeValueToMem(v, p, *val.getType(), alignment);
    }

    if (!res)
      LOG("opsem",
          errs() << "Skipping store to " << addr << " of " << val << "\n";);

    ctx.setMemReadRegister(Expr());
    ctx.setMemWriteRegister(Expr());
    return res;
  }

  Expr executeMemSetInst(const Value &dst, const Value &val,
                         const Value &length, unsigned alignment,
                         Bv2OpSemContext &ctx) {
    if (!ctx.getMemReadRegister() || !ctx.getMemWriteRegister() ||
        m_sem.isSkipped(dst) || m_sem.isSkipped(val)) {
      LOG("opsem", WARN << "Skipping memset\n");
      ctx.setMemReadRegister(Expr());
      ctx.setMemWriteRegister(Expr());
      return Expr();
    }

    if (ctx.isMemScalar()) {
      ERR << "memset to scalars is not supported";
      llvm_unreachable(nullptr);
    }

    Expr res;
    Expr addr = lookup(dst);

    assert(val.getType()->isIntegerTy(8));
    Expr v = lookup(val);
    Expr len = lookup(length);
    if (v && addr) {
      if (const ConstantInt *ci = dyn_cast<const ConstantInt>(&length)) {
        res = m_ctx.MemSet(addr, v, ci->getZExtValue(), alignment);
      } else
        llvm_unreachable("Unsupported memset with symbolic length");
    }

    if (!res)
      LOG("opsem", errs() << "Skipping memset\n";);

    ctx.setMemReadRegister(Expr());
    ctx.setMemWriteRegister(Expr());
    return res;
  }

  Expr executeMemCpyInst(const Value &dst, const Value &src,
                         const Value &length, unsigned alignment,
                         Bv2OpSemContext &ctx) {
    if (!ctx.getMemReadRegister() || !ctx.getMemWriteRegister() ||
        !ctx.getMemTrsfrReadReg() || m_sem.isSkipped(dst) ||
        m_sem.isSkipped(src)) {
      LOG("opsem", WARN << "skipping memcpy");
      ctx.setMemTrsfrReadReg(Expr());
      ctx.setMemReadRegister(Expr());
      ctx.setMemWriteRegister(Expr());
      return Expr();
    }

    if (ctx.isMemScalar())
      llvm_unreachable("memcpy to scalars is not supported");

    Expr res;
    Expr dstAddr = lookup(dst);
    Expr srcAddr = lookup(src);
    Expr len = lookup(length);
    if (dstAddr && srcAddr) {
      if (const ConstantInt *ci = dyn_cast<const ConstantInt>(&length)) {
        res = m_ctx.MemCpy(dstAddr, srcAddr, ci->getZExtValue(), alignment);
      } else
        llvm_unreachable("Unsupported memcpy with symbolic length");
    }

    if (!res)
      LOG("opsem", errs() << "Skipping memcpy\n";);

    ctx.setMemTrsfrReadReg(Expr());
    ctx.setMemReadRegister(Expr());
    ctx.setMemWriteRegister(Expr());
    return res;
  }

  Expr executeBitCastInst(const Value &op, Type *ty, Bv2OpSemContext &ctx) {
    Type *opTy = op.getType();

    if (opTy->getTypeID() == Type::VectorTyID ||
        ty->getTypeID() == Type::VectorTyID)
      llvm_unreachable("Vector types are unsupported");

    Expr res = lookup(op);
    if (!res)
      return Expr();

    if (ty->isPointerTy())
      return res;

    if (ty->isIntegerTy()) {
      if (opTy->isFloatTy())
        llvm_unreachable("bitcast from float to int is not supported");
      else if (opTy->isDoubleTy())
        llvm_unreachable("bitcast from double to int is not supported");
      else if (opTy->isIntegerTy()) {
        return res;
      } else {
        llvm_unreachable("Invalid bitcast");
      }
    } else if (ty->isFloatTy()) {
      if (opTy->isIntegerTy())
        llvm_unreachable("bitcast to float not supported");
      else
        return res;
    } else if (ty->isDoubleTy()) {
      if (opTy->isIntegerTy())
        llvm_unreachable("bitcast to double not supported");
      else
        return res;
    }

    llvm_unreachable("Invalid bitcast");
  }

  void visitModule(Module &M) {
    LOG("opsem.module", errs() << M << "\n";);
    m_ctx.onModuleEntry(M);

    for (const Function &fn : M.functions()) {
      if (fn.hasAddressTaken()) {
        // XXX hard-coded. should be based on use
        // XXX some functions have their address taken for llvm.used
        if (fn.getName().equals("verifier.error") ||
            fn.getName().startswith("verifier.assume") ||
            fn.getName().equals("seahorn.fail") ||
            fn.getName().startswith("shadow.mem"))
          continue;
        Expr symReg = m_ctx.mkRegister(fn);
        assert(symReg);
        setValue(fn, m_ctx.getMemManager()->falloc(fn));
      }
    }

    for (const GlobalVariable &gv : M.globals()) {
      if (m_sem.isSkipped(gv))
        continue;
      if (gv.getSection().equals("llvm.metadata")) {
        LOG("opsem", WARN << "Skipping global variable marked "
                             "by llvm.metadata section: @"
                          << gv.getName(););
        continue;
      }
      Expr symReg = m_ctx.mkRegister(gv);
      assert(symReg);
      setValue(gv, m_ctx.getMemManager()->galloc(gv));
    }

    LOG("opsem", m_ctx.getMemManager()->dumpGlobalsMap());
  }

  void visitBasicBlock(BasicBlock &BB) {
    Function &F = *BB.getParent();
    /// -- check if globals need to be initialized
    if (&F.getEntryBlock() == &BB) {
      if (F.getName().equals("main"))
        visitModule(*F.getParent());
      m_ctx.onFunctionEntry(*BB.getParent());
    }

    // read the error flag to make it live
    m_ctx.read(m_sem.errorFlag(BB));
  }
};

struct OpSemPhiVisitor : public InstVisitor<OpSemPhiVisitor>, OpSemVisitorBase {
  OpSemPhiVisitor(Bv2OpSemContext &ctx, Bv2OpSem &sem)
      : OpSemVisitorBase(ctx, sem) {}

  void visitBasicBlock(BasicBlock &BB) {
    // -- evaluate all phi-nodes atomically. First read all incoming
    // -- values, then update phi-nodes all together.
    llvm::SmallVector<Expr, 8> ops;

    auto curr = BB.begin();
    if (!isa<PHINode>(curr))
      return;

    // -- evaluate all incoming values in parallel
    for (; PHINode *phi = dyn_cast<PHINode>(curr); ++curr) {
      // skip phi nodes that are not tracked
      if (m_sem.isSkipped(*phi))
        continue;
      const Value &v = *phi->getIncomingValueForBlock(m_ctx.getPrevBb());
      ops.push_back(lookup(v));
    }

    // -- set values to all PHINode registers
    curr = BB.begin();
    for (unsigned i = 0; PHINode *phi = dyn_cast<PHINode>(curr); ++curr) {
      if (m_sem.isSkipped(*phi))
        continue;
      setValue(*phi, ops[i++]);
    }
  }
};
} // namespace details
} // namespace seahorn

namespace seahorn {
namespace details {
Bv2OpSemContext::Bv2OpSemContext(Bv2OpSem &sem, SymStore &values,
                                 ExprVector &side)
    : OpSemContext(values, side), m_sem(sem), m_func(nullptr), m_bb(nullptr),
      m_inst(nullptr), m_prev(nullptr), m_scalar(false) {
  zeroE = mkTerm<mpz_class>(0, efac());
  oneE = mkTerm<mpz_class>(1, efac());

  m_alu = mkBvOpSemAlu(*this);
  setMemManager(
      new OpSemMemManager(m_sem, *this, PtrSize, WordSize, UseLambdas));
}

Bv2OpSemContext::Bv2OpSemContext(SymStore &values, ExprVector &side,
                                 const Bv2OpSemContext &o)
    : OpSemContext(values, side), m_sem(o.m_sem), m_func(o.m_func),
      m_bb(o.m_bb), m_inst(o.m_inst), m_prev(o.m_prev),
      m_readRegister(o.m_readRegister), m_writeRegister(o.m_writeRegister),
      m_scalar(o.m_scalar), m_trfrReadReg(o.m_trfrReadReg),
      m_fparams(o.m_fparams), m_ignored(o.m_ignored),
      m_registers(o.m_registers), m_alu(nullptr), m_memManager(nullptr),
      m_parent(&o), zeroE(o.zeroE), oneE(o.oneE), m_z3(o.m_z3) {
  setPathCond(o.getPathCond());
}

void Bv2OpSemContext::write(Expr v, Expr u) {
  if (SimplifyOnWrite) {
    ScopedStats _st_("opsem.simplify");
    if (!m_z3)
      m_z3.reset(new ufo::EZ3(efac()));

    ufo::ZParams<ufo::EZ3> params(*m_z3);
    params.set("ctrl_c", true);
    // params.set("timeout", 10000U /*ms*/);
    // params.set("flat", false);
    // params.set("ite_extra_rules", false /*default=false*/);
    Expr _u = z3_simplify(*m_z3, u, params);
    LOG("opsem.simplify",
        //
        if (!isOpX<LAMBDA>(_u) && !isOpX<ITE>(_u) && dagSize(_u) > 100) {
          errs() << "Term after simplification:\n"
                 << m_z3->toSmtLib(_u) << "\n";
        });

    LOG("opsem.dump.subformulae",
        if ((isOpX<EQ>(_u) || isOpX<NEG>(_u)) && dagSize(_u) > 100) {
          static unsigned cnt = 0;
          std::ofstream file("assert." + std::to_string(++cnt) + ".smt2");
          file << m_z3->toSmtLibDecls(_u) << "\n";
          file << "(assert " << m_z3->toSmtLib(_u) << ")\n";
        });
    u = _u;
  }
  OpSemContext::write(v, u);
}
unsigned Bv2OpSemContext::ptrSzInBits() const {
  // XXX refactoring hack
  if (!m_parent && !m_memManager)
    return 32;

  return mem().ptrSzInBits();
}

void Bv2OpSemContext::setMemManager(OpSemMemManager *man) {
  m_memManager.reset(man);

  // TODO: move into MemManager
  mpz_class val;
  switch (ptrSzInBits()) {
  case 64:
    // TODO: take alignment into account
    val = 0x000000000FFFFFFF;
    break;
  case 32:
    // TODO: take alignment into account
    val = 0x0FFFFFFF;
    break;
  default:
    LOG("opsem",
        errs() << "Unsupported pointer size: " << ptrSzInBits() << "\n";);
    llvm_unreachable("Unexpected pointer size");
  }
}

Expr Bv2OpSemContext::loadValueFromMem(Expr ptr, const llvm::Type &ty,
                                       uint32_t align) {
  assert(m_memManager);
  assert(getMemReadRegister());
  return m_memManager->loadValueFromMem(ptr, getMemReadRegister(), ty, align);
}

Expr Bv2OpSemContext::storeValueToMem(Expr val, Expr ptr, const llvm::Type &ty,
                                      uint32_t align) {
  assert(m_memManager);
  assert(getMemReadRegister());
  assert(getMemWriteRegister());
  return m_memManager->storeValueToMem(val, ptr, getMemReadRegister(),
                                       getMemWriteRegister(), ty, align);
}

Expr Bv2OpSemContext::MemSet(Expr ptr, Expr val, unsigned len, uint32_t align) {
  assert(m_memManager);
  assert(getMemReadRegister());
  assert(getMemWriteRegister());
  return m_memManager->MemSet(ptr, val, len, getMemReadRegister(),
                              getMemWriteRegister(), align);
}

Expr Bv2OpSemContext::MemCpy(Expr dPtr, Expr sPtr, unsigned len,
                             uint32_t align) {
  assert(m_memManager);
  assert(getMemTrsfrReadReg());
  assert(getMemReadRegister());
  assert(getMemWriteRegister());
  return m_memManager->MemCpy(dPtr, sPtr, len, getMemTrsfrReadReg(),
                              getMemReadRegister(), getMemWriteRegister(),
                              align);
}

Expr Bv2OpSemContext::MemFill(Expr dPtr, char *sPtr, unsigned len,
                              uint32_t align) {
  assert(m_memManager);
  assert(getMemReadRegister());
  assert(getMemWriteRegister());
  return m_memManager->MemFill(dPtr, sPtr, len, align);
}

Expr Bv2OpSemContext::inttoptr(Expr intValue, const Type &intTy,
                               const Type &ptrTy) const {
  return mem().inttoptr(intValue, intTy, ptrTy);
}

Expr Bv2OpSemContext::ptrtoint(Expr ptrValue, const Type &ptrTy,
                               const Type &intTy) const {
  return mem().ptrtoint(ptrValue, ptrTy, intTy);
}

Expr Bv2OpSemContext::gep(Expr ptr, gep_type_iterator it,
                          gep_type_iterator end) const {
  return mem().gep(ptr, it, end);
}

void Bv2OpSemContext::onFunctionEntry(const Function &fn) {
  mem().onFunctionEntry(fn);
}
void Bv2OpSemContext::onModuleEntry(const Module &M) {
  return mem().onModuleEntry(M);
}

void Bv2OpSemContext::onBasicBlockEntry(const BasicBlock &bb) {
  if (!m_func)
    m_func = bb.getParent();
  assert(m_func == bb.getParent());
  if (m_bb)
    m_prev = m_bb;
  m_bb = &bb;
  m_inst = bb.begin();
}

void Bv2OpSemContext::declareRegister(Expr v) { m_registers.insert(v); }
bool Bv2OpSemContext::isKnownRegister(Expr v) { return m_registers.count(v); }

Expr Bv2OpSemContext::mkRegister(const llvm::BasicBlock &bb) {
  if (Expr r = getRegister(bb))
    return r;
  Expr reg = bind::boolConst(mkTerm<const BasicBlock *>(&bb, efac()));
  declareRegister(reg);
  m_valueToRegister.insert(std::make_pair(&bb, reg));
  return reg;
}

Expr Bv2OpSemContext::mkPtrRegisterSort(const Instruction &inst) const {
  return mem().mkPtrRegisterSort(inst);
}

Expr Bv2OpSemContext::mkPtrRegisterSort(const Function &fn) const {
  return mem().mkPtrRegisterSort(fn);
}

Expr Bv2OpSemContext::mkPtrRegisterSort(const GlobalVariable &gv) const {
  return mem().mkPtrRegisterSort(gv);
}

Expr Bv2OpSemContext::mkMemRegisterSort(const Instruction &inst) const {
  return mem().mkMemRegisterSort(inst);
}

Expr Bv2OpSemContext::mkRegister(const llvm::Instruction &inst) {
  if (Expr r = getRegister(inst))
    return r;
  Expr reg;
  // everything else is mapped to a symbolic register with a
  // non-deterministic initial value
  Expr v = mkTerm<const Value *>(&inst, efac());

  // pseudo register corresponding to memory blocks
  const Value *scalar = nullptr;
  if (isShadowMem(inst, &scalar)) {
    // if memory is single cell, allocate regular register
    if (scalar) {
      assert(scalar->getType()->isPointerTy());
      Type &eTy = *cast<PointerType>(scalar->getType())->getElementType();
      // -- create a constant with the name v[scalar]
      reg = bind::mkConst(
          op::array::select(v, mkTerm<const Value *>(scalar, efac())),
          alu().intTy(m_sem.sizeInBits(eTy)));
    }

    // if tracking memory content, create array-valued register for
    // the pseudo-assignment
    else { //(true /*m_trackLvl >= MEM*/) {
      reg = bind::mkConst(v, mkMemRegisterSort(inst));
    }
  } else {
    const Type &ty = *inst.getType();
    switch (ty.getTypeID()) {
    case Type::IntegerTyID:
      reg = bind::mkConst(v, alu().intTy(m_sem.sizeInBits(ty)));
     break;
    case Type::PointerTyID:
      reg = bind::mkConst(v, mkPtrRegisterSort(inst));
      break;
    default:
      errs() << "Error: unhandled type: " << ty << " of " << inst << "\n";
      llvm_unreachable(nullptr);
    }
  }
  assert(reg);
  declareRegister(reg);
  m_valueToRegister.insert(std::make_pair(&inst, reg));
  return reg;
}

Expr Bv2OpSemContext::mkRegister(const llvm::Function &fn) {
  if (Expr r = getRegister(fn))
    return r;

  Expr reg;
  Expr v = mkTerm<const Value *>(&fn, efac());

  reg = bind::mkConst(v, mkPtrRegisterSort(fn));
  declareRegister(reg);
  m_valueToRegister.insert(std::make_pair(&fn, reg));
  return reg;
}

Expr Bv2OpSemContext::mkRegister(const llvm::GlobalVariable &gv) {
  if (Expr r = getRegister(gv))
    return r;
  Expr reg;
  Expr v = mkTerm<const Value *>(&gv, efac());

  reg = bind::mkConst(v, mkPtrRegisterSort(gv));
  declareRegister(reg);
  m_valueToRegister.insert(std::make_pair(&gv, reg));
  return reg;
}

Expr Bv2OpSemContext::mkRegister(const llvm::Value &v) {
  if (auto const *bb = dyn_cast<llvm::BasicBlock>(&v)) {
    return mkRegister(*bb);
  }
  if (auto const *inst = dyn_cast<llvm::Instruction>(&v)) {
    return mkRegister(*inst);
  }
  if (auto const *fn = dyn_cast<llvm::Function>(&v)) {
    return mkRegister(*fn);
  }
  if (auto const *gv = dyn_cast<llvm::GlobalVariable>(&v)) {
    return mkRegister(*gv);
  }
  ERR << "cannot make symbolic register for " << v << "\n";
  llvm_unreachable(nullptr);
}

Expr Bv2OpSemContext::getConstantValue(const llvm::Constant &c) {
  // -- easy common cases
  if (c.isNullValue() || isa<ConstantPointerNull>(&c)) {
    return c.getType()->isIntegerTy(1) ? alu().si(0, 1) : mem().nullPtr();
  } else if (const ConstantInt *ci = dyn_cast<const ConstantInt>(&c)) {
    if (ci->getType()->isIntegerTy(1))
      return ci->isOne() ? alu().si(1, 1) : alu().si(0, 1);

    mpz_class k = toMpz(ci->getValue());
    return alu().si(k, m_sem.sizeInBits(c));
  }

  if (c.getType()->isIntegerTy()) {
    auto GVO = m_sem.getConstantValue(&c);
    if (GVO.hasValue()) {
      GenericValue gv = GVO.getValue();
      mpz_class k = toMpz(gv.IntVal);
      return alu().si(k, m_sem.sizeInBits(c));
    }
  } else if (c.getType()->isPointerTy()) {
    LOG("opsem", WARN << "unhandled constant pointer " << c;);
  } else {
    LOG("opsem", WARN << "unhandled constant " << c;);
  }
  return Expr();
}

std::pair<char *, unsigned>
Bv2OpSemContext::getGlobalVariableInitValue(const llvm::GlobalVariable &gv) {
  return m_memManager->getGlobalVariableInitValue(gv);
}
} // namespace details

Bv2OpSem::Bv2OpSem(ExprFactory &efac, Pass &pass, const DataLayout &dl,
                   TrackLevel trackLvl)
    : OperationalSemantics(efac), m_pass(pass), m_trackLvl(trackLvl),
      m_td(&dl) {
  m_canFail = pass.getAnalysisIfAvailable<CanFail>();
  auto *p = pass.getAnalysisIfAvailable<TargetLibraryInfoWrapperPass>();
  if (p)
    m_tli = &p->getTLI();

  // -- hack to get ENode::dump() to compile by forcing a use
  LOG("dump.debug", trueE->dump(););
}

OpSemContextPtr Bv2OpSem::mkContext(SymStore &values, ExprVector &side) {
  return OpSemContextPtr(new details::Bv2OpSemContext(*this, values, side));
}

Bv2OpSem::Bv2OpSem(const Bv2OpSem &o)
    : OperationalSemantics(o), m_pass(o.m_pass), m_trackLvl(o.m_trackLvl),
      m_td(o.m_td), m_canFail(o.m_canFail) {}

Expr Bv2OpSem::errorFlag(const BasicBlock &BB) {
  // -- if BB belongs to a function that cannot fail, errorFlag is always false
  if (m_canFail && !m_canFail->canFail(BB.getParent()))
    return falseE;
  return this->OperationalSemantics::errorFlag(BB);
}

void Bv2OpSem::exec(const BasicBlock &bb, details::Bv2OpSemContext &ctx) {
  ctx.onBasicBlockEntry(bb);

  details::OpSemVisitor v(ctx, *this);
  v.visitBasicBlock(const_cast<BasicBlock &>(bb));
  // skip PHI instructions
  for (; isa<PHINode>(ctx.getCurrentInst()); ++ctx)
    ;

  while (intraStep(ctx)) {
    /* do nothing */;
  }
}

void Bv2OpSem::execPhi(const BasicBlock &bb, const BasicBlock &from,
                       details::Bv2OpSemContext &ctx) {
  ctx.onBasicBlockEntry(bb);
  ctx.setPrevBb(from);
  intraPhi(ctx);
}

Expr Bv2OpSem::symbolicIndexedOffset(gep_type_iterator TI, gep_type_iterator TE,
                                     details::Bv2OpSemContext &ctx) {
  unsigned ptrSz = pointerSizeInBits();

  // numeric offset
  uint64_t noffset = 0;
  // symbolic offset
  Expr soffset;

  for (; TI != TE; ++TI) {
    Value *CurVal = TI.getOperand();
    if (StructType *STy = TI.getStructTypeOrNull()) {
      unsigned fieldNo = cast<ConstantInt>(CurVal)->getZExtValue();
      noffset += fieldOff(STy, fieldNo);
    } else {
      assert(TI.isSequential());
      unsigned sz = storageSize(TI.getIndexedType());
      if (ConstantInt *ci = dyn_cast<ConstantInt>(CurVal)) {
        int64_t arrayIdx = ci->getSExtValue();
        noffset += (uint64_t)arrayIdx * sz;
      } else {
        Expr a = getOperandValue(*CurVal, ctx);
        assert(a);
        a = mk<BMUL>(a, bv::bvnum(sz, ptrSz, m_efac));
        soffset = (soffset ? mk<BADD>(soffset, a) : a);
      }
    }
  }

  Expr res;
  if (noffset > 0)
    res = bv::bvnum(/* cast to make clang on osx happy */
                    (unsigned long int)noffset, ptrSz, m_efac);
  if (soffset)
    res = res ? mk<BADD>(soffset, res) : soffset;

  if (!res) {
    assert(noffset == 0);
    assert(!soffset);
    res = bv::bvnum((unsigned long int)noffset, ptrSz, m_efac);
  }

  return res;
}

unsigned Bv2OpSem::pointerSizeInBits() const {
  return m_td->getPointerSizeInBits();
}

uint64_t Bv2OpSem::sizeInBits(const llvm::Type &t) const {
  return m_td->getTypeSizeInBits(const_cast<llvm::Type *>(&t));
}

uint64_t Bv2OpSem::sizeInBits(const llvm::Value &v) const {
  return sizeInBits(*v.getType());
}

unsigned Bv2OpSem::storageSize(const llvm::Type *t) const {
  return m_td->getTypeStoreSize(const_cast<Type *>(t));
}

unsigned Bv2OpSem::fieldOff(const StructType *t, unsigned field) const {
  return m_td->getStructLayout(const_cast<StructType *>(t))
      ->getElementOffset(field);
}

Expr Bv2OpSem::getOperandValue(const Value &v, details::Bv2OpSemContext &ctx) {
  Expr res;
  if (auto *bb = dyn_cast<BasicBlock>(&v)) {
    Expr reg = ctx.getRegister(*bb);
    if (reg)
      res = ctx.read(reg);
  } else if (auto *fn = dyn_cast<Function>(&v)) {
    if (Expr reg = ctx.getRegister(*fn)) {
      res = ctx.read(reg);
    } else
      res = ctx.getConstantValue(*fn);
  } else if (auto *gv = dyn_cast<GlobalVariable>(&v)) {
    if (Expr reg = ctx.getRegister(*gv)) {
      res = ctx.read(reg);
    } else
      res = ctx.getConstantValue(*gv);
  } else if (auto *cv = dyn_cast<Constant>(&v)) {
    res = ctx.getConstantValue(*cv);
    assert(res);
  } else {
    Expr reg = ctx.getRegister(v);
    if (reg)
      res = ctx.read(reg);
    else
      WARN << "Failed to get register for: " << v << "\n";
  }
  return res;
}

bool Bv2OpSem::isSymReg(Expr v, details::Bv2OpSemContext &C) {
  if (this->OperationalSemantics::isSymReg(v))
    return true;

  if (C.isKnownRegister(v))
    return true;

  // TODO: memStart and memEnd

  // a symbolic register is any expression that resolves to an
  // llvm::Value XXX it might be better to register registers with a
  // SymStore and XXX let register be only expressions that are
  // explicitly marked as registers
  if (!isOpX<FAPP>(v))
    return false;

  Expr u = bind::fname(v);
  u = bind::fname(u);
  if (isOpX<VALUE>(u))
    return true;

  errs() << "Unexpected symbolic value: " << *v << "\n";
}

const Value &Bv2OpSem::conc(Expr v) const {
  assert(isOpX<FAPP>(v));
  // name of the app
  Expr u = bind::fname(v);
  // name of the fdecl
  u = bind::fname(u);
  assert(isOpX<VALUE>(v));
  return *getTerm<const Value *>(v);
}

bool Bv2OpSem::isSkipped(const Value &v) const {
  // skip shadow.mem instructions if memory is not a unique scalar
  // and we are now ignoring memory instructions
  const Value *scalar = nullptr;
  if (isShadowMem(v, &scalar)) {
    return scalar == nullptr && m_trackLvl < MEM;
  }

  const Type *ty = v.getType();
  if (ty->isPointerTy()) {
    // -- XXX A hack because shadow.mem generates not well formed
    // -- bitcode that contains future references. A register that
    // -- is defined later is used to name a shadow region in the
    // -- beginning of the function. Perhaps there is a better
    // -- solution. For now, we just do not track anything that came
    // -- that way.
    if (v.hasOneUse())
      if (const CallInst *ci = dyn_cast<const CallInst>(*v.user_begin()))
        if (const Function *fn = ci->getCalledFunction())
          if (fn->getName().startswith("shadow.mem"))
            return true;
    return m_trackLvl < PTR;
  }

  // -- explicitly name all types that we support
  // -- TODO: support arrays and struct values
  switch (ty->getTypeID()) {
  case Type::VoidTyID:
    return false;
  case Type::HalfTyID:
  case Type::FloatTyID:
  case Type::DoubleTyID:
  case Type::X86_FP80TyID:
  case Type::FP128TyID:
  case Type::PPC_FP128TyID:
    // floating types are not supported
    // TODO: integrate floating branch
    return true;
  case Type::LabelTyID:
    ERR << "Unexpected label type";
    llvm_unreachable(nullptr);
  case Type::MetadataTyID:
    ERR << "Unexpected metadata type";
    llvm_unreachable(nullptr);
  case Type::X86_MMXTyID:
    LOG("opsem", WARN << "Unsupported X86 type\n");
    return true;
  case Type::TokenTyID:
    llvm_unreachable("Unexpected token type");
  case Type::IntegerTyID:
    return false;
  case Type::FunctionTyID:
    llvm_unreachable("Unexpected function type");
  case Type::StructTyID:
    LOG("opsem", WARN << "Unsupported struct type\n";);
    return true;
  case Type::ArrayTyID:
    LOG("opsem", WARN << "Unsupported array type\n";);
    return true;
  case Type::PointerTyID:
    // -- pointers are handled earlier in the procedure
    llvm_unreachable(nullptr);
  case Type::VectorTyID:
    LOG("opsem", WARN << "Unsupported vector type\n";);
    return true;
  default:
    LOG("opsem", ERR << "Unknown type: " << *ty << "\n";);
    llvm_unreachable(nullptr);
  }
  llvm_unreachable(nullptr);
}

/// \brief Executes one intra-procedural instructions in the current
/// context. Returns false if there are no more instructions to
/// execute after the last one
bool Bv2OpSem::intraStep(details::Bv2OpSemContext &C) {
  if (C.isAtBbEnd())
    return false;

  const Instruction &inst = C.getCurrentInst();

  // -- non-branch terminators are executed elsewhere
  if (isa<TerminatorInst>(&inst) && !isa<BranchInst>(&inst))
    return false;

  // -- either skip or execute the instruction
  if (isSkipped(inst)) {
    skipInst(inst, C);
  } else {
    // -- execute instruction
    details::OpSemVisitor v(C, *this);
    LOG("opsem.verbose", errs() << "Executing: " << inst << "\n";);
    v.visit(const_cast<Instruction &>(inst));
  }

  // -- advance instruction pointer if needed
  if (!isa<TerminatorInst>(&inst)) {
    ++C;
    return true;
  }
  return false;
}

void Bv2OpSem::intraPhi(details::Bv2OpSemContext &C) {
  assert(C.getPrevBb());

  // act is ignored since phi node only introduces a definition
  details::OpSemPhiVisitor v(C, *this);
  v.visitBasicBlock(const_cast<BasicBlock &>(*C.getCurrBb()));
}
/// \brief Executes one intra-procedural branch instruction in the
/// current context. Assumes that current instruction is a branch
void Bv2OpSem::intraBr(details::Bv2OpSemContext &C, const BasicBlock &dst) {
  const BranchInst *br = dyn_cast<const BranchInst>(&C.getCurrentInst());
  if (!br)
    return;

  // next instruction
  ++C;

  if (br->isConditional()) {
    const Value &c = *br->getCondition();
    if (const Constant *cv = dyn_cast<const Constant>(&c)) {
      auto gv = getConstantValue(cv);
      assert(gv.hasValue());
      if (gv->IntVal.isOneValue() && br->getSuccessor(0) != &dst ||
          gv->IntVal.isNullValue() && br->getSuccessor(1) != &dst) {
        C.resetSide();
        C.addScopedSide(C.read(errorFlag(*C.getCurrBb())));
      }
    } else if (Expr target = getOperandValue(c, C)) {
      Expr cond = br->getSuccessor(0) == &dst ? target : mk<NEG>(target);
      cond = boolop::lor(C.read(errorFlag(*C.getCurrBb())), cond);
      C.addScopedSide(cond);
      C.onBasicBlockEntry(dst);
    }
  } else {
    if (br->getSuccessor(0) != &dst) {
      C.resetSide();
      C.addScopedSide(C.read(errorFlag(*C.getCurrBb())));
    } else {
      C.onBasicBlockEntry(dst);
    }
  }
}

void Bv2OpSem::skipInst(const Instruction &inst,
                        details::Bv2OpSemContext &ctx) {
  const Value *s;
  if (isShadowMem(inst, &s))
    return;
  if (ctx.isIgnored(inst))
    return;
  ctx.ignore(inst);
  LOG("opsem", WARN << "skipping instruction: " << inst << " @ "
                    << inst.getParent()->getName() << " in "
                    << inst.getParent()->getParent()->getName(););
}

void Bv2OpSem::unhandledValue(const Value &v, details::Bv2OpSemContext &ctx) {
  if (const Instruction *inst = dyn_cast<const Instruction>(&v))
    return unhandledInst(*inst, ctx);
  LOG("opsem", WARN << "unhandled value: " << v;);
}
void Bv2OpSem::unhandledInst(const Instruction &inst,
                             details::Bv2OpSemContext &ctx) {
  if (ctx.isIgnored(inst))
    return;
  ctx.ignore(inst);
  LOG("opsem", WARN << "unhandled instruction: " << inst << " @ "
                    << inst.getParent()->getName() << " in "
                    << inst.getParent()->getParent()->getName());
}

/// \brief Returns a symbolic register corresponding to a value
Expr Bv2OpSem::mkSymbReg(const Value &v, OpSemContext &_ctx) {
  return details::ctx(_ctx).mkRegister(v);
}

Expr Bv2OpSem::getSymbReg(const Value &v, const OpSemContext &_ctx) const {
  return const_ctx(_ctx).getRegister(v);
}

/// \brief Returns a concrete value to which a constant evaluates
/// Adapted from llvm::ExecutionEngine
Optional<GenericValue> Bv2OpSem::getConstantValue(const Constant *C) {
  // If its undefined, return the garbage.
  if (isa<UndefValue>(C)) {
    GenericValue Result;
    switch (C->getType()->getTypeID()) {
    default:
      break;
    case Type::IntegerTyID:
    case Type::X86_FP80TyID:
    case Type::FP128TyID:
    case Type::PPC_FP128TyID:
      // Although the value is undefined, we still have to construct an APInt
      // with the correct bit width.
      Result.IntVal = APInt(C->getType()->getPrimitiveSizeInBits(), 0);
      break;
    case Type::StructTyID: {
      // if the whole struct is 'undef' just reserve memory for the value.
      if (StructType *STy = dyn_cast<StructType>(C->getType())) {
        unsigned int elemNum = STy->getNumElements();
        Result.AggregateVal.resize(elemNum);
        for (unsigned int i = 0; i < elemNum; ++i) {
          Type *ElemTy = STy->getElementType(i);
          if (ElemTy->isIntegerTy())
            Result.AggregateVal[i].IntVal =
                APInt(ElemTy->getPrimitiveSizeInBits(), 0);
          else if (ElemTy->isAggregateType()) {
            const Constant *ElemUndef = UndefValue::get(ElemTy);
            Result.AggregateVal[i] = getConstantValue(ElemUndef).getValue();
          }
        }
      }
    } break;
    case Type::VectorTyID:
      // if the whole vector is 'undef' just reserve memory for the value.
      auto *VTy = dyn_cast<VectorType>(C->getType());
      Type *ElemTy = VTy->getElementType();
      unsigned int elemNum = VTy->getNumElements();
      Result.AggregateVal.resize(elemNum);
      if (ElemTy->isIntegerTy())
        for (unsigned int i = 0; i < elemNum; ++i)
          Result.AggregateVal[i].IntVal =
              APInt(ElemTy->getPrimitiveSizeInBits(), 0);
      break;
    }
    return Result;
  }

  // Otherwise, if the value is a ConstantExpr...
  if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
    Constant *Op0 = CE->getOperand(0);
    switch (CE->getOpcode()) {
    case Instruction::GetElementPtr: {
      // Compute the index
      auto base = getConstantValue(Op0);
      GenericValue Result = base.getValue();
      APInt Offset(m_td->getPointerSizeInBits(), 0);
      cast<GEPOperator>(CE)->accumulateConstantOffset(*m_td, Offset);

      char *tmp = (char *)Result.PointerVal;
      Result = PTOGV(tmp + Offset.getSExtValue());
      return Result;
    }
    case Instruction::Trunc: {
      GenericValue GV = getConstantValue(Op0).getValue();
      uint32_t BitWidth = cast<IntegerType>(CE->getType())->getBitWidth();
      GV.IntVal = GV.IntVal.trunc(BitWidth);
      return GV;
    }
    case Instruction::ZExt: {
      GenericValue GV = getConstantValue(Op0).getValue();
      uint32_t BitWidth = cast<IntegerType>(CE->getType())->getBitWidth();
      GV.IntVal = GV.IntVal.zext(BitWidth);
      return GV;
    }
    case Instruction::SExt: {
      GenericValue GV = getConstantValue(Op0).getValue();
      uint32_t BitWidth = cast<IntegerType>(CE->getType())->getBitWidth();
      GV.IntVal = GV.IntVal.sext(BitWidth);
      return GV;
    }
    case Instruction::FPTrunc: {
      // FIXME long double
      GenericValue GV = getConstantValue(Op0).getValue();
      GV.FloatVal = float(GV.DoubleVal);
      return GV;
    }
    case Instruction::FPExt: {
      // FIXME long double
      GenericValue GV = getConstantValue(Op0).getValue();
      GV.DoubleVal = double(GV.FloatVal);
      return GV;
    }
    case Instruction::UIToFP: {
      GenericValue GV = getConstantValue(Op0).getValue();
      if (CE->getType()->isFloatTy())
        GV.FloatVal = float(GV.IntVal.roundToDouble());
      else if (CE->getType()->isDoubleTy())
        GV.DoubleVal = GV.IntVal.roundToDouble();
      else if (CE->getType()->isX86_FP80Ty()) {
        APFloat apf = APFloat::getZero(APFloat::x87DoubleExtended());
        (void)apf.convertFromAPInt(GV.IntVal, false,
                                   APFloat::rmNearestTiesToEven);
        GV.IntVal = apf.bitcastToAPInt();
      }
      return GV;
    }
    case Instruction::SIToFP: {
      GenericValue GV = getConstantValue(Op0).getValue();
      if (CE->getType()->isFloatTy())
        GV.FloatVal = float(GV.IntVal.signedRoundToDouble());
      else if (CE->getType()->isDoubleTy())
        GV.DoubleVal = GV.IntVal.signedRoundToDouble();
      else if (CE->getType()->isX86_FP80Ty()) {
        APFloat apf = APFloat::getZero(APFloat::x87DoubleExtended());
        (void)apf.convertFromAPInt(GV.IntVal, true,
                                   APFloat::rmNearestTiesToEven);
        GV.IntVal = apf.bitcastToAPInt();
      }
      return GV;
    }
    case Instruction::FPToUI: // double->APInt conversion handles sign
    case Instruction::FPToSI: {
      GenericValue GV = getConstantValue(Op0).getValue();
      uint32_t BitWidth = cast<IntegerType>(CE->getType())->getBitWidth();
      if (Op0->getType()->isFloatTy())
        GV.IntVal = APIntOps::RoundFloatToAPInt(GV.FloatVal, BitWidth);
      else if (Op0->getType()->isDoubleTy())
        GV.IntVal = APIntOps::RoundDoubleToAPInt(GV.DoubleVal, BitWidth);
      else if (Op0->getType()->isX86_FP80Ty()) {
        APFloat apf = APFloat(APFloat::x87DoubleExtended(), GV.IntVal);
        uint64_t v;
        bool ignored;
        (void)apf.convertToInteger(makeMutableArrayRef(v), BitWidth,
                                   CE->getOpcode() == Instruction::FPToSI,
                                   APFloat::rmTowardZero, &ignored);
        GV.IntVal = v; // endian?
      }
      return GV;
    }
    case Instruction::PtrToInt: {
      auto OGV = getConstantValue(Op0);
      if (!OGV.hasValue())
        return llvm::None;
      GenericValue GV = OGV.getValue();

      uint32_t PtrWidth = m_td->getTypeSizeInBits(Op0->getType());
      assert(PtrWidth <= 64 && "Bad pointer width");
      GV.IntVal = APInt(PtrWidth, uintptr_t(GV.PointerVal));
      uint32_t IntWidth = m_td->getTypeSizeInBits(CE->getType());
      GV.IntVal = GV.IntVal.zextOrTrunc(IntWidth);
      return GV;
    }
    case Instruction::IntToPtr: {
      GenericValue GV = getConstantValue(Op0).getValue();
      uint32_t PtrWidth = m_td->getTypeSizeInBits(CE->getType());
      GV.IntVal = GV.IntVal.zextOrTrunc(PtrWidth);
      assert(GV.IntVal.getBitWidth() <= 64 && "Bad pointer width");
      GV.PointerVal = PointerTy(uintptr_t(GV.IntVal.getZExtValue()));
      return GV;
    }
    case Instruction::BitCast: {
      GenericValue GV = getConstantValue(Op0).getValue();
      Type *DestTy = CE->getType();
      switch (Op0->getType()->getTypeID()) {
      default:
        llvm_unreachable("Invalid bitcast operand");
      case Type::IntegerTyID:
        assert(DestTy->isFloatingPointTy() && "invalid bitcast");
        if (DestTy->isFloatTy())
          GV.FloatVal = GV.IntVal.bitsToFloat();
        else if (DestTy->isDoubleTy())
          GV.DoubleVal = GV.IntVal.bitsToDouble();
        break;
      case Type::FloatTyID:
        assert(DestTy->isIntegerTy(32) && "Invalid bitcast");
        GV.IntVal = APInt::floatToBits(GV.FloatVal);
        break;
      case Type::DoubleTyID:
        assert(DestTy->isIntegerTy(64) && "Invalid bitcast");
        GV.IntVal = APInt::doubleToBits(GV.DoubleVal);
        break;
      case Type::PointerTyID:
        assert(DestTy->isPointerTy() && "Invalid bitcast");
        break; // getConstantValue(Op0)  above already converted it
      }
      return GV;
    }
    case Instruction::Add:
    case Instruction::FAdd:
    case Instruction::Sub:
    case Instruction::FSub:
    case Instruction::Mul:
    case Instruction::FMul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor: {
      GenericValue LHS = getConstantValue(Op0).getValue();
      GenericValue RHS = getConstantValue(CE->getOperand(1)).getValue();
      GenericValue GV;
      switch (CE->getOperand(0)->getType()->getTypeID()) {
      default:
        llvm_unreachable("Bad add type!");
      case Type::IntegerTyID:
        switch (CE->getOpcode()) {
        default:
          llvm_unreachable("Invalid integer opcode");
        case Instruction::Add:
          GV.IntVal = LHS.IntVal + RHS.IntVal;
          break;
        case Instruction::Sub:
          GV.IntVal = LHS.IntVal - RHS.IntVal;
          break;
        case Instruction::Mul:
          GV.IntVal = LHS.IntVal * RHS.IntVal;
          break;
        case Instruction::UDiv:
          GV.IntVal = LHS.IntVal.udiv(RHS.IntVal);
          break;
        case Instruction::SDiv:
          GV.IntVal = LHS.IntVal.sdiv(RHS.IntVal);
          break;
        case Instruction::URem:
          GV.IntVal = LHS.IntVal.urem(RHS.IntVal);
          break;
        case Instruction::SRem:
          GV.IntVal = LHS.IntVal.srem(RHS.IntVal);
          break;
        case Instruction::And:
          GV.IntVal = LHS.IntVal & RHS.IntVal;
          break;
        case Instruction::Or:
          GV.IntVal = LHS.IntVal | RHS.IntVal;
          break;
        case Instruction::Xor:
          GV.IntVal = LHS.IntVal ^ RHS.IntVal;
          break;
        }
        break;
      case Type::FloatTyID:
        switch (CE->getOpcode()) {
        default:
          llvm_unreachable("Invalid float opcode");
        case Instruction::FAdd:
          GV.FloatVal = LHS.FloatVal + RHS.FloatVal;
          break;
        case Instruction::FSub:
          GV.FloatVal = LHS.FloatVal - RHS.FloatVal;
          break;
        case Instruction::FMul:
          GV.FloatVal = LHS.FloatVal * RHS.FloatVal;
          break;
        case Instruction::FDiv:
          GV.FloatVal = LHS.FloatVal / RHS.FloatVal;
          break;
        case Instruction::FRem:
          GV.FloatVal = std::fmod(LHS.FloatVal, RHS.FloatVal);
          break;
        }
        break;
      case Type::DoubleTyID:
        switch (CE->getOpcode()) {
        default:
          llvm_unreachable("Invalid double opcode");
        case Instruction::FAdd:
          GV.DoubleVal = LHS.DoubleVal + RHS.DoubleVal;
          break;
        case Instruction::FSub:
          GV.DoubleVal = LHS.DoubleVal - RHS.DoubleVal;
          break;
        case Instruction::FMul:
          GV.DoubleVal = LHS.DoubleVal * RHS.DoubleVal;
          break;
        case Instruction::FDiv:
          GV.DoubleVal = LHS.DoubleVal / RHS.DoubleVal;
          break;
        case Instruction::FRem:
          GV.DoubleVal = std::fmod(LHS.DoubleVal, RHS.DoubleVal);
          break;
        }
        break;
      case Type::X86_FP80TyID:
      case Type::PPC_FP128TyID:
      case Type::FP128TyID: {
        const fltSemantics &Sem =
            CE->getOperand(0)->getType()->getFltSemantics();
        APFloat apfLHS = APFloat(Sem, LHS.IntVal);
        switch (CE->getOpcode()) {
        default:
          llvm_unreachable("Invalid long double opcode");
        case Instruction::FAdd:
          apfLHS.add(APFloat(Sem, RHS.IntVal), APFloat::rmNearestTiesToEven);
          GV.IntVal = apfLHS.bitcastToAPInt();
          break;
        case Instruction::FSub:
          apfLHS.subtract(APFloat(Sem, RHS.IntVal),
                          APFloat::rmNearestTiesToEven);
          GV.IntVal = apfLHS.bitcastToAPInt();
          break;
        case Instruction::FMul:
          apfLHS.multiply(APFloat(Sem, RHS.IntVal),
                          APFloat::rmNearestTiesToEven);
          GV.IntVal = apfLHS.bitcastToAPInt();
          break;
        case Instruction::FDiv:
          apfLHS.divide(APFloat(Sem, RHS.IntVal), APFloat::rmNearestTiesToEven);
          GV.IntVal = apfLHS.bitcastToAPInt();
          break;
        case Instruction::FRem:
          apfLHS.mod(APFloat(Sem, RHS.IntVal));
          GV.IntVal = apfLHS.bitcastToAPInt();
          break;
        }
      } break;
      }
      return GV;
    }
    default:
      break;
    }

    SmallString<256> Msg;
    raw_svector_ostream OS(Msg);
    OS << "ConstantExpr not handled: " << *CE;
    report_fatal_error(OS.str());
  }

  // Otherwise, we have a simple constant.
  GenericValue Result;
  switch (C->getType()->getTypeID()) {
  case Type::FloatTyID:
    Result.FloatVal = cast<ConstantFP>(C)->getValueAPF().convertToFloat();
    break;
  case Type::DoubleTyID:
    Result.DoubleVal = cast<ConstantFP>(C)->getValueAPF().convertToDouble();
    break;
  case Type::X86_FP80TyID:
  case Type::FP128TyID:
  case Type::PPC_FP128TyID:
    Result.IntVal = cast<ConstantFP>(C)->getValueAPF().bitcastToAPInt();
    break;
  case Type::IntegerTyID:
    Result.IntVal = cast<ConstantInt>(C)->getValue();
    break;
  case Type::PointerTyID:
    if (isa<ConstantPointerNull>(C))
      Result.PointerVal = nullptr;
    else if (const Function *F = dyn_cast<Function>(C)) {
      // TODO:
      // Result = PTOGV((void*)ctx.getPtrToFunction(*F));
      WARN << "Unhandled function pointer in a constant expression:  " << *C
           << "\n";
      return llvm::None;
    } else if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(C)) {
      // TODO:
      // Result = PTOGV((void*)ctx.getPtrToGlobal(*GV));
      WARN << "Unhandled global variable in a constant expression: " << *C
           << "\n";
      return llvm::None;
    } else
      llvm_unreachable("Unknown constant pointer type!");
    break;
  case Type::VectorTyID: {
    unsigned elemNum;
    Type *ElemTy;
    const ConstantDataVector *CDV = dyn_cast<ConstantDataVector>(C);
    const ConstantVector *CV = dyn_cast<ConstantVector>(C);
    const ConstantAggregateZero *CAZ = dyn_cast<ConstantAggregateZero>(C);

    if (CDV) {
      elemNum = CDV->getNumElements();
      ElemTy = CDV->getElementType();
    } else if (CV || CAZ) {
      VectorType *VTy = dyn_cast<VectorType>(C->getType());
      elemNum = VTy->getNumElements();
      ElemTy = VTy->getElementType();
    } else {
      llvm_unreachable("Unknown constant vector type!");
    }

    Result.AggregateVal.resize(elemNum);
    // Check if vector holds floats.
    if (ElemTy->isFloatTy()) {
      if (CAZ) {
        GenericValue floatZero;
        floatZero.FloatVal = 0.f;
        std::fill(Result.AggregateVal.begin(), Result.AggregateVal.end(),
                  floatZero);
        break;
      }
      if (CV) {
        for (unsigned i = 0; i < elemNum; ++i)
          if (!isa<UndefValue>(CV->getOperand(i)))
            Result.AggregateVal[i].FloatVal =
                cast<ConstantFP>(CV->getOperand(i))
                    ->getValueAPF()
                    .convertToFloat();
        break;
      }
      if (CDV)
        for (unsigned i = 0; i < elemNum; ++i)
          Result.AggregateVal[i].FloatVal = CDV->getElementAsFloat(i);

      break;
    }
    // Check if vector holds doubles.
    if (ElemTy->isDoubleTy()) {
      if (CAZ) {
        GenericValue doubleZero;
        doubleZero.DoubleVal = 0.0;
        std::fill(Result.AggregateVal.begin(), Result.AggregateVal.end(),
                  doubleZero);
        break;
      }
      if (CV) {
        for (unsigned i = 0; i < elemNum; ++i)
          if (!isa<UndefValue>(CV->getOperand(i)))
            Result.AggregateVal[i].DoubleVal =
                cast<ConstantFP>(CV->getOperand(i))
                    ->getValueAPF()
                    .convertToDouble();
        break;
      }
      if (CDV)
        for (unsigned i = 0; i < elemNum; ++i)
          Result.AggregateVal[i].DoubleVal = CDV->getElementAsDouble(i);

      break;
    }
    // Check if vector holds integers.
    if (ElemTy->isIntegerTy()) {
      if (CAZ) {
        GenericValue intZero;
        intZero.IntVal = APInt(ElemTy->getScalarSizeInBits(), 0ull);
        std::fill(Result.AggregateVal.begin(), Result.AggregateVal.end(),
                  intZero);
        break;
      }
      if (CV) {
        for (unsigned i = 0; i < elemNum; ++i)
          if (!isa<UndefValue>(CV->getOperand(i)))
            Result.AggregateVal[i].IntVal =
                cast<ConstantInt>(CV->getOperand(i))->getValue();
          else {
            Result.AggregateVal[i].IntVal = APInt(
                CV->getOperand(i)->getType()->getPrimitiveSizeInBits(), 0);
          }
        break;
      }
      if (CDV)
        for (unsigned i = 0; i < elemNum; ++i)
          Result.AggregateVal[i].IntVal =
              APInt(CDV->getElementType()->getPrimitiveSizeInBits(),
                    CDV->getElementAsInteger(i));

      break;
    }
    llvm_unreachable("Unknown constant pointer type!");
  } break;

  default:
    SmallString<256> Msg;
    raw_svector_ostream OS(Msg);
    OS << "ERROR: Constant unimplemented for type: " << *C->getType();
    report_fatal_error(OS.str());
  }

  return Result;
}

void Bv2OpSem::execEdg(const BasicBlock &src, const BasicBlock &dst,
                       details::Bv2OpSemContext &ctx) {
  exec(src, ctx.pc(trueE));
  execBr(src, dst, ctx);
  execPhi(dst, src, ctx);

  // an edge into a basic block that does not return includes the block itself
  const TerminatorInst *term = dst.getTerminator();
  if (term && isa<const UnreachableInst>(term))
    exec(dst, ctx);
}

void Bv2OpSem::execBr(const BasicBlock &src, const BasicBlock &dst,
                      details::Bv2OpSemContext &ctx) {
  ctx.onBasicBlockEntry(src);
  ctx.setInstruction(*src.getTerminator());
  intraBr(ctx, dst);
}

} // namespace seahorn

namespace seahorn {
namespace details {
/// \brief Unwraps a context
seahorn::details::Bv2OpSemContext &ctx(OpSemContext &_ctx) {
  return static_cast<seahorn::details::Bv2OpSemContext &>(_ctx);
}
} // namespace details
} // namespace seahorn
namespace {
// \brief Unwraps a const context
const seahorn::details::Bv2OpSemContext &const_ctx(const OpSemContext &_ctx) {
  return static_cast<const seahorn::details::Bv2OpSemContext &>(_ctx);
}
} // namespace
