// This Clang plugin reports several patterns in the source code:
//
// * Calls to Register_Command and Register_CommandWithPayload are
//   logged, together with key argument values.
//
// * Calls to sprintf() and vsprintf() from the libc are logged.  This
//   is helpful because Condor overloads sprintf() with a safer
//   variant in which we are not interested.
//
// * Calls to the Condor-specific overloads of sprintf() and
//   vsprintf() are recorded (as "sprintf-overload").
//
// * Calls to strcpy/strcat which copy buffers into arguments are
//   logged because such programming patterns are usually insecure
//   (similar to gets()).
//
// The results are stored in an SQLite database
// "condor-analyzer.sqlite", which must be located in a parent
// directory.  This database has to be created manually, using the
// ./create-db utility.
//
// Clang has to be invoked this way:
//
//   clang++ -Xclang -load -Xclang plugin.so
//     -Xclang -plugin -Xclang condor-analysis ...
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

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_os_ostream.h"

using namespace clang;

namespace {

void
FatalError(DiagnosticsEngine &D, const std::string &message)
{
  unsigned Fatal = D.getCustomDiagID
    (DiagnosticsEngine::Fatal, "(condor-analysis) " + message);
  D.Report(Fatal);
}

void
FatalError(DiagnosticsEngine &D, SourceLocation Pos, const std::string &message)
{
  unsigned Fatal = D.getCustomDiagID
    (DiagnosticsEngine::Fatal, "(condor-analysis) " + message);
  D.Report(Pos, Fatal);
}

template <class Visitor>
class ConsumerFromVisitor : public ASTConsumer {
  std::shared_ptr<FileIdentificationDatabase> FileDB;

public:
  ConsumerFromVisitor(std::shared_ptr<FileIdentificationDatabase> DB)
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
    for (auto FI = SrcMan.fileinfo_begin(), end = SrcMan.fileinfo_end();
	 FI != end; ++FI) {
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
  std::shared_ptr<FileIdentificationDatabase> FileDB;

public:
  OuterVisitor(std::shared_ptr<FileIdentificationDatabase> DB, ASTContext &C)
    : Context(C), FileDB(DB)
  {
  }

  bool shouldVisitTemplateInstantiations() const { return true; }

  bool VisitCXXMemberCallExpr(CXXMemberCallExpr *Expr)
  {
    ProcessSizeofCallExpr(Expr);
    return ProcessRegisterCommand(Expr);
  }

  bool VisitCallExpr(CallExpr *Expr)
  {
    ProcessSizeofCallExpr(Expr);
    return ProcessSprintf(Expr) | ProcessStrcpy(Expr);
  }

  bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *Expr)
  {
    return ProcessMyStringOperator(Expr);
  }

private:
  ////////////////////////////////////////////////////////////////////
  // Register_Command

  bool ProcessRegisterCommand(CXXMemberCallExpr *Expr)
  {
    if (const CXXMethodDecl *MethodDecl = getMethodDecl(Expr)) {
      std::string MethodName(MethodDecl->getNameAsString());
      if (MethodName == "Register_Command"
	  || MethodName == "Register_CommandWithPayload") {
	unsigned numArgs = Expr->getNumArgs();
	if (numArgs < 5 ) {
	  Report(Expr->getExprLoc(), "Register_Command",
		 "call without enough arguments");
	  return true;
	}

	llvm::APSInt command(0);
	if (!Expr->getArg(0)->EvaluateAsInt(command, Context)) {
	  Report(Expr->getExprLoc(), "Register_Command",
		 "call with non-constant command");
	  return true;
	}

	llvm::APSInt perm(0);	// default is ALLOW
	if (numArgs >= 6) {
	  if (!Expr->getArg(5)->EvaluateAsInt(perm, Context)) {
	    Report(Expr->getExprLoc(), "Register_Command",
		   "call with non-constant perm");
	    return true;
	  }
	}

	bool forceAuthentication = false;
	if (numArgs >= 8) {
	  if (!Expr->getArg(7)->EvaluateAsBooleanCondition
	      (forceAuthentication, Context)
	      && !Expr->getArg(7)->isDefaultArgument()) {
	    Report(Expr->getExprLoc(), "Register_Command",
		   "call with non-constant force_authentication");
	    return true;
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
    return true;
  }

  ////////////////////////////////////////////////////////////////////
  // sprintf

  enum class SprintfTarget : int {
    None, CharPtr, MyString, StdString, Other,
      };

  bool ProcessSprintf(CallExpr *Expr)
  {
    if (FunctionDecl *Decl = Expr->getDirectCallee()) {
      std::string FunctionName = Decl->getNameAsString();
      if (isSprintfName(FunctionName)) {
	SprintfTarget Target = getSprintfTarget(Decl);
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
    return true;
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

  static SprintfTarget getSprintfTarget(FunctionDecl *Decl) {
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
      if (auto StructType = dyn_cast<RecordType>(RefedType)) {
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

  bool ProcessStrcpy(CallExpr *Expr)
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
    return true;
  }

  static bool isStrcpyName(const std::string &Name) {
    return Name == "strcpy" || Name == "strcat";
  }

  static bool parameterNameInArgument(CallExpr *Expr, std::string &name) {
    if (Expr->getNumArgs() < 2) {
      return false;
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

    } Visitor(name);
    Visitor.TraverseStmt(Expr->getArg(0));
    return Visitor.Parameter;
  }

  ////////////////////////////////////////////////////////////////////
  // MyString

  bool ProcessMyStringOperator(CXXOperatorCallExpr *Expr)
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

    return true;
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
    std::map<unsigned, const Expr *> SizeofArguments; 

    // Find sizeof with pointer arguments.
    // ??? This shoud generalize to sizeofs of non-array arguments
    // ??? and we could check if the same expression is used
    // ??? in a non-reference-taking context in the parameter list.
    unsigned NumArgs = Call->getNumArgs();
    for (unsigned i = 0; i < NumArgs; ++i) {
      auto ArgExpr = ExtractSizeofPointer(Call->getArg(i));
      if (ArgExpr != nullptr) {
	SizeofArguments[i] = ArgExpr;
      }
    }

    // Check for exact matches with other arguments.
    auto end = SizeofArguments.end();
    if (!SizeofArguments.empty()) {
      for (unsigned i = 0; i < NumArgs; ++i) {
	for (auto p = SizeofArguments.begin(); p != end; ++p) {
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

  const Expr *ExtractSizeofPointer(const Expr *E)
  {
    if (auto SE = dyn_cast<UnaryExprOrTypeTraitExpr>(E->IgnoreParenCasts())) {
      if (SE->getKind() == UETT_SizeOf && !SE->isArgumentType()) {
	E = SE->getArgumentExpr()->IgnoreParenCasts();
	if (E->getType()->isPointerType()) {
	  return E;
	}
      }
    }
    return nullptr;
  }

  bool EquivalentExpr(const Expr *L, const Expr *R)
  {
    L = L->IgnoreParenCasts();
    R = R->IgnoreParenCasts();
    
    if (auto LD = dyn_cast<DeclRefExpr>(L)) {
      if (auto RD = dyn_cast<DeclRefExpr>(R)) {
	return LD->getDecl() == RD->getDecl();
      }
      return false;
    }
    if (auto LO = dyn_cast<UnaryOperator>(L)) {
      if (auto RO = dyn_cast<UnaryOperator>(R)) {
	return LO->getOpcode() == RO->getOpcode()
	  && EquivalentExpr(LO->getSubExpr(), RO->getSubExpr());
      }
      return false;
    }
    if (auto LO = dyn_cast<BinaryOperator>(L)) {
      if (auto RO = dyn_cast<BinaryOperator>(R)) {
	return LO->getOpcode() == RO->getOpcode()
	  && EquivalentExpr(LO->getLHS(), RO->getLHS())
	  && EquivalentExpr(LO->getRHS(), RO->getRHS());
      }
      return false;
    }
    return false;
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

    if (Location.isFileID()) {
      PresumedLoc PLoc = Context.getSourceManager().getPresumedLoc(Location);
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
  std::shared_ptr<FileIdentificationDatabase> FileDB;

protected:
  ASTConsumer *CreateASTConsumer(CompilerInstance &, llvm::StringRef) {
    return new ConsumerFromVisitor<OuterVisitor>(FileDB);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string>& args) {

    if (args.size() && args[0] == "help") {
      PrintHelp(llvm::errs());
    }
    
    auto DB = std::make_shared<Database>();
    if (!DB->Open()) {
      FatalError(CI.getDiagnostics(), DB->ErrorMessage);
      return false;
    }
    FileDB = std::make_shared<FileIdentificationDatabase>(DB);
    return true;
  }
  void PrintHelp(llvm::raw_ostream& ros) {
    ros << "Analyse Condor source code\n";
  }

};

static FrontendPluginRegistry::Add<Action>
X("condor-analysis", "Condor source code analysis");

}
