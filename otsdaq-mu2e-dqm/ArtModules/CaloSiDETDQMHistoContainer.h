#ifndef _CaloSiDETDQMHistoContainer_h_
#define _CaloSiDETDQMHistoContainer_h_

#include <TGraph.h>
#include <TH1F.h>
#include <string>
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/NetworkUtilities/TCPPublishServer.h"

#include "TRACE/tracemf.h"
#define TRACE_NAME "CaloSiDETDQM"

namespace ots
{

class CaloSiDETDQMHistoContainer
{
  public:
	CaloSiDETDQMHistoContainer(){};
	virtual ~CaloSiDETDQMHistoContainer(void){};
	struct InfoHist_
	{
		TH1F*       _Hist;
		std::string _type;
		InfoHist_()
		{
			_Hist = nullptr;
			_type = "add";
		}
	};
	struct InfoGraph_
	{
		TGraph* _Graph;
		InfoGraph_() { _Graph = NULL; }
	};

	InfoHist_  h1_channel_occupancy;
	InfoHist_  h1_channel_occupancy_lastevent;
	InfoGraph_ g_nhits_event;

	/// Clear the contents of every booked histogram and graph; used at run boundaries
	/// so a long-lived art process does not carry entries from one run into the next.
	void Reset(void)
	{
		if(h1_channel_occupancy._Hist)
			h1_channel_occupancy._Hist->Reset();
		if(h1_channel_occupancy_lastevent._Hist)
			h1_channel_occupancy_lastevent._Hist->Reset();
		if(g_nhits_event._Graph)
			g_nhits_event._Graph->Set(0);
	}

	// Histogram colors by status
	static void GoodHist(TH1* h)
	{
		h->SetLineColor(kAzure + 2);
		h->SetFillColor(kAzure - 9);
	}
	static void WarningHist(TH1* h)
	{
		h->SetLineColor(kOrange + 6);
		h->SetFillColor(kOrange - 4);
	}
	static void BadHist(TH1* h)
	{
		h->SetLineColor(kRed - 4);
		h->SetFillColor(kRed - 6);
	}

	static void BookHist(InfoHist_&                            h,
	                     art::ServiceHandle<art::TFileService> tfs,
	                     std::string                           Name,
	                     std::string                           Title,
	                     const int                             nBins,
	                     const float                           xmin,
	                     const float                           xmax,
	                     const char*                           folder,
	                     std::string                           type = "add")
	{
		art::TFileDirectory testDir = tfs->mkdir(folder);
		h._Hist = testDir.make<TH1F>(Name.c_str(), Title.c_str(), nBins, xmin, xmax);
		h._type = type;

		// Initialize histogram style
		h._Hist->SetLineWidth(2);
		h._Hist->GetXaxis()->SetTitleSize(.04);
		h._Hist->GetYaxis()->SetTitleSize(.04);
		GoodHist(h._Hist);
	}

	static void BookGraph(InfoGraph_&                           g,
	                      art::ServiceHandle<art::TFileService> tfs,
	                      std::string                           Name,
	                      std::string                           Title,
	                      const char*                           folder)
	{
		art::TFileDirectory testDir = tfs->mkdir(folder);
		g._Graph = testDir.makeAndRegister<TGraph>(Name.c_str(), Title.c_str());

		// Initialize histogram style
		g._Graph->SetLineWidth(2);
		g._Graph->SetMarkerStyle(20);
		g._Graph->GetXaxis()->SetTitleSize(.04);
		g._Graph->GetYaxis()->SetTitleSize(.04);
	}
};

}  // namespace ots

#endif
