#include "db-file.hpp"
#include "file.hpp"
#include "util.hpp"

#include <map>
#include <vector>

#include <limits.h>
#include <sys/stat.h>

//////////////////////////////////////////////////////////////////////
// FileIdentification

FileIdentification::FileIdentification(const char *path)
  : Mtime(0), Size(0)
{
  if (ResolvePath(path, Path)) {
    struct stat st;
    int ret = lstat(Path.c_str(), &st);
    if (ret == 0) {
      Mtime = st.st_mtime;
      Size = st.st_size;
    } else {
      Path.clear();
    }
  }
}

//////////////////////////////////////////////////////////////////////
// FileIdentificationDatabase

struct FileIdentificationDatabase::Impl {
  std::shared_ptr<Database> DB;

  typedef sqlite_int64 FileID;

  struct FileTableEntry {
    FileIdentification Ident;
    FileID ID;

    FileTableEntry(const std::string &Path)
      : Ident(Path.c_str()), ID(0)
    {
    }
  };

  std::map<std::string, std::shared_ptr<FileTableEntry>> FTable;
  std::vector<std::string> TouchedFiles;

  struct Report {
    std::shared_ptr<FileTableEntry> FI;
    unsigned Line;
    unsigned Column;
    std::string Tool;
    std::string Message;

    Report(std::shared_ptr<FileTableEntry> fi,
	   unsigned line,
	   unsigned column,
	   std::string tool,
	   std::string message)
      : FI(std::move(fi)),
	Line(line),
	Column(column),
	Tool(std::move(tool)),
	Message(std::move(message))
    {
    }
  };

  std::vector<Report> Reports;

  Impl(std::shared_ptr<Database> db)
    : DB(std::move(db))
  {
  }

  std::shared_ptr<FileTableEntry> Resolve(const std::string &Path)
  {
    auto p = FTable.find(Path);
    if (p == FTable.end()) {
      auto FTE = std::make_shared<FileTableEntry>(Path);
      if (FTE->Ident.Valid()) {
	p = FTable.find(FTE->Ident.Path);
	if (p == FTable.end()) {
	  FTable[Path] = FTE;
	  FTable[FTE->Ident.Path] = FTE;
	  return FTE;
	} 
	FTable[Path] = p->second;
      } else {
	// Could not resolve file name.
	DB->ErrorMessage = "could not find file on disk: ";
	DB->ErrorMessage += Path;
	return nullptr;
      }
    }
    return p->second;
  }

  bool Record
    (const char *Path, unsigned Line, unsigned Column,
     const char *Tool, const std::string &Message)
  {
    auto FTE = Resolve(Path);
    if (FTE == nullptr) {
      return false;
    }
    Reports.emplace_back(Report(std::move(FTE), Line, Column,
				Tool, Message));
    return true;
  }

  bool Commit()
  {
    TransactionResult result =
      DB->Transact([&]() -> TransactionResult {
	  TransactionResult tret;
	  for (auto const &Path : TouchedFiles) {
	    tret = MarkAsProcessed(Path);
	    if (tret != TransactionResult::COMMIT) {
	      return tret;
	    }
	  }

	  Statement stmt;
	  tret = stmt.TxnPrepare
	    (*DB, "INSERT INTO files (path, mtime, size) VALUES (?, ?, ?)");
	  if (tret != TransactionResult::COMMIT) {
	    return tret;
	  }
	  for (auto const &E : FTable) {
	    const FileIdentification &FI(E.second->Ident);
	    if (E.first != FI.Path) {
	      // This is a duplicate, secondary entry.  Avoid
	      // shadowing the real entry.
	      continue;
	    }
	    sqlite3_bind_text(stmt.Ptr, 1, FI.Path.data(), FI.Path.size(),
			      SQLITE_TRANSIENT);
	    sqlite3_bind_int64(stmt.Ptr, 2, FI.Mtime);
	    sqlite3_bind_int64(stmt.Ptr, 3, FI.Size);
	    if (sqlite3_step(stmt.Ptr) != SQLITE_DONE) {
	      return DB->SetTransactionError(sqlite3_sql(stmt.Ptr));
	    }
	    E.second->ID = sqlite3_last_insert_rowid(DB->Ptr);
	  }

	  tret = stmt.TxnPrepare
	    (*DB,
	     "INSERT INTO reports (file, line, column, tool, message) "
	     "VALUES (?, ?, ?, ?, ?);");
	  if (tret != TransactionResult::COMMIT) {
	    return tret;
	  }

	  for (auto const &R : Reports) {
	    sqlite3_reset(stmt.Ptr);
	    sqlite3_bind_int64(stmt.Ptr, 1, R.FI->ID);
	    sqlite3_bind_int64(stmt.Ptr, 2, R.Line);
	    sqlite3_bind_int64(stmt.Ptr, 3, R.Column);
	    sqlite3_bind_text(stmt.Ptr, 4, R.Tool.data(), R.Tool.size(),
			      SQLITE_TRANSIENT);
	    sqlite3_bind_text(stmt.Ptr, 5, R.Message.data(), R.Message.size(),
			      SQLITE_TRANSIENT);
	    if (sqlite3_step(stmt.Ptr) != SQLITE_DONE) {
	      return DB->SetTransactionError(sqlite3_sql(stmt.Ptr));
	    }
	  }
	  return TransactionResult::COMMIT;
	});
    return result == TransactionResult::COMMIT;
  }

  TransactionResult MarkAsProcessed(const std::string &Path)
  {
    std::string Absolute;
    if (!ResolvePath(Path.c_str(), Absolute)) {
      DB->ErrorMessage = "could not find file on disk: ";
      DB->ErrorMessage += Path;
      return TransactionResult::ERROR;
    }

    Statement SQL;
    TransactionResult tret =
      SQL.TxnPrepare(*DB, "SELECT id FROM files "
		     "WHERE path = ? ORDER BY id DESC LIMIT 1");
    if (tret != TransactionResult::COMMIT) {
      return tret;
    }
    sqlite3_bind_text(SQL.Ptr, 1, Absolute.data(), Absolute.size(),
		      SQLITE_TRANSIENT);
    int ret = sqlite3_step(SQL.Ptr);
    if (ret == SQLITE_DONE) {
      // File is not in the database.  No need to mask older errors.
      return TransactionResult::COMMIT;
    }
    if (ret != SQLITE_ROW) {
      return DB->SetTransactionError(sqlite3_sql(SQL.Ptr));
    }
    // Add an entry for the file, hiding the previous reports.
    // TODO: Only hide changed files? What about plugin changes?
    if (Resolve(Path) == nullptr) {
      return TransactionResult::ERROR;
    }
    return TransactionResult::COMMIT;
  }
};

FileIdentificationDatabase::FileIdentificationDatabase(std::shared_ptr<Database> db)
  : impl(new Impl(std::move(db)))
{
}

FileIdentificationDatabase::~FileIdentificationDatabase()
{
}

bool FileIdentificationDatabase::isOpen() const
{
  return impl->DB->Ptr != nullptr;
}

std::string FileIdentificationDatabase::ErrorMessage() const
{
  return impl->DB->ErrorMessage;
}

bool
FileIdentificationDatabase::Report
  (const char *Path, unsigned Line, unsigned Column,
   const char *Tool, std::string Message)
{
  return impl->Record(Path, Line, Column, Tool, std::move(Message));
}

void
FileIdentificationDatabase::MarkForProcessing(const char *Path)
{
  impl->TouchedFiles.emplace_back(std::move(Path));
}

bool
FileIdentificationDatabase::Commit()
{
  return impl->Commit();
}

