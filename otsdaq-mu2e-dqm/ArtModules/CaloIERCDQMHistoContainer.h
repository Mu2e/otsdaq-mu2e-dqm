#ifndef _CaloIERCDQMHistoContainer_h_
#define _CaloIERCDQMHistoContainer_h_

#include <TH1F.h>
#include <string>
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/NetworkUtilities/TCPPublishServer.h"

#include "TRACE/tracemf.h"
#define TRACE_NAME "CaloIERCDQM"

namespace ots
{

class CaloIERCDQMHistoContainer
{
  public:
	CaloIERCDQMHistoContainer(){};
	virtual ~CaloIERCDQMHistoContainer(void){};
	struct InfoHist_
	{
		TH1F* _Hist;
		InfoHist_() { _Hist = NULL; }
	};
	struct InfoGraph_
	{
		TGraph* _Graph;
		InfoGraph_() { _Graph = NULL; }
	};

	std::map<int, InfoHist_>  map_h1_dt_singlechan;
	std::map<int, InfoGraph_> map_g_dtevt_singlechan;

	std::map<int, InfoHist_>  map_h1_dt_sameboard;
	std::map<int, InfoGraph_> map_g_dtevt_sameboard;

	std::map<int, InfoHist_>  map_h1_dt_diffboard;
	std::map<int, InfoGraph_> map_g_dtevt_diffboard;

	void BookSingleChannel(art::ServiceHandle<art::TFileService> tfs,
	                       int                                   mapID,
	                       std::string                           Name,
	                       std::string                           Title,
	                       int                                   nBins,
	                       float                                 min,
	                       float                                 max)
	{
		TLOG(TLVL_DEBUG) << "Booking single channel hist for sipmID " << mapID;
		map_h1_dt_singlechan[mapID] = InfoHist_();
		art::TFileDirectory testDir = tfs->mkdir("SingleChannel");
		this->map_h1_dt_singlechan[mapID]._Hist =
		    testDir.make<TH1F>(Name.c_str(), Title.c_str(), nBins, min, max);
		this->map_h1_dt_singlechan[mapID]._Hist->GetXaxis()->SetTitle("dt [ns]");
	}
	void BookSingleChannelG(art::ServiceHandle<art::TFileService> tfs,
	                        int                                   mapID,
	                        std::string                           Name,
	                        std::string                           Title)
	{
		TLOG(TLVL_DEBUG) << "Booking single channel graph for sipmID " << mapID;
		map_g_dtevt_singlechan[mapID] = InfoGraph_();
		art::TFileDirectory testDir   = tfs->mkdir("SingleChannel");
		this->map_g_dtevt_singlechan[mapID]._Graph =
		    testDir.makeAndRegister<TGraph>(Name.c_str(), Title.c_str());
		this->map_g_dtevt_singlechan[mapID]._Graph->GetXaxis()->SetTitle("Event number");
		this->map_g_dtevt_singlechan[mapID]._Graph->GetYaxis()->SetTitle("dt [ns]");
		this->map_g_dtevt_singlechan[mapID]._Graph->SetMarkerStyle(20);
	}
	void BookSameBoard(art::ServiceHandle<art::TFileService> tfs,
	                   int                                   mapID,
	                   std::string                           Name,
	                   std::string                           Title,
	                   int                                   nBins,
	                   float                                 min,
	                   float                                 max)
	{
		TLOG(TLVL_DEBUG) << "Booking single board hist for sipmID " << mapID;
		map_h1_dt_sameboard[mapID]  = InfoHist_();
		art::TFileDirectory testDir = tfs->mkdir("SameBoard");
		this->map_h1_dt_sameboard[mapID]._Hist =
		    testDir.make<TH1F>(Name.c_str(), Title.c_str(), nBins, min, max);
		this->map_h1_dt_sameboard[mapID]._Hist->GetXaxis()->SetTitle("dt [ns]");
	}
	void BookSameBoardG(art::ServiceHandle<art::TFileService> tfs,
	                    int                                   mapID,
	                    std::string                           Name,
	                    std::string                           Title)
	{
		TLOG(TLVL_DEBUG) << "Booking single board graph for sipmID " << mapID;
		map_g_dtevt_sameboard[mapID] = InfoGraph_();
		art::TFileDirectory testDir  = tfs->mkdir("SameBoard");
		this->map_g_dtevt_sameboard[mapID]._Graph =
		    testDir.makeAndRegister<TGraph>(Name.c_str(), Title.c_str());
		this->map_g_dtevt_sameboard[mapID]._Graph->GetXaxis()->SetTitle("Event number");
		this->map_g_dtevt_sameboard[mapID]._Graph->GetYaxis()->SetTitle("dt [ns]");
		this->map_g_dtevt_sameboard[mapID]._Graph->SetMarkerStyle(20);
	}
	void BookDiffBoard(art::ServiceHandle<art::TFileService> tfs,
	                   int                                   mapID,
	                   std::string                           Name,
	                   std::string                           Title,
	                   int                                   nBins,
	                   float                                 min,
	                   float                                 max)
	{
		TLOG(TLVL_DEBUG) << "Booking diff board hist for sipmID " << mapID;
		map_h1_dt_diffboard[mapID]  = InfoHist_();
		art::TFileDirectory testDir = tfs->mkdir("DiffBoard");
		this->map_h1_dt_diffboard[mapID]._Hist =
		    testDir.make<TH1F>(Name.c_str(), Title.c_str(), nBins, min, max);
		this->map_h1_dt_diffboard[mapID]._Hist->GetXaxis()->SetTitle("dt [ns]");
	}
	void BookDiffBoardG(art::ServiceHandle<art::TFileService> tfs,
	                    int                                   mapID,
	                    std::string                           Name,
	                    std::string                           Title)
	{
		TLOG(TLVL_DEBUG) << "Booking diff board graph for sipmID " << mapID;
		map_g_dtevt_diffboard[mapID] = InfoGraph_();
		art::TFileDirectory testDir  = tfs->mkdir("DiffBoard");
		this->map_g_dtevt_diffboard[mapID]._Graph =
		    testDir.makeAndRegister<TGraph>(Name.c_str(), Title.c_str());
		this->map_g_dtevt_diffboard[mapID]._Graph->GetXaxis()->SetTitle("Event number");
		this->map_g_dtevt_diffboard[mapID]._Graph->GetYaxis()->SetTitle("dt [ns]");
		this->map_g_dtevt_diffboard[mapID]._Graph->SetMarkerStyle(20);
	}
};

}  // namespace ots

#endif
