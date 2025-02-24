#ifndef _ProtoTypeHistos_h_
#define _ProtoTypeHistos_h_

#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"
#include "otsdaq/NetworkUtilities/TCPPublishServer.h"
#include <TH1F.h>
#include <string>

namespace ots {

class ProtoTypeHistos {
public:
  ProtoTypeHistos(const std::string Title) : _title(Title){};
  ProtoTypeHistos(){};
  virtual ~ProtoTypeHistos(void){};
  struct summaryInfoHist_ {
    TH1F *_FirstHist;
    summaryInfoHist_() { _FirstHist = NULL; }
  };
  const std::string _title;
  summaryInfoHist_ Test;

  void BookHistos(art::ServiceHandle<art::TFileService> tfs) {
    art::TFileDirectory TestDir = tfs->mkdir("TestingHistos");
    this->Test._FirstHist = TestDir.make<TH1F>("test", "test", 1000, 0, 110);
  }

  void BookHistos(TDirectory *dir) {
    dir->mkdir("TestingHistos", "TestingHistos");
    this->Test._FirstHist = new TH1F("test", "test", 1000, 0, 110);
  }
};

} // namespace ots

#endif
