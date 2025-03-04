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

#include <TBufferFile.h>
#include <TH1F.h>

#include "otsdaq-mu2e-dqm/ArtModules/SimpleDQMHistoContainer.h"
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/Macros/ProcessorPluginMacros.h"
#include "otsdaq/MessageFacility/MessageFacility.h"
#include "otsdaq/NetworkUtilities/TCPSendClient.h"

#include "Offline/Mu2eUtilities/inc/TriggerResultsNavigator.hh"

namespace ots
{
class SimpleDQM : public art::EDAnalyzer
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
		fhicl::Sequence<std::string> histType{
		    Name("histType"),
		    Comment("This parameter determines which quantity is histogrammed")};
		fhicl::Atom<int> freqDQM{
		    Name("freqDQM"),
		    Comment("Frequency for sending histograms to the data-receiver")};
		fhicl::Atom<int> diag{Name("diagLevel"), Comment("Diagnostic level"), 0};
	};

	typedef art::EDAnalyzer::Table<Config> Parameters;

	explicit SimpleDQM(Parameters const& conf);

	void analyze(art::Event const& event) override;
	void beginRun(art::Run const&) override;
	void beginJob() override;
	void endJob() override;

	void summary_trigger_fill(SimpleDQMHistoContainer* histos);
	void PlotRate(art::Event const& e);

  private:
	Config                                conf_;
	int                                   port_;
	std::string                           address_;
	std::string                           moduleTag_;
	std::vector<std::string>              histType_;
	int                                   freqDQM_, diagLevel_, evtCounter_;
	art::ServiceHandle<art::TFileService> tfs;
	SimpleDQMHistoContainer*              summary_histos = new SimpleDQMHistoContainer();
	HistoSender*                          histSender_;
	bool                                  doOnspillHist_, doOffspillHist_;
	std::string                           moduleTag;
};
}  // namespace ots

ots::SimpleDQM::SimpleDQM(Parameters const& conf)
    : art::EDAnalyzer(conf)
    , conf_(conf())
    , port_(conf().port())
    , address_(conf().address())
    , moduleTag_(conf().moduleTag())
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
		__COUT__ << "[SimpleDQM::analyze] DQM for " << histType_[0] << std::endl;
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

void ots::SimpleDQM::beginJob()
{
	__COUT__ << "[SimpleDQM::beginJob] Beginning job" << std::endl;
	summary_histos->BookSummaryHistos(tfs, "Trigger counts", 1, 0, 1);
}

void ots::SimpleDQM::analyze(art::Event const& event)
{
	++evtCounter_;

	summary_trigger_fill(summary_histos);

	if(evtCounter_ % freqDQM_ != 0)
		return;

	// send a packet AND reset the histograms
	std::map<std::string, std::vector<TH1*>> hists_to_send;

	// send the summary hists
	for(size_t i = 0; i < summary_histos->histograms.size(); i++)
	{
		__COUT__ << "[SimpleDQM::analyze] collecting summary histogram "
		         << summary_histos->histograms[i]._Hist << std::endl;
		hists_to_send[moduleTag_ + "_summary"].push_back(
		    (TH1*)summary_histos->histograms[i]._Hist->Clone());
		summary_histos->histograms[i]._Hist->Reset();
	}

	histSender_->sendHistograms(hists_to_send);
}

void ots::SimpleDQM::summary_trigger_fill(SimpleDQMHistoContainer* histos)
{
	//  __COUT__ << "filling Summary histograms..."<< std::endl;

	if(histos->histograms.size() == 0)
	{
		__COUT__ << "No histograms booked. Should they have been created elsewhere?"
		         << std::endl;
	}
	else
	{
		histos->histograms[1]._Hist->Fill(0);
	}
}

void ots::SimpleDQM::endJob() {}

void ots::SimpleDQM::beginRun(const art::Run& run) {}

DEFINE_ART_MODULE(ots::SimpleDQM)
