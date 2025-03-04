// Author: G. Pezzullo
// This module produces histograms of data from the TriggerResults

#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art/Framework/Services/System/TriggerNamesService.h"
#include "art_root_io/TFileService.h"
#include "canvas/Persistency/Common/TriggerResults.h"
#include "fhiclcpp/types/OptionalAtom.h"

#include "TRACE/tracemf.h"

#include <TBufferFile.h>
#include <TF1.h>
#include <TFile.h>
#include <TGraph.h>
#include <TGraphErrors.h>
#include <TH1F.h>
#include <TSpline.h>
#include <TVirtualFitter.h>
#include <map>
#include <vector>

#include "otsdaq-mu2e-dqm/ArtModules/CaloSiDETDQMHistoContainer.h"
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/Macros/ProcessorPluginMacros.h"
#include "otsdaq/MessageFacility/MessageFacility.h"
#include "otsdaq/NetworkUtilities/TCPSendClient.h"

//-- insert calls to proditions ..for calodmap-----
#include "Offline/CaloConditions/inc/CaloDAQMap.hh"
#include "Offline/ProditionsService/inc/ProditionsHandle.hh"
//-------------------------------------------------

#include "Offline/DAQ/inc/CaloDAQUtilities.hh"
#include "Offline/RecoDataProducts/inc/CaloDigi.hh"

namespace ots
{
class CaloSiDETDQM : public art::EDAnalyzer
{
  public:
	struct Config
	{
		using Name    = fhicl::Name;
		using Comment = fhicl::Comment;
		fhicl::Atom<int> port{
		    Name("port"),
		    Comment("This parameter sets the port where the histogram will be sent")};
		fhicl::Atom<std::string> address{
		    Name("address"),
		    Comment("This paramter sets the IP address where the "
		            "histogram will be sent")};
		fhicl::Atom<std::string> moduleTag{Name("moduleTag"), Comment("Module tag name")};
		fhicl::Atom<std::string> caloDigiTag{Name("caloDigiTag"),
		                                     Comment("Module tag name")};
		fhicl::Sequence<std::string> histType{
		    Name("histType"),
		    Comment("This parameter determines which quantity is histogrammed")};
		fhicl::Atom<int> freqDQM{
		    Name("freqDQM"),
		    Comment("Frequency for sending histograms to the data-receiver")};
		fhicl::Atom<int>         diag{Name("diagLevel"), Comment("Diagnostic level"), 0};
		fhicl::Atom<std::string> spline{
		    Name("splineFilename"), Comment("spline root file path"), ""};
		fhicl::Atom<bool> uset0{
		    Name("uset0"), Comment("Use t0 instead of fitting with templates"), false};
		fhicl::Atom<int> skipAfterN{
		    Name("skipAfterN"), Comment("Don't fit after N hits"), -1};
	};

	typedef art::EDAnalyzer::Table<Config> Parameters;

	explicit CaloSiDETDQM(Parameters const& conf);

	void analyze(art::Event const& event) override;
	void beginRun(art::Run const&) override;
	void beginJob() override;
	void endJob() override;

	void summary_fill(art::Event const&               event,
	                  CaloSiDETDQMHistoContainer*     histos,
	                  const mu2e::CaloDigiCollection* caloDigis);
	void PlotRate(art::Event const& e);

  private:
	Config                                conf_;
	int                                   port_;
	std::string                           address_;
	std::string                           moduleTag_;
	std::string                           caloDigiTag_;
	std::vector<std::string>              histType_;
	int                                   freqDQM_, diagLevel_, evtCounter_;
	art::ServiceHandle<art::TFileService> tfs;
	CaloSiDETDQMHistoContainer* histo_container = new CaloSiDETDQMHistoContainer();
	HistoSender*                histSender_;
	bool                        doOnspillHist_, doOffspillHist_;
	mu2e::ProditionsHandle<mu2e::CaloDAQMap> _calodaqconds_h;

	int this_eventNumber;
};
}  // namespace ots

ots::CaloSiDETDQM::CaloSiDETDQM(Parameters const& conf)
    : art::EDAnalyzer(conf)
    , conf_(conf())
    , port_(conf().port())
    , address_(conf().address())
    , moduleTag_(conf().moduleTag())
    , caloDigiTag_(conf().caloDigiTag())
    , histType_(conf().histType())
    , freqDQM_(conf().freqDQM())
    , diagLevel_(conf().diag())
    , evtCounter_(0)
    , doOnspillHist_(false)
    , doOffspillHist_(false)
{
	histSender_ = new HistoSender(address_, port_);

	if(diagLevel_ > 0)
	{
		__COUT__ << "[CaloSiDETDQM::analyze] DQM for " << histType_[0] << std::endl;
	}

	for(std::string name : histType_)
	{
		if(name == "Onspill")
		{
			doOnspillHist_ = true;
		}
		if(name == "Offspill")
		{
			doOffspillHist_ = true;
		}
	}
}

void ots::CaloSiDETDQM::beginJob()
{
	__COUT__ << "[CaloSiDETDQM::beginJob] Beginning job" << std::endl;

	histo_container->BookHist(histo_container->h1_channel_occupancy,
	                          tfs,
	                          "h1_channel_occupancy",
	                          "Channel occupancy;Board*100+Channel;Total hits",
	                          16000,
	                          0,
	                          16000,
	                          "summary");

	histo_container->BookHist(
	    histo_container->h1_channel_occupancy_lastevent,
	    tfs,
	    "h1_channel_occupancy_lastevent",
	    "Channel occupancy in the last event;Board*100+Channel;Total hits",
	    16000,
	    0,
	    16000,
	    "summary");

	histo_container->BookGraph(histo_container->g_nhits_event,
	                           tfs,
	                           "g_nhits_event",
	                           "Channel occupancy;Board*100+Channel;Total hits",
	                           "summary");
}

void ots::CaloSiDETDQM::analyze(art::Event const& event)
{
	++evtCounter_;

	art::EventNumber_t eventNumber = event.event();
	this_eventNumber               = (int)eventNumber;
	TLOG(TLVL_DEBUG) << "Analyzing event " << this_eventNumber;

	art::Handle<mu2e::CaloDigiCollection> caloDigisHandle;
	if(!event.getByLabel(caloDigiTag_, caloDigisHandle))
	{
		__COUT__ << "[CaloSiDETDQM::" << __func__ << "] No calo digis found with tag "
		         << caloDigiTag_.c_str() << std::endl;
		return;
	}
	else if(diagLevel_ > 0)
	{
		__COUT__ << "[CaloSiDETDQM::" << __func__
		         << "] Calo digi collection handle found\n";
	}

	auto caloDigis = caloDigisHandle.product();
	if(!caloDigis)
	{
		__COUT__ << "[CaloSiDETDQM::" << __func__ << "] No calo digis found with tag "
		         << caloDigiTag_.c_str() << std::endl;
		return;
	}
	else if(diagLevel_ > 0)
	{
		__COUT__ << "[CaloSiDETDQM::" << __func__ << "] Calo digi collection found\n";
	}

	if(!caloDigis)
	{
		TLOG(TLVL_DEBUG) << "CaloDigi pointer is null";
	}
	else
	{
		TLOG(TLVL_DEBUG) << "CaloDigi pointer has size " << caloDigis->size();
	}
	TLOG(TLVL_DEBUG) << "Filling calo DQM plots for event " << this_eventNumber;
	summary_fill(event, histo_container, caloDigis);

	if(evtCounter_ % freqDQM_ != 0)
		return;

	// send a packet AND reset the histograms
	std::map<std::string, std::vector<TH1*>>    hists_to_send;
	std::map<std::string, std::vector<TGraph*>> graphs_to_send;

	// send the summary hists

	hists_to_send[moduleTag_ + ":replace"].push_back(
	    (TH1*)histo_container->h1_channel_occupancy._Hist->Clone());
	hists_to_send[moduleTag_ + ":replace"].push_back(
	    (TH1*)histo_container->h1_channel_occupancy_lastevent._Hist->Clone());
	graphs_to_send[moduleTag_ + ":replace"].push_back(
	    (TGraph*)histo_container->g_nhits_event._Graph->Clone());

	histSender_->sendHistograms(hists_to_send);
	histSender_->sendGraphs(graphs_to_send);
}

void ots::CaloSiDETDQM::summary_fill(art::Event const&               event,
                                     CaloSiDETDQMHistoContainer*     histo_container,
                                     const mu2e::CaloDigiCollection* caloDigis)
{
	histo_container->h1_channel_occupancy_lastevent._Hist->Reset();

	mu2e::CaloDAQMap const& calodaqconds = _calodaqconds_h.get(event.id());

	TLOG(TLVL_DEBUG) << "There are " << caloDigis->size() << " calo digis";
	for(uint ihit = 0; ihit < caloDigis->size(); ihit++)
	{
		int SiPMID    = caloDigis->at(ihit).SiPMID();
		int BoardChan = calodaqconds.rawId(mu2e::CaloSiPMId(SiPMID)).id();
		int boardID   = BoardChan / 20;
		int chanID    = BoardChan % 20;
		// float thisTime = caloDigis->at(ihit).t0();

		histo_container->h1_channel_occupancy._Hist->Fill(boardID * 100 + chanID);
		histo_container->h1_channel_occupancy_lastevent._Hist->Fill(boardID * 100 +
		                                                            chanID);
	}

	histo_container->g_nhits_event._Graph->AddPoint(this_eventNumber, caloDigis->size());
}

void ots::CaloSiDETDQM::endJob() {}

void ots::CaloSiDETDQM::beginRun(const art::Run& run) {}

DEFINE_ART_MODULE(ots::CaloSiDETDQM)
