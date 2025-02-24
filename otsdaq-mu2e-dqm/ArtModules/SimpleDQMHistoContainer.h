#ifndef _SimpleDQMHistoContainer_h_
#define _SimpleDQMHistoContainer_h_

#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"
#include "otsdaq/NetworkUtilities/TCPPublishServer.h"
#include "otsdaq/Macros/CoutMacros.h"
#include <TH1F.h>
#include <string>

namespace ots {

  class SimpleDQMHistoContainer {
  public:
    SimpleDQMHistoContainer(){};
    virtual ~SimpleDQMHistoContainer(void){};
    struct summaryInfoHist_ {
      TH1F *_Hist;
      std::string _type;
      summaryInfoHist_() { _Hist = NULL; _type = "add"; }
    };

    std::vector<summaryInfoHist_> histograms;

    void BookSummaryHistos(art::ServiceHandle<art::TFileService> tfs, std::string Name, std::string Title,
			   const int nBins, const float min, const float max, const char* folder = "Trigger_summary",
			   std::string type = "add") {
      histograms.push_back(summaryInfoHist_());
      art::TFileDirectory testDir = tfs->mkdir(folder);
      histograms.back()._Hist = 
	testDir.make<TH1F>(Name.c_str(), Title.c_str(), nBins, min, max);
      histograms.back()._type = type;
    }
  
    /* void BookHistos(art::ServiceHandle<art::TFileService> tfs, std::string Title, */
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
