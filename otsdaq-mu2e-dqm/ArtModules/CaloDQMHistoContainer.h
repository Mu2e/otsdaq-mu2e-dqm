#ifndef _CaloDQMHistoContainer_h_
#define _CaloDQMHistoContainer_h_

#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/NetworkUtilities/TCPPublishServer.h"
#include <TH1F.h>
#include <string>

namespace ots {

class CaloDQMHistoContainer {
public:
  CaloDQMHistoContainer(){};
  virtual ~CaloDQMHistoContainer(void){};
  struct summaryInfoHist_ {
    TH1F *_Hist;
    int plane;
    int panel;
    int straw;
    summaryInfoHist_() { _Hist = NULL; }
  };

  std::vector<summaryInfoHist_> histograms;

  void BookSummaryHistos(art::ServiceHandle<art::TFileService> tfs,
                         std::string Title, int nBins, float min, float max) {
    histograms.push_back(summaryInfoHist_());
    art::TFileDirectory testDir = tfs->mkdir("Calo_summary");
    this->histograms[histograms.size() - 1]._Hist =
        testDir.make<TH1F>(Title.c_str(), Title.c_str(), nBins, min, max);
  }

  /* void BookHistos(art::ServiceHandle<art::TFileService> tfs, std::string
   * Title, */
  /* 		    int plane, int panel, int straw) { */
  /*   histograms.push_back(summaryInfoHist_()); */
  /*   std::string         dirName = "plane_"+std::to_string(plane); */
  /*   int                 nBins(100); */
  /*   float               hMin(0), hMax(100); */
  /*   art::TFileDirectory testDir = tfs->mkdir(dirName); */

  /*   if(straw>=0){//histograms are straw-specific, aka pedestals */
  /* 	std::string subDirN = "panel_"  +std::to_string(panel); */
  /* 	dirName  += "/"+subDirN; */
  /* 	art::TFileDirectory subDir  = testDir.mkdir(subDirN); */
  /* 	nBins    = 200; */
  /* 	hMax     = 500.; */
  /*   } */

  /*   this->histograms[histograms.size() - 1]._Hist = */
  /* 	testDir.make<TH1F>(Title.c_str(), Title.c_str(), nBins, hMin, hMax); */
  /*   this->histograms[histograms.size() - 1].plane = plane; */
  /*   this->histograms[histograms.size() - 1].panel = panel; */
  /*   this->histograms[histograms.size() - 1].straw = straw; */
  /* } */
};

} // namespace ots

#endif
