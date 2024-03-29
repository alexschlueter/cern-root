//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// version: $Id$
// author:  Lukasz Janyst <ljanyst@cern.ch>
//------------------------------------------------------------------------------

#include "cling/Interpreter/Interpreter.h"

#include "DynamicLookup.h"
#include "ExecutionContext.h"
#include "IncrementalParser.h"

#include "cling/Interpreter/CIFactory.h"
#include "cling/Interpreter/CompilationOptions.h"
#include "cling/Interpreter/InterpreterCallbacks.h"
#include "cling/Interpreter/LookupHelper.h"
#include "cling/Interpreter/StoredValueRef.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/Utils/AST.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Mangle.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/SourceManager.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/ParseDiagnostic.h" // FIXME: remove this back-dependency!
// when clang is ready.
#include "clang/Parse/Parser.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaInternal.h"

#include "llvm/Linker.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <iostream>
#include <fstream>
#include <set>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#ifdef WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

using namespace clang;

namespace {

  static cling::Interpreter::ExecutionResult
  ConvertExecutionResult(cling::ExecutionContext::ExecutionResult ExeRes) {
    switch (ExeRes) {
    case cling::ExecutionContext::kExeSuccess:
      return cling::Interpreter::kExeSuccess;
    case cling::ExecutionContext::kExeFunctionNotCompiled:
      return cling::Interpreter::kExeFunctionNotCompiled;
    case cling::ExecutionContext::kExeUnresolvedSymbols:
      return cling::Interpreter::kExeUnresolvedSymbols;
    default: break;
    }
    return cling::Interpreter::kExeSuccess;
  }
} // unnamed namespace


// "Declared" to the JIT in RuntimeUniverse.h
extern "C" {
  int cling__runtime__internal__local_cxa_atexit(void (*func) (void*), void* arg,
                                                 void* dso,
                                                 void* interp) {
    return ((cling::Interpreter*)interp)->CXAAtExit(func, arg, dso);
  }
}



namespace cling {
#if (!_WIN32)
  // "Declared" to the JIT in RuntimeUniverse.h
  namespace runtime {
    namespace internal {
      struct __trigger__cxa_atexit {
        ~__trigger__cxa_atexit();
      } /*S*/;
      __trigger__cxa_atexit::~__trigger__cxa_atexit() {
        if (std::getenv("bar") == (char*)-1) {
          llvm::errs() <<
            "UNEXPECTED cling::runtime::internal::__trigger__cxa_atexit\n";
        }
      }
    }
  }
#endif

  Interpreter::PushTransactionRAII::PushTransactionRAII(const Interpreter* i)
    : m_Interpreter(i) {
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = 0;
    CO.Debug = 0;
    CO.CodeGeneration = 1;
    CO.CodeGenerationForModule = 0;

    m_Transaction = m_Interpreter->m_IncrParser->beginTransaction(CO);
  }

  Interpreter::PushTransactionRAII::~PushTransactionRAII() {
    pop();
  }

  void Interpreter::PushTransactionRAII::pop() const {
    if (Transaction* T 
        = m_Interpreter->m_IncrParser->endTransaction(m_Transaction)) {
      assert(T == m_Transaction && "Ended different transaction?");
      m_Interpreter->m_IncrParser->commitTransaction(T);
    }
  }  

  // This function isn't referenced outside its translation unit, but it
  // can't use the "static" keyword because its address is used for
  // GetMainExecutable (since some platforms don't support taking the
  // address of main, and some platforms can't implement GetMainExecutable
  // without being given the address of a function in the main executable).
  llvm::sys::Path GetExecutablePath(const char *Argv0) {
    // This just needs to be some symbol in the binary; C++ doesn't
    // allow taking the address of ::main however.
    void *MainAddr = (void*) (intptr_t) GetExecutablePath;
    return llvm::sys::Path::GetMainExecutable(Argv0, MainAddr);
  }

  const Parser& Interpreter::getParser() const {
    return *m_IncrParser->getParser();
  }

  CodeGenerator* Interpreter::getCodeGenerator() const {
    return m_IncrParser->getCodeGenerator();
  }

  void Interpreter::unload() {
    m_IncrParser->unloadTransaction(0);
  }

  Interpreter::Interpreter(int argc, const char* const *argv,
                           const char* llvmdir /*= 0*/) :
    m_UniqueCounter(0), m_PrintAST(false), m_PrintIR(false), 
    m_DynamicLookupEnabled(false), m_RawInputEnabled(false) {

    m_AtExitFuncs.reserve(200);
    m_LoadedFiles.reserve(20);

    m_LLVMContext.reset(new llvm::LLVMContext);
    std::vector<unsigned> LeftoverArgsIdx;
    m_Opts = InvocationOptions::CreateFromArgs(argc, argv, LeftoverArgsIdx);
    std::vector<const char*> LeftoverArgs;

    for (size_t I = 0, N = LeftoverArgsIdx.size(); I < N; ++I) {
      LeftoverArgs.push_back(argv[LeftoverArgsIdx[I]]);
    }

    m_IncrParser.reset(new IncrementalParser(this, LeftoverArgs.size(),
                                             &LeftoverArgs[0],
                                             llvmdir));
    Sema& SemaRef = getSema();
    m_LookupHelper.reset(new LookupHelper(new Parser(SemaRef.getPreprocessor(), 
                                                     SemaRef, 
                                                     /*SkipFunctionBodies*/false,
                                                     /*isTemp*/true), this));

    if (m_IncrParser->hasCodeGenerator()) {
      llvm::Module* theModule = m_IncrParser->getCodeGenerator()->GetModule();
      m_ExecutionContext.reset(new ExecutionContext(theModule));
    }

    m_IncrParser->Initialize();

    // Add path to interpreter's include files
    // Try to find the headers in the src folder first
#ifdef CLING_SRCDIR_INCL
    llvm::sys::Path SrcP(CLING_SRCDIR_INCL);
    if (SrcP.canRead())
      AddIncludePath(SrcP.str());
#endif

    llvm::sys::Path P = GetExecutablePath(argv[0]);
    if (!P.isEmpty()) {
      P.eraseComponent();  // Remove /cling from foo/bin/clang
      P.eraseComponent();  // Remove /bin   from foo/bin
      // Get foo/include
      P.appendComponent("include");
      if (P.canRead())
        AddIncludePath(P.str());
      else {
#ifdef CLING_INSTDIR_INCL
        llvm::sys::Path InstP(CLING_INSTDIR_INCL);
        if (InstP.canRead())
          AddIncludePath(InstP.str());
#endif
      }
    }

    m_ExecutionContext->addSymbol("cling__runtime__internal__local_cxa_atexit",
                  (void*)(intptr_t)&cling__runtime__internal__local_cxa_atexit);

    // Enable incremental processing, which prevents the preprocessor destroying
    // the lexer on EOF token.
    getSema().getPreprocessor().enableIncrementalProcessing();

    handleFrontendOptions();

    // Tell the diagnostic client that we are entering file parsing mode.
    DiagnosticConsumer& DClient = getCI()->getDiagnosticClient();
    DClient.BeginSourceFile(getCI()->getLangOpts(),
                            &getCI()->getPreprocessor());

    if (getCI()->getLangOpts().CPlusPlus) {
      // Set up common declarations which are going to be available
      // only at runtime
      // Make sure that the universe won't be included to compile time by using
      // -D __CLING__ as CompilerInstance's arguments
#ifdef _WIN32
	  // We have to use the #defined __CLING__ on windows first. 
      //FIXME: Find proper fix.
      declare("#ifdef __CLING__ \n#endif");  
#endif
      declare("#include \"cling/Interpreter/RuntimeUniverse.h\"");
      declare("#include \"cling/Interpreter/ValuePrinter.h\"");

      if (getCodeGenerator()) {
        // Set up the gCling variable if it can be used
        std::stringstream initializer;
        initializer << "namespace cling {namespace runtime { "
          "cling::Interpreter *gCling=(cling::Interpreter*)"
                    << (uintptr_t)this << ";} }";
        declare(initializer.str());
      }
    }
    else {
      declare("#include \"cling/Interpreter/CValuePrinter.h\"");
    }

  }

  Interpreter::~Interpreter() {
    getCI()->getDiagnostics().getClient()->EndSourceFile();

    for (size_t I = 0, N = m_AtExitFuncs.size(); I < N; ++I) {
      const CXAAtExitElement& AEE = m_AtExitFuncs[N - I - 1];
      (*AEE.m_Func)(AEE.m_Arg);
    }
  }

  const char* Interpreter::getVersion() const {
    return "$Id$";
  }

  void Interpreter::handleFrontendOptions() {
    if (m_Opts.ShowVersion) {
      llvm::errs() << getVersion() << '\n';
    }
    if (m_Opts.Help) {
      m_Opts.PrintHelp();
    }
  }

  void Interpreter::AddIncludePath(llvm::StringRef incpath)
  {
    // Add the given path to the list of directories in which the interpreter
    // looks for include files. Only one path item can be specified at a
    // time, i.e. "path1:path2" is not supported.

    CompilerInstance* CI = getCI();
    HeaderSearchOptions& headerOpts = CI->getHeaderSearchOpts();
    const bool IsFramework = false;
    const bool IsSysRootRelative = true;
    headerOpts.AddPath(incpath, frontend::Angled, IsFramework,
                       IsSysRootRelative);

    Preprocessor& PP = CI->getPreprocessor();
    ApplyHeaderSearchOptions(PP.getHeaderSearchInfo(), headerOpts,
                                    PP.getLangOpts(),
                                    PP.getTargetInfo().getTriple());
  }

  void Interpreter::DumpIncludePath() {
    llvm::SmallVector<std::string, 100> IncPaths;
    GetIncludePaths(IncPaths, true /*withSystem*/, true /*withFlags*/);
    // print'em all
    for (unsigned i = 0; i < IncPaths.size(); ++i) {
      llvm::errs() << IncPaths[i] <<"\n";
    }
  }

  void Interpreter::storeInterpreterState(const std::string& name) const {
    llvm::sys::Path LookupFile = llvm::sys::Path::GetCurrentDirectory();
    std::string ErrMsg;
    if (LookupFile.isEmpty()) {
      llvm::errs() << "Error: " << ErrMsg << "\n";
      return;
    }
    LookupFile.appendComponent(name + ".includedFiles.tmp");
    std::ofstream ofs (LookupFile.c_str(), std::ofstream::out);  
    llvm::raw_os_ostream Out(ofs);
    printIncludedFiles(Out);
    dumpAST(name);
    dumpLookupTable(name);
  }

  void Interpreter::compareInterpreterState(const std::string& name) const {
    // Store new state
    std::string compareTo = name + "cmp";
    storeInterpreterState(compareTo);
    // Compare with the previous one
    compareAST(name);
    compareIncludedFiles(name);
    compareLookup(name);
  }

  void Interpreter::dumpAST(const std::string& name) const {
    ASTContext& C = getSema().getASTContext();
    TranslationUnitDecl* TU = C.getTranslationUnitDecl();
    unsigned Indentation = 0;
    bool PrintInstantiation = false;
    std::string ErrMsg;
    llvm::sys::Path Filename = llvm::sys::Path::GetCurrentDirectory();
    if (Filename.isEmpty()) {
      llvm::errs() << "Error: " << ErrMsg << "\n";
      return;
    }
    //Test that the filename isn't already used
    std::string testFilename = name + "AST.diff";
    Filename.appendComponent(testFilename);
    if (llvm::sys::fs::exists(Filename.str())) {
      llvm::errs() << Filename.str() << "\n";
      llvm::errs() << "Filename already exists. Please choose a new one \n";
      exit (1);
    }
    else {
    std::string rename = name + "AST.tmp";
    Filename.eraseComponent();
    Filename.appendComponent(rename);
    std::ofstream ofs (Filename.c_str(), std::ofstream::out);  
    llvm::raw_os_ostream Out(ofs);
    clang::PrintingPolicy policy = C.getPrintingPolicy();
    // Iteration over the decls might cause deserialization.
    cling::Interpreter::PushTransactionRAII deserRAII(this);
    TU->print(Out, policy, Indentation, PrintInstantiation);
    Out.flush();
    }
  }

  class DumpDeclContexts : public RecursiveASTVisitor<DumpDeclContexts> {
  private:
    //llvm::raw_ostream& m_OS;
  public:
    //DumpDeclContexts(llvm::raw_ostream& OS) : m_OS(OS) { }
    DumpDeclContexts(llvm::raw_ostream&) { }
    bool VisitDeclContext(DeclContext* DC) {
      //DC->dumpLookups(m_OS);
      return true;
    }
  };

  void Interpreter::dumpLookupTable(const std::string& name) const {
    std::string ErrMsg;
    llvm::sys::Path LookupFile = llvm::sys::Path::GetCurrentDirectory();
    if (LookupFile.isEmpty()) {
      llvm::errs() << "Error: " << ErrMsg << "\n";
      return;
    }
    //Test that the filename isn't already used
    std::string testFilename = name + "LookupFile.diff";
    LookupFile.appendComponent(testFilename);
    if (llvm::sys::fs::exists(LookupFile.str())) {
      llvm::errs() << LookupFile.str() << "\n";
      llvm::errs() << "Filename already exists. Please choose a new one \n";
      exit (1);
    }
    else {
    std::string fileName = name + "Lookup.tmp";
    LookupFile.eraseComponent();
    LookupFile.appendComponent(fileName);
    std::ofstream ofs (LookupFile.c_str(), std::ofstream::out);  
    llvm::raw_os_ostream Out(ofs);
    ASTContext& C = getSema().getASTContext();
    DumpDeclContexts dumper(Out);
    dumper.TraverseDecl(C.getTranslationUnitDecl());
    }
  }

  void Interpreter::printIncludedFiles(llvm::raw_ostream& Out) const {
    std::vector<std::string> loadedFiles;
    typedef std::vector<Interpreter::LoadedFileInfo*> LoadedFiles_t;
    const LoadedFiles_t& LoadedFiles = getLoadedFiles();
    for (LoadedFiles_t::const_iterator I = LoadedFiles.begin(),
           E = LoadedFiles.end(); I != E; ++I) {
      char cType[] = { 'S', 'D', 'B' };
      Out << '[' << cType[(*I)->getType()] << "] "<< (*I)->getName() << '\n';
      loadedFiles.push_back((*I)->getName());
    }
    clang::SourceManager &SM = getCI()->getSourceManager();
    for (clang::SourceManager::fileinfo_iterator I = SM.fileinfo_begin(),
           E = SM.fileinfo_end(); I != E; ++I) {
      const clang::SrcMgr::ContentCache &C = *I->second;
      const clang::FileEntry *FE = C.OrigEntry;
      std::string fileName(FE->getName());
      // avoid duplicates (i.e. already printed in the previous for loop)
      if (std::find(loadedFiles.begin(),
                    loadedFiles.end(), fileName) == loadedFiles.end()) {
        // filter out any /usr/...../bits/* file names
        if (!(fileName.compare(0, 5, "/usr/") == 0 &&
            fileName.find("/bits/") != std::string::npos)) {
          Out << fileName << '\n';
        }
      }
    }
  }

  void Interpreter::compareAST(const std::string& name) const {
   // Diff between the two existing file
    llvm::sys::Path tmpDir1 = llvm::sys::Path::GetCurrentDirectory();
     std::string ErrMsg;
    if (tmpDir1.isEmpty()) {
      llvm::errs() << "Error: " << ErrMsg << "\n";
      return;
    }
    llvm::sys::Path stateFile1 = tmpDir1;
    std::string state1 = name + "AST.tmp";
    stateFile1.appendComponent(state1);
    llvm::sys::Path tmpDir2 = llvm::sys::Path::GetCurrentDirectory();
    if (tmpDir2.isEmpty()) {
      llvm::errs() << "Error: " << ErrMsg << "\n";
      return;
    }
    llvm::sys::Path stateFile2 = tmpDir2;
    std::string state2 = name + "cmpAST.tmp";
    stateFile2.appendComponent(state2);
    std::string command = "diff -u " + stateFile1.str() + " " 
      + stateFile2.str();
    // printing the results
#ifndef LLVM_ON_WIN32
      FILE* pipe = popen(command.c_str(), "r");
      if (!pipe) {
	perror( "Error" );
      }
      char buffer[128];
      std::string result = "";
      while(!feof(pipe)) {
	if(fgets(buffer, 128, pipe) != NULL) 
    		result += buffer;
	  }
      pclose(pipe);     
      if(!result.empty()){
	std::string ErrMsg;
	llvm::sys::Path DiffFile = llvm::sys::Path::GetCurrentDirectory();
	if (DiffFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
	}
	// Test if nameAST.diff already exists
        std::string file;
	llvm::sys::Path testFile = llvm::sys::Path::GetCurrentDirectory();
	if (testFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
        }
	std::string testName = name + "AST.diff";
	testFile.appendComponent(testName);
	if (llvm::sys::fs::exists(testFile.str())){
	  file = name + "1AST.diff";
	}
	else {
	  file = name + "AST.diff";
	}
	DiffFile.appendComponent(file);
	std::ofstream ofs (DiffFile.c_str(), std::ofstream::out);  
	llvm::raw_os_ostream Out(ofs);
	Out << result;
	Out.flush();
	llvm::errs() << "File with AST differencies stored in: ";
	llvm::errs() << file << "\n";
	llvm::errs() << DiffFile.c_str();
	llvm::errs() << "\n";
      }
#else
      FILE* pipe = _popen(command.c_str(), "r");
      if (!pipe) {
	perror( "Error" );
      }
      char buffer[128];
      std::string result = "";
      while(!feof(pipe)) {
	  if(fgets(buffer, 128, pipe) != NULL)
    		result += buffer;
	  }
      _pclose(pipe);  
      if(!result.empty()){
	std::string ErrMsg;
	llvm::sys::Path DiffFile = llvm::sys::Path::GetCurrentDirectory();
	if (DiffFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
	}
	// Test if nameAST.diff already exists
        std::string file;
	llvm::sys::Path testFile = llvm::sys::Path::GetCurrentDirectory();
	if (testFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
        }
	std::string testName = name + "AST.diff";
	testFile.appendComponent(testName);
	if (llvm::sys::fs::exists(testFile.str())){
	  file = name + "1AST.diff";
	}
	else {
	  file = name + "AST.diff";
	}
	DiffFile.appendComponent(file);
	std::ofstream ofs (DiffFile.c_str(), std::ofstream::out);  
	llvm::raw_os_ostream Out(ofs);
	Out.flush();
	llvm::errs() << "File with AST differencies stored in: ";
	llvm::errs() << file << "\n";
	llvm::errs() << DiffFile.c_str();
	llvm::errs() << "\n";
      }
#endif   
      std::string command2 = stateFile1.str();
      std::string command3 = stateFile2.str();
      int result1 = std::remove(command2.c_str());
      int result2 = std::remove(command3.c_str());
      if((result1 != 0) && (result2 != 0))
	perror( "Error deleting files were AST were stored" );
  }


  void Interpreter::compareIncludedFiles(const std::string& name) const {
    // Diff between the two existing file
    llvm::sys::Path tmpDir1 = llvm::sys::Path::GetCurrentDirectory();
     std::string ErrMsg;
    if (tmpDir1.isEmpty()) {
      llvm::errs() << "Error: " << ErrMsg << "\n";
      return;
    }
    llvm::sys::Path stateFile1 = tmpDir1;
    std::string state1 = name + ".includedFiles.tmp";
    stateFile1.appendComponent(state1);
    llvm::sys::Path tmpDir2 = llvm::sys::Path::GetCurrentDirectory();
    if (tmpDir2.isEmpty()) {
      llvm::errs() << "Error: " << ErrMsg << "\n";
      return;
    }
    llvm::sys::Path stateFile2 = tmpDir2;
    std::string state2 = name + "cmp.includedFiles.tmp";
    stateFile2.appendComponent(state2);
    std::string command = "diff -u " + stateFile1.str() + " " 
      + stateFile2.str();
// printing the results
#ifndef LLVM_ON_WIN32
      FILE* pipe = popen(command.c_str(), "r");
      if (!pipe) {
	perror( "Error" );
      }
      char buffer[128];
      std::string result = "";
      while(!feof(pipe)) {
	if(fgets(buffer, 128, pipe) != NULL) 
    		result += buffer;
	  }
      pclose(pipe);     
      if(!result.empty()){
	std::string ErrMsg;
	llvm::sys::Path DiffFile = llvm::sys::Path::GetCurrentDirectory();
	if (DiffFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
	}
	// Test if nameIncludedFiles.diff already exists
        std::string file;
	llvm::sys::Path testFile = llvm::sys::Path::GetCurrentDirectory();
	if (testFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
        }
	std::string testName = name + "IncludedFiles.diff";
	testFile.appendComponent(testName);
	if (llvm::sys::fs::exists(testFile.str())){
	  file = name + "1IncludedFiles.diff";
	}
	else {
	  file = name + "IncludedFiles.diff";
	}
	DiffFile.appendComponent(file);
	std::ofstream ofs (DiffFile.c_str(), std::ofstream::out);  
	llvm::raw_os_ostream Out(ofs);
	Out << result;
	Out.flush();
	llvm::errs() << "File with included files differencies stored in: ";
	llvm::errs() << file << "\n";
	llvm::errs() << DiffFile.c_str();
	llvm::errs() << "\n";
      }
#else
      FILE* pipe = _popen(command.c_str(), "r");
      if (!pipe) {
	perror( "Error" );
      }
      char buffer[128];
      std::string result = "";
      while(!feof(pipe)) {
	  if(fgets(buffer, 128, pipe) != NULL)
    		result += buffer;
	  }
      _pclose(pipe);  
      if(!result.empty()){
	std::string ErrMsg;
	llvm::sys::Path DiffFile = llvm::sys::Path::GetCurrentDirectory();
	if (DiffFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
	}
	// Test if nameIncludedFiles.diff already exists
        std::string file;
	llvm::sys::Path testFile = llvm::sys::Path::GetCurrentDirectory();
	if (testFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
        }
	std::string testName = name + "IncludedFiles.diff";
	testFile.appendComponent(testName);
	if (llvm::sys::fs::exists(testFile.str())){
	  file = name + "1IncludedFiles.diff";
	}
	else {
	  file = name + "IncludedFiles.diff";
	}
	DiffFile.appendComponent(file);
	std::ofstream ofs (DiffFile.c_str(), std::ofstream::out);  
	llvm::raw_os_ostream Out(ofs);
	Out.flush();
	llvm::errs() << "File with included files differencies stored in: ";
	llvm::errs() << file << "\n";
	llvm::errs() << DiffFile.c_str();
	llvm::errs() << "\n";
      }
#endif   
      std::string command2 = stateFile1.str();
      std::string command3 = stateFile2.str();
      int result1 = std::remove(command2.c_str());
      int result2 = std::remove(command3.c_str());
      if((result1 != 0) && (result2 != 0))
	perror( "Error deleting files were included files were stored" );
  }

 void Interpreter::compareLookup(const std::string& name) const {
    // Diff between the two existing file
    llvm::sys::Path tmpDir1 = llvm::sys::Path::GetCurrentDirectory();
     std::string ErrMsg;
    if (tmpDir1.isEmpty()) {
      llvm::errs() << "Error: " << ErrMsg << "\n";
      return;
    }
    llvm::sys::Path stateFile1 = tmpDir1;
    std::string state1 = name + "Lookup.tmp";
    stateFile1.appendComponent(state1);
    llvm::sys::Path tmpDir2 = llvm::sys::Path::GetCurrentDirectory();
    if (tmpDir2.isEmpty()) {
      llvm::errs() << "Error: " << ErrMsg << "\n";
      return;
    }
    llvm::sys::Path stateFile2 = tmpDir2;
    std::string state2 = name + "cmpLookup.tmp";
    stateFile2.appendComponent(state2);
    std::string command = "diff -u " + stateFile1.str() + " " 
      + stateFile2.str();
// printing the results
#ifndef LLVM_ON_WIN32
      FILE* pipe = popen(command.c_str(), "r");
      if (!pipe) {
	perror( "Error" );
      }
      char buffer[128];
      std::string result = "";
      while(!feof(pipe)) {
	if(fgets(buffer, 128, pipe) != NULL) 
    		result += buffer;
	  }
      pclose(pipe);     
      if(!result.empty()){
	std::string ErrMsg;
	llvm::sys::Path DiffFile = llvm::sys::Path::GetCurrentDirectory();
	if (DiffFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
	}
	// Test if lookup.diff already exists
        std::string file;
	llvm::sys::Path testFile = llvm::sys::Path::GetCurrentDirectory();
	if (testFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
        }
	std::string testName = name + "Lookup.diff";
	testFile.appendComponent(testName);
	if (llvm::sys::fs::exists(testFile.str())){
	  file = name + "1Lookup.diff";
	}
	else {
	  file = name + "Lookup.diff";
	}
	DiffFile.appendComponent(file);
	std::ofstream ofs (DiffFile.c_str(), std::ofstream::out);  
	llvm::raw_os_ostream Out(ofs);
	Out << result;
	Out.flush();
	llvm::errs() << "File with lookup tables differencies stored in: ";
	llvm::errs() << file << "\n";
	llvm::errs() << DiffFile.c_str();
	llvm::errs() << "\n";
      }
#else
      FILE* pipe = _popen(command.c_str(), "r");
      if (!pipe) {
	perror( "Error" );
      }
      char buffer[128];
      std::string result = "";
      while(!feof(pipe)) {
	  if(fgets(buffer, 128, pipe) != NULL)
    		result += buffer;
	  }
      _pclose(pipe);  
      if(!result.empty()){
	std::string ErrMsg;
	llvm::sys::Path DiffFile = llvm::sys::Path::GetCurrentDirectory();
	if (DiffFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
	}
	// Test if Lookup.diff already exists
        std::string file;
	llvm::sys::Path testFile = llvm::sys::Path::GetCurrentDirectory();
	if (testFile.isEmpty()) {
	  llvm::errs() << "Error: " << ErrMsg << "\n";
	  return;
        }
	std::string testName = name + "Lookup.diff";
	testFile.appendComponent(testName);
	if (llvm::sys::fs::exists(testFile.str())){
	  file = name + "1Lookup.diff";
	}
	else {
	  file = name + "Lookup.diff";
	}
	DiffFile.appendComponent(file);
	std::ofstream ofs (DiffFile.c_str(), std::ofstream::out);  
	llvm::raw_os_ostream Out(ofs);
	Out.flush();
	llvm::errs() << "File with lookup tables differencies stored in: ";
	llvm::errs() << file << "\n";
	llvm::errs() << DiffFile.c_str();
	llvm::errs() << "\n";
      }
#endif   
      std::string command2 = stateFile1.str();
      std::string command3 = stateFile2.str();
      int result1 = std::remove(command2.c_str());
      int result2 = std::remove(command3.c_str());
      if((result1 != 0) && (result2 != 0))
	perror( "Error deleting files were lookup tables were stored" );
  }

  // Adapted from clang/lib/Frontend/CompilerInvocation.cpp
  void Interpreter::GetIncludePaths(llvm::SmallVectorImpl<std::string>& incpaths,
                                   bool withSystem, bool withFlags) {
    const HeaderSearchOptions Opts(getCI()->getHeaderSearchOpts());

    if (withFlags && Opts.Sysroot != "/") {
      incpaths.push_back("-isysroot");
      incpaths.push_back(Opts.Sysroot);
    }

    /// User specified include entries.
    for (unsigned i = 0, e = Opts.UserEntries.size(); i != e; ++i) {
      const HeaderSearchOptions::Entry &E = Opts.UserEntries[i];
      if (E.IsFramework && E.Group != frontend::Angled)
        llvm::report_fatal_error("Invalid option set!");
      switch (E.Group) {
      case frontend::After:
        if (withFlags) incpaths.push_back("-idirafter");
        break;

      case frontend::Quoted:
        if (withFlags) incpaths.push_back("-iquote");
        break;

      case frontend::System:
        if (!withSystem) continue;
        if (withFlags) incpaths.push_back("-isystem");
        break;

      case frontend::IndexHeaderMap:
        if (!withSystem) continue;
        if (withFlags) incpaths.push_back("-index-header-map");
        if (withFlags) incpaths.push_back(E.IsFramework? "-F" : "-I");
        break;

      case frontend::CSystem:
        if (!withSystem) continue;
        if (withFlags) incpaths.push_back("-c-isystem");
        break;

      case frontend::ExternCSystem:
        if (!withSystem) continue;
        if (withFlags) incpaths.push_back("-extern-c-isystem");
        break;

      case frontend::CXXSystem:
        if (!withSystem) continue;
        if (withFlags) incpaths.push_back("-cxx-isystem");
        break;

      case frontend::ObjCSystem:
        if (!withSystem) continue;
        if (withFlags) incpaths.push_back("-objc-isystem");
        break;

      case frontend::ObjCXXSystem:
        if (!withSystem) continue;
        if (withFlags) incpaths.push_back("-objcxx-isystem");
        break;

      case frontend::Angled:
        if (withFlags) incpaths.push_back(E.IsFramework ? "-F" : "-I");
        break;
      }
      incpaths.push_back(E.Path);
    }

    if (withSystem && !Opts.ResourceDir.empty()) {
      if (withFlags) incpaths.push_back("-resource-dir");
      incpaths.push_back(Opts.ResourceDir);
    }
    if (withSystem && withFlags && !Opts.ModuleCachePath.empty()) {
      incpaths.push_back("-fmodule-cache-path");
      incpaths.push_back(Opts.ModuleCachePath);
    }
    if (withSystem && withFlags && !Opts.UseStandardSystemIncludes)
      incpaths.push_back("-nostdinc");
    if (withSystem && withFlags && !Opts.UseStandardCXXIncludes)
      incpaths.push_back("-nostdinc++");
    if (withSystem && withFlags && Opts.UseLibcxx)
      incpaths.push_back("-stdlib=libc++");
    if (withSystem && withFlags && Opts.Verbose)
      incpaths.push_back("-v");
  }

  CompilerInstance* Interpreter::getCI() const {
    return m_IncrParser->getCI();
  }

  const Sema& Interpreter::getSema() const {
    return getCI()->getSema();
  }

  Sema& Interpreter::getSema() {
    return getCI()->getSema();
  }

   llvm::ExecutionEngine* Interpreter::getExecutionEngine() const {
    return m_ExecutionContext->getExecutionEngine();
  }

  llvm::Module* Interpreter::getModule() const {
    return m_IncrParser->getCodeGenerator()->GetModule();
  }

  ///\brief Maybe transform the input line to implement cint command line
  /// semantics (declarations are global) and compile to produce a module.
  ///
  Interpreter::CompilationResult
  Interpreter::process(const std::string& input, StoredValueRef* V /* = 0 */,
                       Transaction** T /* = 0 */) {
    if (isRawInputEnabled() || !ShouldWrapInput(input))
      return declare(input, T);

    CompilationOptions CO;
    CO.DeclarationExtraction = 1;
    CO.ValuePrinting = CompilationOptions::VPAuto;
    CO.ResultEvaluation = (bool)V;
    CO.DynamicScoping = isDynamicLookupEnabled();
    CO.Debug = isPrintingAST();
    CO.IRDebug = isPrintingIR();

    if (EvaluateInternal(input, CO, V, T) == Interpreter::kFailure) {
      return Interpreter::kFailure;
    }

    return Interpreter::kSuccess;
  }

  Interpreter::CompilationResult 
  Interpreter::parse(const std::string& input, Transaction** T /*=0*/) const {
    CompilationOptions CO;
    CO.CodeGeneration = 0;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = isDynamicLookupEnabled();
    CO.Debug = isPrintingAST();
    CO.IRDebug = isPrintingIR();

    return DeclareInternal(input, CO, T);
  }

  Interpreter::CompilationResult
  Interpreter::loadModuleForHeader(const std::string& headerFile) {
    Preprocessor& PP = getCI()->getPreprocessor();
    //Copied from clang's PPDirectives.cpp
    bool isAngled = false;
    // Clang doc says:
    // "LookupFrom is set when this is a \#include_next directive, it specifies 
    // the file to start searching from."
    const DirectoryLookup* LookupFrom = 0;
    const DirectoryLookup* CurDir = 0;

    Module* module = 0;
    PP.LookupFile(headerFile, isAngled, LookupFrom, CurDir, /*SearchPath*/0,
                  /*RelativePath*/ 0, &module, /*SkipCache*/false);
    if (!module)
      return Interpreter::kFailure;

    SourceLocation fileNameLoc;
    ModuleIdPath path = std::make_pair(PP.getIdentifierInfo(module->Name),
                                       fileNameLoc);

    // Pretend that the module came from an inclusion directive, so that clang
    // will create an implicit import declaration to capture it in the AST.
    bool isInclude = true;
    SourceLocation includeLoc;
    if (getCI()->loadModule(includeLoc, path, Module::AllVisible, isInclude)) {
      // After module load we need to "force" Sema to generate the code for
      // things like dynamic classes.
      getSema().ActOnEndOfTranslationUnit();
      return Interpreter::kSuccess;
    }

    return Interpreter::kFailure;
  }

  Interpreter::CompilationResult
  Interpreter::parseForModule(const std::string& input) {
    CompilationOptions CO;
    CO.CodeGeneration = 1;
    CO.CodeGenerationForModule = 1;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = isDynamicLookupEnabled();
    CO.Debug = isPrintingAST();
    CO.IRDebug = isPrintingIR();
    
    // When doing parseForModule avoid warning about the user code
    // being loaded ... we probably might as well extend this to
    // ALL warnings ... but this will suffice for now (working
    // around a real bug in QT :().
    DiagnosticsEngine& Diag = getCI()->getDiagnostics();
    Diag.setDiagnosticMapping(clang::diag::warn_field_is_uninit,
                              clang::diag::MAP_IGNORE, SourceLocation());
    return DeclareInternal(input, CO);
  }
  
  Interpreter::CompilationResult
  Interpreter::declare(const std::string& input, Transaction** T/*=0 */) {
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = isDynamicLookupEnabled();
    CO.Debug = isPrintingAST();
    CO.IRDebug = isPrintingIR();

    return DeclareInternal(input, CO, T);
  }

  Interpreter::CompilationResult
  Interpreter::evaluate(const std::string& input, StoredValueRef& V) {
    // Here we might want to enforce further restrictions like: Only one
    // ExprStmt can be evaluated and etc. Such enforcement cannot happen in the
    // worker, because it is used from various places, where there is no such
    // rule
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 1;

    return EvaluateInternal(input, CO, &V);
  }

  Interpreter::CompilationResult
  Interpreter::echo(const std::string& input, StoredValueRef* V /* = 0 */) {
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = CompilationOptions::VPEnabled;
    CO.ResultEvaluation = 0;

    return EvaluateInternal(input, CO, V);
  }

  Interpreter::CompilationResult
  Interpreter::execute(const std::string& input) {
    CompilationOptions CO;
    CO.DeclarationExtraction = 0;
    CO.ValuePrinting = 0;
    CO.ResultEvaluation = 0;
    CO.DynamicScoping = 0;
    CO.Debug = isPrintingAST();
    CO.IRDebug = isPrintingIR();

    // Disable warnings which doesn't make sense when using the prompt
    // This gets reset with the clang::Diagnostics().Reset()
    // TODO: Here might be useful to issue unused variable diagnostic,
    // because we don't do declaration extraction and the decl won't be visible
    // anymore.
    ignoreFakeDiagnostics();

    // Wrap the expression
    std::string WrapperName;
    std::string Wrapper = input;
    WrapInput(Wrapper, WrapperName);
    
    const Transaction* lastT = m_IncrParser->Compile(Wrapper, CO);
    assert(lastT->getState() == Transaction::kCommitted && "Must be committed");
    if (lastT->getIssuedDiags() == Transaction::kNone)
      if (RunFunction(lastT->getWrapperFD()) < kExeFirstError)
        return Interpreter::kSuccess;

    return Interpreter::kFailure;
  }

  Interpreter::CompilationResult Interpreter::emitAllDecls(Transaction* T) {
    assert(getCodeGenerator() && "No CodeGenerator?");
    m_IncrParser->markWholeTransactionAsUsed(T);
    m_IncrParser->codeGenTransaction(T);

    // The static initializers might run anything and can thus cause more
    // decls that need to end up in a transaction. But this one is done
    // with CodeGen...
    T->setState(Transaction::kCommitted);
    if (runStaticInitializersOnce(*T))
      return Interpreter::kSuccess;

    return Interpreter::kFailure;
  }

  bool Interpreter::ShouldWrapInput(const std::string& input) {
    llvm::OwningPtr<llvm::MemoryBuffer> buf;
    buf.reset(llvm::MemoryBuffer::getMemBuffer(input, "Cling Preparse Buf"));
    Lexer WrapLexer(SourceLocation(), getSema().getLangOpts(), input.c_str(), 
                    input.c_str(), input.c_str() + input.size());
    Token Tok;
    WrapLexer.Lex(Tok);

    const tok::TokenKind kind = Tok.getKind();

    if (kind == tok::raw_identifier && !Tok.needsCleaning()) {
      StringRef keyword(Tok.getRawIdentifierData(), Tok.getLength());
      if (keyword.equals("using"))
        return false;
      if (keyword.equals("extern"))
        return false;
      if (keyword.equals("namespace"))
        return false;
      if (keyword.equals("template"))
        return false;
    }
    else if (kind == tok::hash) {
      WrapLexer.Lex(Tok);
      if (Tok.is(tok::raw_identifier) && !Tok.needsCleaning()) {
        StringRef keyword(Tok.getRawIdentifierData(), Tok.getLength());
        if (keyword.equals("include"))
          return false;
      }
    }

    return true;
  }

  void Interpreter::WrapInput(std::string& input, std::string& fname) {
    fname = createUniqueWrapper();
    input.insert(0, "void " + fname + "() {\n ");
    input.append("\n;\n}");
  }

  Interpreter::ExecutionResult
  Interpreter::RunFunction(const FunctionDecl* FD, StoredValueRef* res /*=0*/) {
    if (getCI()->getDiagnostics().hasErrorOccurred())
      return kExeCompilationError;

    if (!m_IncrParser->hasCodeGenerator()) {
      return kExeNoCodeGen;
    }

    if (!FD)
      return kExeUnkownFunction;

    std::string mangledNameIfNeeded;
    maybeMangleDeclName(FD, mangledNameIfNeeded);
    ExecutionContext::ExecutionResult ExeRes =
       m_ExecutionContext->executeFunction(mangledNameIfNeeded.c_str(),
                                           getCI()->getASTContext(),
                                           FD->getResultType(), res);
    if (res && res->isValid())
      res->get().setLLVMType(getLLVMType(res->get().getClangType()));
    return ConvertExecutionResult(ExeRes);
  }

  void Interpreter::createUniqueName(std::string& out) {
    out += utils::Synthesize::UniquePrefix;
    llvm::raw_string_ostream(out) << m_UniqueCounter++;
  }

  bool Interpreter::isUniqueName(llvm::StringRef name) {
    return name.startswith(utils::Synthesize::UniquePrefix);
  }

  llvm::StringRef Interpreter::createUniqueWrapper() {
    const size_t size 
      = sizeof(utils::Synthesize::UniquePrefix) + sizeof(m_UniqueCounter);
    llvm::SmallString<size> out(utils::Synthesize::UniquePrefix);
    llvm::raw_svector_ostream(out) << m_UniqueCounter++;

    return (getCI()->getASTContext().Idents.getOwn(out)).getName();
  }

  bool Interpreter::isUniqueWrapper(llvm::StringRef name) {
    return name.startswith(utils::Synthesize::UniquePrefix);
  }

  Interpreter::CompilationResult
  Interpreter::DeclareInternal(const std::string& input, 
                               const CompilationOptions& CO,
                               Transaction** T /* = 0 */) const {
    // Disable warnings which doesn't make sense when using the prompt
    // This gets reset with the clang::Diagnostics().Reset()
    ignoreFakeDiagnostics();

    Transaction* lastT = m_IncrParser->Compile(input, CO);
    if (lastT->getIssuedDiags() != Transaction::kErrors) {
      if (T)
        *T = lastT;
      return Interpreter::kSuccess;
    }

    return Interpreter::kFailure;
  }

  Interpreter::CompilationResult
  Interpreter::EvaluateInternal(const std::string& input, 
                                const CompilationOptions& CO,
                                StoredValueRef* V, /* = 0 */
                                Transaction** T /* = 0 */) {
    // Disable warnings which doesn't make sense when using the prompt
    // This gets reset with the clang::Diagnostics().Reset()
    ignoreFakeDiagnostics();

    // Wrap the expression
    std::string WrapperName;
    std::string Wrapper = input;
    WrapInput(Wrapper, WrapperName);

    Transaction* lastT = m_IncrParser->Compile(Wrapper, CO);
    assert((!V || lastT->size()) && "No decls created!?");
    assert((lastT->getState() == Transaction::kCommitted
           || lastT->getState() == Transaction::kRolledBack) 
           && "Not committed?");
    assert(lastT->getWrapperFD() && "Must have wrapper!");
    if (lastT->getIssuedDiags() != Transaction::kErrors)
      if (RunFunction(lastT->getWrapperFD(), V) < kExeFirstError)
        return Interpreter::kSuccess;
    if (V)
      *V = StoredValueRef::invalidValue();

    return Interpreter::kFailure;
  }

  void Interpreter::addLoadedFile(const std::string& name,
                                  Interpreter::LoadedFileInfo::FileType type,
                                  const void* dyLibHandle) {
    m_LoadedFiles.push_back(new LoadedFileInfo(name, type, dyLibHandle));
  }

  Interpreter::CompilationResult
  Interpreter::loadFile(const std::string& filename,
                        bool allowSharedLib /*=true*/) {
    if (allowSharedLib) {
      bool tryCode;
      if (loadLibrary(filename, false, &tryCode) == kLoadLibSuccess)
        return kSuccess;
      if (!tryCode)
        return kFailure;
    }

    std::string code;
    code += "#include \"" + filename + "\"";
    CompilationResult res = declare(code);
    if (res == kSuccess)
      addLoadedFile(filename, LoadedFileInfo::kSource);
    return res;
  }

  static llvm::sys::Path
  findSharedLibrary(llvm::StringRef fileStem,
                    const llvm::SmallVectorImpl<llvm::sys::Path>& Paths,
                    bool& exists, bool& isDyLib) {
    for (llvm::SmallVectorImpl<llvm::sys::Path>::const_iterator
        IPath = Paths.begin(), EPath = Paths.end(); IPath != EPath; ++IPath) {
      llvm::sys::Path ThisPath(*IPath);
      ThisPath.appendComponent(fileStem);
      exists = llvm::sys::fs::exists(ThisPath.str());
      if (exists && ThisPath.isDynamicLibrary()) {
        isDyLib = true;
        return ThisPath;
      }
    }
    return llvm::sys::Path();
  }

  Interpreter::LoadLibResult
  Interpreter::tryLinker(const std::string& filename, bool permanent,
                         bool isAbsolute, bool& exists, bool& isDyLib) {
    using namespace llvm::sys;
    exists = false;
    isDyLib = false;
    llvm::Module* module = m_IncrParser->getCodeGenerator()->GetModule();
    assert(module && "Module must exist for linking!");

    llvm::sys::Path FoundDyLib;

    if (isAbsolute) {
      exists = llvm::sys::fs::exists(filename.c_str());
      if (exists && Path(filename).isDynamicLibrary()) {
        isDyLib = true;
        FoundDyLib = filename;
      }
    } else {
      const InvocationOptions& Opts = getOptions();
      llvm::SmallVector<Path, 16>
      SearchPaths(Opts.LibSearchPath.begin(), Opts.LibSearchPath.end());

      std::vector<Path> SysSearchPaths;
      Path::GetSystemLibraryPaths(SysSearchPaths);
      SearchPaths.append(SysSearchPaths.begin(), SysSearchPaths.end());

      FoundDyLib = findSharedLibrary(filename, SearchPaths, exists, isDyLib);

      std::string filenameWithExt(filename);
      filenameWithExt += ("." + Path::GetDLLSuffix()).str();
      if (!exists) {
        // Add DyLib extension:
        FoundDyLib = findSharedLibrary(filenameWithExt, SearchPaths, exists,
                                       isDyLib);
      }
    }

    if (!isDyLib)
      return kLoadLibError;
    
    assert(!FoundDyLib.isEmpty() && "The shared lib exists but can't find it!");

    //FIXME: This is meant to be an workaround for very hard to trace bug.
    PushTransactionRAII RAII(this);

    // TODO: !permanent case
#ifdef WIN32
    void* DyLibHandle = needs to be implemented!;
    std::string errMsg;
#else
    const void* DyLibHandle
      = dlopen(FoundDyLib.str().c_str(), RTLD_LAZY|RTLD_GLOBAL);
    std::string errMsg;
    if (const char* DyLibError = dlerror()) {
      errMsg = DyLibError;
    }
#endif
    if (!DyLibHandle) {
      llvm::errs() << "cling::Interpreter::tryLinker(): " << errMsg << '\n';
      return kLoadLibError;
    }
    std::pair<std::set<const void*>::iterator, bool> insRes
      = m_DyLibs.insert(DyLibHandle);
    if (!insRes.second)
      return kLoadLibExists;
    addLoadedFile(FoundDyLib.str(), LoadedFileInfo::kDynamicLibrary,
                  DyLibHandle);
    return kLoadLibSuccess;
  }

  Interpreter::LoadLibResult
  Interpreter::loadLibrary(const std::string& filename, bool permanent,
                           bool* tryCode) {
    //FIXME: This is meant to be an workaround for very hard to trace bug.
    PushTransactionRAII RAII(this);
    // If it's not an absolute path, prepend "lib"
    SmallVector<char, 128> Absolute(filename.c_str(),
                                    filename.c_str() + filename.length());
    Absolute.push_back(0);
    llvm::sys::fs::make_absolute(Absolute);
    bool isAbsolute = filename == Absolute.data();
    bool exists = false;
    bool isDyLib = false;
    LoadLibResult res = tryLinker(filename, permanent, isAbsolute, exists,
                                  isDyLib);
    if (tryCode) {
      *tryCode = !isDyLib;
      if (isAbsolute)
        *tryCode &= exists;
    }
    if (exists)
      return res;

    if (!isAbsolute && filename.compare(0, 3, "lib")) {
      // try with "lib" prefix:
      res = tryLinker("lib" + filename, permanent, false, exists, isDyLib);
      if (tryCode) {
        *tryCode = !isDyLib;
        if (isAbsolute)
          *tryCode &= exists;
      }
      if (res != kLoadLibError)
        return res;
    }
    return kLoadLibError;
  }

  void
  Interpreter::ExposeHiddenSharedLibrarySymbols(void* DyLibHandle) {
    llvm::sys::DynamicLibrary::addPermanentLibrary(const_cast<void*>(DyLibHandle));
  }


  void Interpreter::installLazyFunctionCreator(void* (*fp)(const std::string&)) {
    m_ExecutionContext->installLazyFunctionCreator(fp);
  }

  void Interpreter::suppressLazyFunctionCreatorDiags(bool suppressed/*=true*/) {
    m_ExecutionContext->suppressLazyFunctionCreatorDiags(suppressed);
  }

  StoredValueRef Interpreter::Evaluate(const char* expr, DeclContext* DC,
                                       bool ValuePrinterReq) {
    Sema& TheSema = getCI()->getSema();
    // The evaluation should happen on the global scope, because of the wrapper
    // that is created. 
    //
    // We can't PushDeclContext, because we don't have scope.
    Sema::ContextRAII pushDC(TheSema, 
                             TheSema.getASTContext().getTranslationUnitDecl());

    StoredValueRef Result;
    getCallbacks()->SetIsRuntime(true);
    if (ValuePrinterReq)
      echo(expr, &Result);
    else 
      evaluate(expr, Result);
    getCallbacks()->SetIsRuntime(false);

    return Result;
  }

  void Interpreter::setCallbacks(InterpreterCallbacks* C) {
    // We need it to enable LookupObject callback.
    m_Callbacks.reset(C);

    // FIXME: We should add a multiplexer in the ASTContext, too.
    llvm::OwningPtr<ExternalASTSource> astContextExternalSource;
    astContextExternalSource.reset(getSema().getExternalSource());
    clang::ASTContext& Ctx = getSema().getASTContext();
    // FIXME: This is a gross hack. We must make multiplexer in the astcontext,
    // or a derived class that extends what we need.
    Ctx.ExternalSource.take(); // FIXME: make sure we delete it.
    Ctx.setExternalSource(astContextExternalSource);
  }

  //FIXME: Get rid of that.
  clang::ASTDeserializationListener*
  Interpreter::getASTDeserializationListener() const {
    if (!m_Callbacks)
      return 0;
    return m_Callbacks->getInterpreterDeserializationListener();
  }


  const Transaction* Interpreter::getFirstTransaction() const {
    return m_IncrParser->getFirstTransaction();
  }

  void Interpreter::enableDynamicLookup(bool value /*=true*/) {
    m_DynamicLookupEnabled = value;

    if (isDynamicLookupEnabled()) {
     if (loadModuleForHeader("cling/Interpreter/DynamicLookupRuntimeUniverse.h")
         != kSuccess)
      declare("#include \"cling/Interpreter/DynamicLookupRuntimeUniverse.h\"");
    }
  }

  Interpreter::ExecutionResult
  Interpreter::runStaticInitializersOnce(const Transaction& T) const {
    assert(m_IncrParser->hasCodeGenerator() && "Running on what?");
    assert(T.getState() == Transaction::kCommitted && "Must be committed");
    // Forward to ExecutionContext; should not be called by
    // anyone except for IncrementalParser.
    llvm::Module* module = m_IncrParser->getCodeGenerator()->GetModule();
    ExecutionContext::ExecutionResult ExeRes
       = m_ExecutionContext->runStaticInitializersOnce(module);

    // Reset the module builder to clean up global initializers, c'tors, d'tors
    getCodeGenerator()->HandleTranslationUnit(getCI()->getASTContext());

    return ConvertExecutionResult(ExeRes);
  }

  Interpreter::ExecutionResult
  Interpreter::runStaticDestructorsOnce() {
    for (size_t I = 0, E = m_AtExitFuncs.size(); I < E; ++I) {
      const CXAAtExitElement& AEE = m_AtExitFuncs[E-I-1];
      (*AEE.m_Func)(AEE.m_Arg);
    }
    m_AtExitFuncs.clear();
    return kExeSuccess; 
  }

  int Interpreter::CXAAtExit(void (*func) (void*), void* arg, void* dso) {
    // Register a CXAAtExit function
    Decl* LastTLD 
      = m_IncrParser->getLastTransaction()->getLastDecl().getSingleDecl();
    m_AtExitFuncs.push_back(CXAAtExitElement(func, arg, dso, LastTLD));
    return 0; // happiness
  }

  void Interpreter::maybeMangleDeclName(const clang::NamedDecl* D,
                                        std::string& mangledName) const {
    ///Get the mangled name of a NamedDecl.
    ///
    ///D - mangle this decl's name
    ///mangledName - put the mangled name in here
    if (!m_MangleCtx) {
      m_MangleCtx.reset(getCI()->getASTContext().createMangleContext());
    }
    if (m_MangleCtx->shouldMangleDeclName(D)) {
      llvm::raw_string_ostream RawStr(mangledName);
      switch(D->getKind()) {
      case Decl::CXXConstructor:
        //Ctor_Complete,          // Complete object ctor
        //Ctor_Base,              // Base object ctor
        //Ctor_CompleteAllocating // Complete object allocating ctor (unused)
        m_MangleCtx->mangleCXXCtor(cast<CXXConstructorDecl>(D), 
                                   Ctor_Complete, RawStr);
        break;

      case Decl::CXXDestructor:
        //Dtor_Deleting, // Deleting dtor
        //Dtor_Complete, // Complete object dtor
        //Dtor_Base      // Base object dtor
        m_MangleCtx->mangleCXXDtor(cast<CXXDestructorDecl>(D),
                                   Dtor_Complete, RawStr);
        break;

      default :
        m_MangleCtx->mangleName(D, RawStr);
        break;
      }
      RawStr.flush();
    } else {
      mangledName = D->getNameAsString();
    }
  }

  void Interpreter::ignoreFakeDiagnostics() const {
    DiagnosticsEngine& Diag = getCI()->getDiagnostics();
    // Disable warnings which doesn't make sense when using the prompt
    // This gets reset with the clang::Diagnostics().Reset()
    Diag.setDiagnosticMapping(clang::diag::warn_unused_expr,
                              clang::diag::MAP_IGNORE, SourceLocation());
    Diag.setDiagnosticMapping(clang::diag::warn_unused_call,
                              clang::diag::MAP_IGNORE, SourceLocation());
    Diag.setDiagnosticMapping(clang::diag::warn_unused_comparison,
                              clang::diag::MAP_IGNORE, SourceLocation());
    Diag.setDiagnosticMapping(clang::diag::ext_return_has_expr,
                              clang::diag::MAP_IGNORE, SourceLocation());
    // Very very ugly. TODO: Revisit and extract out as interpreter arg
    Diag.setDiagnosticMapping(clang::diag::ext_auto_type_specifier,
                              clang::diag::MAP_IGNORE, SourceLocation());
  }

  bool Interpreter::addSymbol(const char* symbolName,  void* symbolAddress) {
    // Forward to ExecutionContext;
    if (!symbolName || !symbolAddress )
      return false;

    return m_ExecutionContext->addSymbol(symbolName,  symbolAddress);
  }

  void* Interpreter::getAddressOfGlobal(const clang::NamedDecl* D,
                                        bool* fromJIT /*=0*/) const {
    // Return a symbol's address, and whether it was jitted.
    std::string mangledName;
    maybeMangleDeclName(D, mangledName);
    return getAddressOfGlobal(mangledName.c_str(), fromJIT);
  }

  void* Interpreter::getAddressOfGlobal(const char* SymName,
                                        bool* fromJIT /*=0*/) const {
    // Return a symbol's address, and whether it was jitted.
    llvm::Module* module = m_IncrParser->getCodeGenerator()->GetModule();
    return m_ExecutionContext->getAddressOfGlobal(module, SymName, fromJIT);
  }

  const llvm::Type* Interpreter::getLLVMType(QualType QT) {
    if (!m_IncrParser->hasCodeGenerator())
      return 0;

    // Note: The first thing this routine does is getCanonicalType(), so we
    //       do not need to do that first.
    return getCodeGenerator()->ConvertType(QT);
  }

} // namespace cling
