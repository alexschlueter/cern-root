// @(#)root/meta:$Id$
// Author: Fons Rademakers   20/06/96

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/


#ifndef ROOT_TDictionary
#define ROOT_TDictionary

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// TDictionary                                                          //
//                                                                      //
// This class defines an abstract interface that must be implemented    //
// by all classes that contain dictionary information.                  //
//                                                                      //
// The dictionary is defined by the followling classes:                 //
// TDataType                              (typedef definitions)         //
// TGlobal                                (global variables)            //
// TFunction                              (global functions)            //
// TClass                                 (classes)                     //
//    TBaseClass                          (base classes)                //
//    TDataMember                         (class datamembers)           //
//    TMethod                             (class methods)               //
//       TMethodArg                       (method arguments)            //
//                                                                      //
// All the above classes implement the TDictionary abstract interface   //
// (note: the indentation shows aggregation not inheritance).           //
// The ROOT dictionary system provides a very extensive RTTI            //
// environment that facilitates a.o. object inspectors, object I/O,     //
// ROOT Trees, etc. Most of the type information is provided by the     //
// CINT C++ interpreter.                                                //
//                                                                      //
// TMethodCall                            (method call environment)     //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

#ifndef ROOT_TNamed
#include "TNamed.h"
#endif

// The following are opaque type and are never really declared
// The specific implemenation of TInterpreter will cast the
// value of pointer to this types to correct (but possibly
// distinct from these)
class CallFunc_t;
class ClassInfo_t;
class BaseClassInfo_t;
class DataMemberInfo_t;
class MethodInfo_t;
class MethodArgInfo_t;
class MethodArgInfo_t;
class TypeInfo_t;
class TypedefInfo_t;

enum EProperty {
   kIsClass         = 0x00000001,
   kIsStruct        = 0x00000002,
   kIsUnion         = 0x00000004,
   kIsEnum          = 0x00000008,
   kIsTypedef       = 0x00000010,
   kIsFundamental   = 0x00000020,
   kIsAbstract      = 0x00000040,
   kIsVirtual       = 0x00000080,
   kIsPureVirtual   = 0x00000100,
   kIsPublic        = 0x00000200,
   kIsProtected     = 0x00000400,
   kIsPrivate       = 0x00000800,
   kIsPointer       = 0x00001000,
   kIsArray         = 0x00002000,
   kIsStatic        = 0x00004000,
   kIsDefault       = 0x00008000,
   kIsReference     = 0x00010000,
   kIsDirectInherit = 0x00020000,
   kIsCCompiled     = 0x00040000,
   kIsCPPCompiled   = 0x00080000,
   kIsCompiled      = 0x000C0000,
   kIsConstant      = 0x00100000,
   kIsVirtualBase   = 0x00200000,
   kIsConstPointer  = 0x00400000,
   kIsExplicit      = 0x04000000,
   kIsNamespace     = 0x08000000,
   kIsConstMethod   = 0x10000000,
   kIsUsingVariable = 0x20000000,
   kIsDefinedInStd  = 0x40000000
};

enum EClassProperty {
   kClassIsValid         = 0x00000001,
   kClassHasExplicitCtor = 0x00000010,
   kClassHasImplicitCtor = 0x00000020,
   kClassHasCtor         = 0x00000030,
   kClassHasDefaultCtor  = 0x00000040,
   kClassHasAssignOpr    = 0x00000080,
   kClassHasExplicitDtor = 0x00000100,
   kClassHasImplicitDtor = 0x00000200,
   kClassHasDtor         = 0x00000300,
   kClassHasVirtual      = 0x00001000,
   kClassIsAbstract      = 0x00002000
};

enum ERefTypeValues {
   kParaNormal     = 0,     // not used
   kParaReference  = 1,
   kParaP2P        = 2,     // not used
   kParaP2P2P      = 3,     // not used
   kParaRef        = 100,
   kParaRefP2P     = 102,   // not used
   kParaRefP2P2P   = 103    // not used
};

namespace ROOT {
   enum EFunctionMatchMode {
      kExactMatch = 0,
      kConversionMatch = 1
   };
}


class TDictionary : public TNamed {

public:
   TDictionary() { }
   TDictionary(const char* name): TNamed(name, "") { }
   virtual ~TDictionary() { }

   virtual Long_t      Property() const = 0;
   static TDictionary* GetDictionary(const char* name);
   static TDictionary* GetDictionary(const type_info &typeinfo);

   // Type of STL container (returned by IsSTLContainer).
   enum ESTLType {kNone=0, kVector=1, kList, kDeque, kMap, kMultimap, kSet, kMultiset};

   ClassDef(TDictionary,0)  //ABC defining interface to dictionary
};

#endif
