// Author: G. Pezzullo
// This module produces an example DQM histogram and sends it to the visualizer

#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art_root_io/TFileService.h"

#include <TBufferFile.h>
#include <TH1.h>
#include <TH1F.h>

#include "otsdaq-mu2e/ArtModules/HistoSender.hh"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/Macros/ProcessorPluginMacros.h"
#include "otsdaq/MessageFacility/MessageFacility.h"
#include "otsdaq/NetworkUtilities/TCPSendClient.h"

#include "trace.h"
#define TRACE_NAME "SimpleDQM"

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
		fhicl::Atom<std::string> outputTag{Name("outputTag"), Comment("Tag to use in output name")};
		fhicl::Atom<int> freqDQM{
		    Name("freqDQM"),
		    Comment("Frequency for sending histograms to the data-receiver")};
		fhicl::Atom<int> diag{Name("diagLevel"), Comment("Diagnostic level"), 0};
		fhicl::Atom<bool> sendHists{
		    Name("sendHists"),
		    Comment("Whether or not to send histograms to the receiver"), true};
	};

	typedef art::EDAnalyzer::Table<Config> Parameters;

	explicit SimpleDQM(Parameters const& conf);

	void analyze(art::Event const& event) override;
	void beginRun(art::Run const&) override;
	void beginJob() override;
	void endJob() override;

  private:
	Config                                conf_;
	int                                   port_;
	std::string                           address_;
	std::string                           outputTag_;
	int                                   freqDQM_, diagLevel_, evtCounter_;
        bool                                  sendHists_;
	art::ServiceHandle<art::TFileService> tfs;
	HistoSender*                          histSender_;

        // Simple event counter
        TH1* hist_;
};
}  // namespace ots

ots::SimpleDQM::SimpleDQM(Parameters const& conf)
    : art::EDAnalyzer(conf)
    , conf_(conf())
    , port_(conf().port())
    , address_(conf().address())
    , outputTag_(conf().outputTag())
    , freqDQM_(conf().freqDQM())
    , diagLevel_(conf().diag())
    , evtCounter_(0)
    , sendHists_(conf().sendHists())
{
        histSender_ = (sendHists_) ? new HistoSender(address_, port_) : nullptr;

	__COUT__ << "[SimpleDQM::" << __func__ << "] Constructor" << std::endl;
}

void ots::SimpleDQM::beginJob()
{
        __COUT__ << "[SimpleDQM::" << __func__ << "] Beginning job" << std::endl;

	// Create the histograms of interest
	art::TFileDirectory dir = tfs->mkdir(outputTag_);
	hist_ = dir.make<TH1F>("event_count", "Event counter;Bin;Events", 1, 0, 1);
	hist_->SetLineWidth(2);
	hist_->SetFillColor(kAzure-9);
	hist_->SetLineColor(kAzure+2);
	hist_->SetFillStyle(3001);
}

void ots::SimpleDQM::analyze(art::Event const& event)
{
        // fill the histograms of interest
	++evtCounter_;
	hist_->Fill(1);
        TLOG(TLVL_DEBUG + 20) << "[SimpleDQM::" << __func__ << "] Analyzing event " << evtCounter_ << std::endl;

	// Only send histograms at a given frequency
	if(evtCounter_ % freqDQM_ != 0 || !sendHists_) return;

	// Add a clone of the histogram to a map of <path/in/output, <list of histograms>>
	std::map<std::string, std::vector<TH1*>> hists_to_send;
 	TLOG(TLVL_DEBUG + 5) << "[SimpleDQM::" << __func__ << "] collecting summary histogram "
			     << hist_->GetName() << std::endl;
	// histograms can be replaced with ":replace" or added together with ":add" appended to the directory path
	hists_to_send[outputTag_ + "_summary:replace"].push_back((TH1*) hist_->Clone());

	// send the histograms to the receiver
	histSender_->sendHistograms(hists_to_send);
}

void ots::SimpleDQM::endJob() {
  __COUT__ << "[SimpleDQM::" << __func__ << "] Ending job, saw " << evtCounter_ << " events\n";
}

void ots::SimpleDQM::beginRun(const art::Run&) {}

DEFINE_ART_MODULE(ots::SimpleDQM)
