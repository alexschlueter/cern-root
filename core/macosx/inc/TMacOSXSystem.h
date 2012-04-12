//Author: Timur Pocheptsov 5/12/2011

#ifndef ROOT_TMacOSXSystem
#define ROOT_TMacOSXSystem

#include <memory>

#ifndef ROOT_TUnixSystem
#include "TUnixSystem.h"
#endif

////////////////////////////////////////////////////////////////////
//                                                                //
// Event loop with MacOS X and Cocoa is different                 //
// from TUnixSystem. The difference is mainly                     //
// in DispatchOneEvent, AddFileHandler and RemoveFileHandler.     //
//                                                                //
////////////////////////////////////////////////////////////////////

namespace ROOT {
namespace MacOSX {
namespace Detail {

//'Private' pimpl class to hide Apple's specific things from CINT.
class MacOSXSystem;

}
}
}

class TMacOSXSystem : public TUnixSystem {
public:
   TMacOSXSystem();
   ~TMacOSXSystem();
   
   void DispatchOneEvent(Bool_t pendingOnly);

private:

//   void WaitForGuiEvents(Long_t nextto);
   void WaitEvents(Long_t nextto);

   void AddFileHandler(TFileHandler *fh);
   TFileHandler *RemoveFileHandler(TFileHandler *fh);

   void ProcessApplicationDefinedEvent(void *event);

   std::auto_ptr<ROOT::MacOSX::Detail::MacOSXSystem> fPimpl; //!

   TMacOSXSystem(const TMacOSXSystem &rhs);
   TMacOSXSystem &operator = (const TMacOSXSystem &rhs);
   
   ClassDef(TMacOSXSystem, 0);//TSystem for Mac OSX.
};

#endif