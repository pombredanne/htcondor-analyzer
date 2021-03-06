/*
 * Copyright (C) 2012 Red Hat, Inc.
 * Written by Florian Weimer <fweimer@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// This Clang plugin reports several patterns in the source code:
//
// * Calls to Register_Command and Register_CommandWithPayload are
//   logged, together with key argument values.
//
// * Calls to sprintf() and vsprintf() from the libc are logged.  This
//   is helpful because HTCondor overloads sprintf() with a safer
//   variant in which we are not interested.  (These overload have
//   since been removed from HTCondor.)
//
// * Calls to the HTCondor-specific overloads of sprintf() and
//   vsprintf() are recorded (as "sprintf-overload").
//
// * Calls to strcpy/strcat which copy buffers into arguments are
//   logged because such programming patterns are usually insecure
//   (similar to gets()).
//
// * References to operator[] on standard templates which are not
//   bounds-checked are flagged with "operator[]" or "operator[]
//   const".
//
// * Pointer arithmetic is flagged as "pointer-arith".
//
// * Calls to alloca are reported as "alloca".
//
// * Local variables which are declared static and not const are
//   reported as "static-local".
//
// The results are stored in an SQLite database
// "htcondor-analyzer.sqlite", which must be located in a parent
// directory.  This database has to be created manually, using the
// ./create-db utility.
//
// Clang has to be invoked this way:
//
//   clang++ -Xclang -load -Xclang plugin.so
//     -Xclang -plugin -Xclang htcondor-analysis ...
//
// Or use the ./cxx wrapper in this directory.
//
// Florian Weimer / Red Hat Product Security Team

#include "db-file.hpp"

#include <sstream>
#include <memory>
#include <map>

#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/AST.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/FileManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Basic/Builtins.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_os_ostream.h"

using namespace clang;

namespace {

void
FatalError(DiagnosticsEngine &D, const std::string &message)
{
  unsigned Fatal = D.getCustomDiagID
    (DiagnosticsEngine::Fatal, "(htcondor-analysis) " + message);
  D.Report(Fatal);
}

void
FatalError(DiagnosticsEngine &D, SourceLocation Pos, const std::string &message)
{
  unsigned Fatal = D.getCustomDiagID
    (DiagnosticsEngine::Fatal, "(htcondor-analysis) " + message);
  D.Report(Pos, Fatal);
}

template <class Visitor>
class ConsumerFromVisitor : public ASTConsumer {
  std::tr1::shared_ptr<FileIdentificationDatabase> FileDB;

public:
  ConsumerFromVisitor(std::tr1::shared_ptr<FileIdentificationDatabase> DB)
    : FileDB(DB)
  {
  }

  virtual void HandleTranslationUnit(ASTContext &Context) {
    if (Context.getDiagnostics().hasErrorOccurred()) {
      return;
    }
    RecordFiles(Context);
    Visitor visitor(FileDB, Context);
    visitor.TraverseDecl(Context.getTranslationUnitDecl());
    if (Context.getDiagnostics().hasErrorOccurred()) {
      return;
    }
    if (!FileDB->Commit()) {
      FatalError(Context.getDiagnostics(),
		 "commit: " + FileDB->ErrorMessage());
    }
  }

  void RecordFiles(ASTContext &Context)
  {
    // TODO: Preload entries to support AST dumps/pre-compiled
    // headers.
    const SourceManager &SrcMan = Context.getSourceManager();
    for (SourceManager::fileinfo_iterator FI = SrcMan.fileinfo_begin(),
	   end = SrcMan.fileinfo_end(); FI != end; ++FI) {
      SrcMgr::ContentCache *CCache = FI->second;
      const FileEntry *FEntry = CCache->ContentsEntry;
      if (!FEntry) {
	FEntry = CCache->OrigEntry;
      }
      if (!FEntry) {
	continue;
      }
      FileDB->MarkForProcessing(FEntry->getName());
    }
  }
};



//////////////////////////////////////////////////////////////////////
// OuterVisitor

class OuterVisitor : public RecursiveASTVisitor<OuterVisitor> {
  ASTContext &Context;
  std::tr1::shared_ptr<FileIdentificationDatabase> FileDB;

public:
  OuterVisitor(std::tr1::shared_ptr<FileIdentificationDatabase> DB, ASTContext &C)
    : Context(C), FileDB(DB)
  {
  }

  bool shouldVisitTemplateInstantiations() const { return true; }

  bool VisitCXXMemberCallExpr(CXXMemberCallExpr *Expr)
  {
    ProcessSizeofCallExpr(Expr);
    ProcessRegisterCommand(Expr);
    return true;
  }

  bool VisitCallExpr(CallExpr *Expr)
  {
    ProcessSizeofCallExpr(Expr);
    ProcessSprintf(Expr);
    ProcessStrcpy(Expr);
    ProcessAlloca(Expr);
    return true;
  }

  bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *Expr)
  {
    ProcessMyStringOperator(Expr);
    StandardLibraryProcessSubscript(Expr);
    return true;
  }

  bool VisitUnaryOperator(UnaryOperator *Expr)
  {
    PointerArithProcessUnaryOperator(Expr);
    return true;
  }

  bool VisitBinaryOperator(BinaryOperator *Expr)
  {
    PointerArithProcessBinaryOperator(Expr);
    return true;
  }

  bool VisitArraySubscriptExpr(ArraySubscriptExpr *Expr)
  {
    PointerArithProcessSubscript(Expr);
    return true;
  }

  bool VisitVarDecl(VarDecl *Decl)
  {
    ProcessStaticLocal(Decl);
    return true;
  }

private:
  ////////////////////////////////////////////////////////////////////
  // Register_Command

  void ProcessRegisterCommand(CXXMemberCallExpr *Expr)
  {
    if (const CXXMethodDecl *MethodDecl = getMethodDecl(Expr)) {
      std::string MethodName(MethodDecl->getNameAsString());
      if (MethodName == "Register_Command"
	  || MethodName == "Register_CommandWithPayload") {
	unsigned numArgs = Expr->getNumArgs();
	if (numArgs < 5 ) {
	  Report(Expr->getExprLoc(), "Register_Command",
		 "call without enough arguments");
	  return;
	}

	llvm::APSInt command;
	if (!Expr->getArg(0)->EvaluateAsInt(command, Context)) {
	  Report(Expr->getExprLoc(), "Register_Command",
		 "call with non-constant command");
	  return;
	}

	llvm::APSInt perm;	// default is ALLOW
	if (numArgs >= 6) {
	  if (!Expr->getArg(5)->EvaluateAsInt(perm, Context)) {
	    Report(Expr->getExprLoc(), "Register_Command",
		   "call with non-constant perm");
	    return;
	  }
	}

	bool forceAuthentication = false;
	if (numArgs >= 8) {
	  if (!Expr->getArg(7)->EvaluateAsBooleanCondition
	      (forceAuthentication, Context)
	      && !Expr->getArg(7)->isDefaultArgument()) {
	    Report(Expr->getExprLoc(), "Register_Command",
		   "call with non-constant force_authentication");
	    return;
	  }
	}

	std::ostringstream ostr;
	{
	  llvm::raw_os_ostream message(ostr);
	  message << MethodName
		  << " command=" << command << " perm=" << perm
		  << " auth=" << (forceAuthentication ? "true" : "false");
	}
	Report(Expr->getExprLoc(), "Register_Command", ostr.str());
      } else if (isSprintfName(MethodName)) {
	ProcessSprintfMemberCall(Expr, MethodDecl, MethodName);
      }
    }
  }

  ////////////////////////////////////////////////////////////////////
  // sprintf

  struct SprintfTarget {
    typedef enum {
      None, CharPtr, MyString, StdString, Other
    } Enum;
  private:
    SprintfTarget();
    ~SprintfTarget();
  };

  void ProcessSprintf(CallExpr *Expr)
  {
    if (FunctionDecl *Decl = Expr->getDirectCallee()) {
      std::string FunctionName = Decl->getNameAsString();
      if (isSprintfName(FunctionName)) {
	SprintfTarget::Enum Target = getSprintfTarget(Decl);
	switch (Target) {
	case SprintfTarget::None:
	  break;
	case SprintfTarget::CharPtr:
	  Report(Expr->getExprLoc(), "sprintf", FunctionName);
	  break;
	case SprintfTarget::MyString:
	  Report(Expr->getExprLoc(), "sprintf-overload",
		 FunctionName + "(MyString)");
	  break;
	case SprintfTarget::StdString:
	  Report(Expr->getExprLoc(), "sprintf-overload",
		 FunctionName + "(std::string)");
	  break;
	case SprintfTarget::Other:
	  {
	    std::ostringstream ostr;
	    ostr << FunctionName << '(';
	    {
	      llvm::raw_os_ostream OS(ostr);
#if 0
	      std::unique_ptr<ASTConsumer> Printer
		(Context.CreateASTPrinter(OS));
	      Printer->TraverseParamVarDecl(*Decl->param_begin);
#else
	      OS << "<unknown>";
#endif
	    }
	    ostr << ')';
	    Report(Expr->getExprLoc(), "sprintf-overload", ostr.str());
	  }
	  break;
	}
      }
    }
  }

  void ProcessSprintfMemberCall(const CXXMemberCallExpr *Expr,
				const CXXMethodDecl *Decl,
				const std::string &MethodName)
  {
    Report(Expr->getCallee()->getExprLoc(),
	   "sprintf-overload", MethodName + "(" +
	   Decl->getParent()->getQualifiedNameAsString() + ")");
  }

  static bool isSprintfName(const std::string &Name) {
    return Name == "sprintf" || Name == "vsprintf";
  }

  static SprintfTarget::Enum getSprintfTarget(FunctionDecl *Decl) {
    if (Decl->getNumParams() < 2) {
      return SprintfTarget::None;
    }
    ParmVarDecl *First = *Decl->param_begin();
    QualType FirstType = First->getType().getCanonicalType();
    if (FirstType->isPointerType()
	&& FirstType->getPointeeType()->isCharType()) {
      return SprintfTarget::CharPtr;
    }
    if (FirstType->isReferenceType()) {
      QualType RefedType = FirstType->getPointeeType();
      if (const RecordType *StructType = dyn_cast<RecordType>(RefedType)) {
	RecordDecl *TypeDecl = StructType->getDecl();
	std::string Name = TypeDecl->getQualifiedNameAsString();
	if (Name == "MyString" ) {
	  return SprintfTarget::MyString;
	} else if (Name == "std::basic_string") {
	  return SprintfTarget::StdString;
	}
      }
    }
    return SprintfTarget::Other;
  }

  ////////////////////////////////////////////////////////////////////
  // strcpy/strcat

  void ProcessStrcpy(CallExpr *Expr)
  {
    if (FunctionDecl *Decl = Expr->getDirectCallee()) {
      std::string FunctionName = Decl->getNameAsString();
      std::string ParameterName;
      if (isStrcpyName(FunctionName)
	  && parameterNameInArgument(Expr, ParameterName)) {
	Report(Expr->getExprLoc(), "strcpy",
	       FunctionName + '(' + ParameterName + ')');
      }
    }
  }

  static bool isStrcpyName(const std::string &Name) {
    return Name == "strcpy" || Name == "strcat" || Name == "sprintf";
  }

  struct ParameterNameVisitor : RecursiveASTVisitor<ParameterNameVisitor> {
    std::string &Name;
    bool Parameter;

    ParameterNameVisitor(std::string &name)
      : Name(name), Parameter(false)
    {
    }

    bool VisitDeclRefExpr(DeclRefExpr *Expr) {
      if (!Parameter) {
	if (ParmVarDecl *Parm = dyn_cast<ParmVarDecl>(Expr->getDecl())) {
	  Parameter = true;
	  Name = Parm->getNameAsString();
	}
      }
      return true;
    }

    bool VisitUnaryDeref(UnaryOperator *) {
      // If we copy into a dereferenced pointer, we likely have a
      // false positive because the pointee might have been
      // allocated by us.
      return false;
    }

    bool VisitMemberExpr(MemberExpr *Expr) {
      // -> dereference is also a pointer dereference.
      return !Expr->isArrow();
    }
  };

  static bool parameterNameInArgument(CallExpr *Expr, std::string &name) {
    if (Expr->getNumArgs() < 2) {
      return false;
    }
    ParameterNameVisitor Visitor(name);
    Visitor.TraverseStmt(Expr->getArg(0));
    return Visitor.Parameter;
  }

  ////////////////////////////////////////////////////////////////////
  // MyString

  void ProcessMyStringOperator(CXXOperatorCallExpr *Expr)
  {
    QualType Type;
    switch (Expr->getOperator()) {
    case OO_Subscript:
      if (matchTypeAgainstMyString(Expr, Type)) {
	std::string message("operator[] ");
	if (!Type.isConstQualified()) {
	  message += "non-";
	}
	message += "const";
	Report(Expr->getExprLoc(), "MyString", message);
      }
      break;
    case OO_PlusEqual:
      if (matchTypeAgainstMyString(Expr, Type)) {
	Type = Expr->getArg(1)->getType().getCanonicalType()
	  .getUnqualifiedType();
	std::string TypeName(Type.getAsString());
	if (TypeName != "char"
	    && TypeName != "const char *" 
	    && TypeName != "class MyString") {
	  Report(Expr->getExprLoc(), "MyString", "operator+= " + TypeName);
	}
      }
      break;
    default:
      ;
    }
  }

  // Returns true if the unqualified type is class MyString, and
  // and stores the potentially qualified type in the reference.
  bool matchTypeAgainstMyString(CXXOperatorCallExpr *Expr, QualType &Type)
  {
    Type = Expr->getArg(0)->getType();
    QualType UType = Type.getCanonicalType().getUnqualifiedType();
    return UType.getAsString() == "class MyString";
  }

  ////////////////////////////////////////////////////////////////////
  // sizeof and pointers

  void ProcessSizeofCallExpr(CallExpr *Call)
  {
    // indexed by argument position
    typedef std::map<unsigned, const Expr *> ArgMap;
    ArgMap SizeofArguments; 

    // Find sizeof with pointer arguments.
    // ??? This shoud generalize to sizeofs of non-array arguments
    // ??? and we could check if the same expression is used
    // ??? in a non-reference-taking context in the parameter list.
    unsigned NumArgs = Call->getNumArgs();
    for (unsigned i = 0; i < NumArgs; ++i) {
      const Expr *ArgExpr = ExtractSizeofPointer(Call->getArg(i));
      if (ArgExpr != NULL) {
	SizeofArguments[i] = ArgExpr;
      }
    }
    
    // Check for exact matches with other arguments.
    ArgMap::iterator end = SizeofArguments.end();
    if (!SizeofArguments.empty()) {
      for (unsigned i = 0; i < NumArgs; ++i) {
	for (ArgMap::iterator p = SizeofArguments.begin(); p != end; ++p) {
	  if (p->first == i) {
	    continue;
	  }
	  if (EquivalentExpr(Call->getArg(i), p->second)) {
	    std::ostringstream ostr;
	    ostr << "pointer=" << i << ", sizeof=" << p->first;
	    Report(Call->getArg(p->first)->getExprLoc(),
		   "sizeof-pointer", ostr.str());
	    return;
	  }
	}
      }
    }
  }

  // Recognizes sizeof(ptr) and sizeof(ptr)-expr.
  const Expr *ExtractSizeofPointer(const Expr *E)
  {
    E = E->IgnoreParenCasts();
    if (const UnaryExprOrTypeTraitExpr *SE = dyn_cast<UnaryExprOrTypeTraitExpr>(E)) {
      if (SE->getKind() == UETT_SizeOf && !SE->isArgumentType()) {
	E = SE->getArgumentExpr()->IgnoreParenCasts();
	if (E->getType()->isPointerType()) {
	  return E;
	}
      }
    } else if (const BinaryOperator *O = dyn_cast<BinaryOperator>(E)) {
      if (O->getOpcode() == BO_Sub) {
	E = ExtractSizeofPointer(O->getLHS());
	if (E != NULL) {
	  return E;
	}
      }
    }
    return NULL;
  }

  bool EquivalentExpr(const Expr *L, const Expr *R)
  {
    L = L->IgnoreParenCasts();
    R = R->IgnoreParenCasts();
    
    if (const DeclRefExpr *LD = dyn_cast<DeclRefExpr>(L)) {
      if (const DeclRefExpr *RD = dyn_cast<DeclRefExpr>(R)) {
	return LD->getDecl() == RD->getDecl();
      }
      return false;
    }
    if (const UnaryOperator *LO = dyn_cast<UnaryOperator>(L)) {
      if (const UnaryOperator *RO = dyn_cast<UnaryOperator>(R)) {
	return LO->getOpcode() == RO->getOpcode()
	  && EquivalentExpr(LO->getSubExpr(), RO->getSubExpr());
      }
      return false;
    }
    if (const BinaryOperator *LO = dyn_cast<BinaryOperator>(L)) {
      if (const BinaryOperator *RO = dyn_cast<BinaryOperator>(R)) {
	return LO->getOpcode() == RO->getOpcode()
	  && EquivalentExpr(LO->getLHS(), RO->getLHS())
	  && EquivalentExpr(LO->getRHS(), RO->getRHS());
      }
      return false;
    }
    return false;
  }

  ////////////////////////////////////////////////////////////////////
  // Pointer arithmetic

  // TODO: Catch simple subscripts which are statically within array
  // bounds.

  void PointerArithProcessUnaryOperator(UnaryOperator *Expr)
  {
    UnaryOperatorKind Kind = Expr->getOpcode();
    const char *KindStr;
    switch (Kind) {
    case UO_PostInc:
    case UO_PreInc:
      KindStr = "inplace-add";
      break;
    case UO_PostDec:
    case UO_PreDec:
      KindStr = "inplace-sub";
      break;
    default:
      KindStr = NULL;
    }
    if (KindStr != NULL && Expr->getType()->isPointerType()) {
      Report(Expr->getExprLoc(), "pointer-arith", KindStr);
    }
  }

  void PointerArithProcessBinaryOperator(BinaryOperator *Expr)
  {
    BinaryOperatorKind Kind = Expr->getOpcode();
    const char *KindStr;
    switch (Kind) {
    case BO_Add:
      KindStr = "add";
      break;
    case BO_Sub:
      KindStr = "sub";
      break;
    case BO_AddAssign:
      KindStr = "inplace-add";
      break;
    case BO_SubAssign:
      KindStr = "inplace-sub";
      break;
    default:
      KindStr = NULL;
    }
    if (KindStr != NULL) {
      unsigned Pointers = 0;
      if (Expr->getLHS()->getType()->isPointerType()
	  || Expr->getLHS()->getType()->isArrayType()) {
	++Pointers;
      }
      if (Expr->getRHS()->getType()->isPointerType()
	  || Expr->getRHS()->getType()->isArrayType()) {
	++Pointers;
      }
      switch (Pointers) {
      case 1:
	Report(Expr->getExprLoc(), "pointer-arith", KindStr);
	break;
      case 2:
	Report(Expr->getExprLoc(), "pointer-arith", "diff");
	break;
      }
    }
  }

  void PointerArithProcessSubscript(ArraySubscriptExpr *E)
  {
    Expr *Subscript = NULL;
    if (E->getLHS()->getType()->isPointerType()
	|| E->getLHS()->getType()->isArrayType()) {
      Subscript = E->getRHS();
    } else if (E->getRHS()->getType()->isPointerType()
	       || E->getRHS()->getType()->isArrayType()) {
      Subscript = E->getLHS();
    }
    
    if (Subscript != NULL) {
      llvm::APSInt I;
      if (!(Subscript->EvaluateAsInt(I, Context) && I == 0)) {
	Report(E->getExprLoc(), "pointer-arith", "subscript");
      }
    }
  }

  ////////////////////////////////////////////////////////////////////
  // Array subscripts without bounds checks

  void StandardLibraryProcessSubscript(CXXOperatorCallExpr *Expr)
  {
    if (Expr->getOperator() == OO_Subscript) {
      QualType Type;
      if (matchTypeAgainstVectorOrString(Expr, Type)) {
	const char *message =
	  Type.isConstQualified() ? "operator[] const" : "operator[]";
	Report(Expr->getExprLoc(), message, Type.getAsString());
      }
    }
  }

  // Returns true if the unqualified type is an instance of the
  // standard library templates vector or basic_string.
  bool matchTypeAgainstVectorOrString(CXXOperatorCallExpr *Expr, QualType &Type)
  {
    Type = Expr->getArg(0)->getType();
    QualType UType = Type.getCanonicalType().getUnqualifiedType();
    if (const RecordType *RType = dyn_cast<RecordType>(UType.getTypePtr())) {
      if (ClassTemplateSpecializationDecl *Decl =
	  dyn_cast<ClassTemplateSpecializationDecl>(RType->getDecl())) {
	const std::string Name
	  (Decl->getSpecializedTemplate()->getQualifiedNameAsString());
	return Name == "std::vector"
	  || Name == "std::basic_string"
	  || Name == "std::array";
      }
    }
    return false;
  }

  //////////////////////////////////////////////////////////////////////
  // alloca

  void ProcessAlloca(CallExpr *E)
  {
    if (Expr *Callee = E->getCallee()->IgnoreParenCasts()) {
      if (DeclRefExpr *Ref = dyn_cast<DeclRefExpr>(Callee)) {
	if (FunctionDecl *FD = dyn_cast<FunctionDecl>(Ref->getDecl())) {
	  unsigned BuiltinID = FD->getBuiltinID();
	  if (BuiltinID) {
	    Builtin::Context Ctx;
	  }
	  if (BuiltinID == Builtin::BI__builtin_alloca
	      || BuiltinID == Builtin::BIalloca) {
	    Report(E->getExprLoc(), "alloca", "x");
	  }
	}
      }
    }
  }

  //////////////////////////////////////////////////////////////////////
  // Static local variables

  void ProcessStaticLocal(VarDecl *Decl)
  {
    if (Decl->isStaticLocal() && !Decl->getType().isConstQualified()) {
      Report(Decl->getLocation(), "static-local", Decl->getNameAsString());
    }
  }


  ////////////////////////////////////////////////////////////////////
  // Helpers

  void Report(SourceLocation Location, const char *Tool,
	      const std::string &Message)
  {
    if (!Location.isValid()) {
      FatalError(Context.getDiagnostics(),
		 "attempt to report at an invalid source location");
      return;
    }

    if (!FileDB->isOpen()) {
      return;
    }

    // This obtains the source code location of the outmost macro call.
    SourceManager &SM(Context.getSourceManager());
    SourceLocation OuterLocation = Location;
    while (OuterLocation.isMacroID()) {
      OuterLocation = SM.getImmediateMacroCallerLoc(OuterLocation);
    }
    PresumedLoc PLoc = SM.getPresumedLoc(OuterLocation);
    if (PLoc.isInvalid()) {
      FatalError(Context.getDiagnostics(),
		 "attempt to report at an invalid presumed location");
      return;
    }

    const char *FileName = PLoc.getFilename();
    unsigned Line = PLoc.getLine();
    unsigned Column = PLoc.getColumn();
    if (!FileDB->Report(FileName, Line, Column, Tool, Message)) {
      FatalError(Context.getDiagnostics(), Location,
		 "could not report: " + FileDB->ErrorMessage());
      return;
    }
  }

  static CXXMethodDecl *getMethodDecl(const CXXMemberCallExpr *Expr) {
    if (const MemberExpr *MemExpr = 
	dyn_cast<MemberExpr>(Expr->getCallee()->IgnoreParens())) {
      return cast<CXXMethodDecl>(MemExpr->getMemberDecl());
    }   
    return 0;
  }
};

class Action : public PluginASTAction {
  std::tr1::shared_ptr<FileIdentificationDatabase> FileDB;

protected:
  ASTConsumer *CreateASTConsumer(CompilerInstance &, llvm::StringRef) {
    return new ConsumerFromVisitor<OuterVisitor>(FileDB);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string>& args) {

    if (args.size() && args[0] == "help") {
      PrintHelp(llvm::errs());
    }
    
    std::tr1::shared_ptr<Database> DB(new Database);
    if (!DB->Open()) {
      FatalError(CI.getDiagnostics(), DB->ErrorMessage);
      return false;
    }
    FileDB.reset(new FileIdentificationDatabase(DB));
    return true;
  }
  void PrintHelp(llvm::raw_ostream& ros) {
    ros << "Analyse HTCondor source code\n";
  }

};

static FrontendPluginRegistry::Add<Action>
X("htcondor-analysis", "Condor source code analysis");

}
