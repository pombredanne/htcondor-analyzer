// This Clang plugin reports several patterns in the source code:
//
// * Calls to Register_Command and Register_CommandWithPayload are
//   logged, together with key argument values.
//
// * Calls to sprintf() and vsprintf() from the libc are logged.  This
//   is helpful because Condor overloads sprintf() with a safer
//   variant in which we are not interested.
//
// * Calls to strcpy/strcat which copy buffers into arguments are
//   logged because such programming patterns are usually insecure
//   (similar to gets()).
//
// The results are stored in an SQLite database
// "condor-analysis.sqlite", which must be located in a parent
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

#include "db.hpp"

#include <sstream>

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
    Visitor visitor(FileDB, Context);
    RecordFiles(Context);
    visitor.TraverseDecl(Context.getTranslationUnitDecl());
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
      if (!FileDB->MarkForProcessing(FEntry->getName())) {
	FatalError(Context.getDiagnostics(), FileDB->DB->ErrorMessage);
	break;
      }
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

  bool TraverseCXXMemberCallExpr(CXXMemberCallExpr *Expr)
  {
    return ProcessRegisterCommand(Expr);
  }

  bool TraverseCallExpr(CallExpr *Expr)
  {
    return ProcessSprintf(Expr) | ProcessStrcpy(Expr);
  }

  bool TraverseCXXOperatorCallExpr(CXXOperatorCallExpr *Expr)
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
      }
    }
    return true;
  }

  ////////////////////////////////////////////////////////////////////
  // sprintf

  bool ProcessSprintf(CallExpr *Expr)
  {
    if (FunctionDecl *Decl = Expr->getDirectCallee()) {
      std::string FunctionName = Decl->getNameAsString();
      if (isSprintfName(FunctionName) && hasCharBuffer(Decl)) {
	Report(Expr->getExprLoc(), "sprintf", FunctionName);
      }
    }
    return true;
  }

  static bool isSprintfName(const std::string &Name) {
    return Name == "sprintf" || Name == "vsprintf";
  }

  static bool hasCharBuffer(FunctionDecl *Decl) {
    if (Decl->getNumParams() < 2) {
      return false;
    }
    ParmVarDecl *First = *Decl->param_begin();
    QualType FirstType = First->getType().getCanonicalType();
    return FirstType->isPointerType()
      && FirstType->getPointeeType()->isCharType();
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

      bool TraverseDeclRefExpr(DeclRefExpr *Expr) {
	if (!Parameter) {
	  if (ParmVarDecl *Parm = dyn_cast<ParmVarDecl>(Expr->getDecl())) {
	    Parameter = true;
	    Name = Parm->getNameAsString();
	  }
	}
	return true;
      }

      bool TraverseUnaryDeref(UnaryOperator *) {
	// If we copy into a dereferenced pointer, we likely have a
	// false positive because the pointee might have been
	// allocated by us.
	return false;
      }

      bool TraverseMemberExpr(MemberExpr *Expr) {
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
  // Helpers

  void Report(SourceLocation Location, const char *Tool,
	      const std::string &Message)
  {
    if (!Location.isValid()) {
      FatalError(Context.getDiagnostics(),
		 "attempt to report at an invalid source location");
      return;
    }

    if (!FileDB->DB->Ptr) {
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
      FileIdentificationDatabase::ID FID = FileDB->Resolve(FileName);
      if (!(FID && Report(FID, Line, Column, Tool, Message)))
	FatalError(Context.getDiagnostics(), Location,
		   "could not report: " + FileDB->DB->ErrorMessage);
	return;
      }
  }

  bool Report(FileIdentificationDatabase::ID FID,
	      unsigned Line, unsigned Column, const char *Tool,
	      const std::string &Message)
  {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2
      (FileDB->DB->Ptr,
       "INSERT INTO reports (file, line, column, tool, message) "
       "VALUES (?, ?, ?, ?, ?);", -1, &stmt, nullptr) != SQLITE_OK) {
      FileDB->DB->SetError();
      return false;
    }
    sqlite3_bind_int64(stmt, 1, FID);
    sqlite3_bind_int64(stmt, 2, Line);
    sqlite3_bind_int64(stmt, 3, Column);
    sqlite3_bind_text(stmt, 4, Tool, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, Message.data(), Message.size(),
		      SQLITE_TRANSIENT);
    bool result = true;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      FileDB->DB->SetError();
      result = false;
    }
    sqlite3_finalize(stmt);
    return result;
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

    FileDB = std::make_shared<FileIdentificationDatabase>
      (std::make_shared<Database>());
    if (!FileDB->DB->Open()) {
      FatalError(CI.getDiagnostics(), FileDB->DB->ErrorMessage);
      return false;
    }
    return true;
  }
  void PrintHelp(llvm::raw_ostream& ros) {
    ros << "Analyse Condor source code\n";
  }

};

static FrontendPluginRegistry::Add<Action>
X("condor-analysis", "Condor source code analysis");

}
