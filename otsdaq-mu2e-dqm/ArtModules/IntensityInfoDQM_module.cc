// Author: G. Pezzullo
// This module produces histograms of data from the TriggerResults

#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art_root_io/TFileService.h"
#include "fhiclcpp/types/OptionalAtom.h"
#include "canvas/Persistency/Common/TriggerResults.h"
#include "art/Framework/Services/System/TriggerNamesService.h"

#include <TBufferFile.h>
#include <TH1F.h>

#include "otsdaq-mu2e-dqm/ArtModules/IntensityInfoDQMHistoContainer.h"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/Macros/ProcessorPluginMacros.h"
#include "otsdaq/MessageFacility/MessageFacility.h"
#include "otsdaq/NetworkUtilities/TCPSendClient.h"
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"

#include "Offline/RecoDataProducts/inc/CaloHit.hh"
#include "Offline/RecoDataProducts/inc/IntensityInfoCalo.hh"
#include "Offline/RecoDataProducts/inc/IntensityInfoTrackerHits.hh"

namespace ots {
  class IntensityInfoDQM : public art::EDAnalyzer {
  public:
    struct Config {
      using Name = fhicl::Name;
      using Comment = fhicl::Comment;
      fhicl::Atom<int>             port      { Name("port"),      Comment("This parameter sets the port where the histogram will be sent") };
      fhicl::Atom<std::string>     address   { Name("address"),   Comment("This paramter sets the IP address where the histogram will be sent") };
      fhicl::Atom<std::string>     moduleTag { Name("moduleTag"), Comment("Module tag name") };
      fhicl::Sequence<std::string> histType  { Name("histType"),  Comment("This parameter determines which quantity is histogrammed") };
      fhicl::Atom<int>             freqDQM   { Name("freqDQM"),   Comment("Frequency for sending histograms to the data-receiver") };
      fhicl::Atom<int>             diag      { Name("diagLevel"), Comment("Diagnostic level"), 0 };
    };

    typedef art::EDAnalyzer::Table<Config> Parameters;

    explicit IntensityInfoDQM(Parameters const& conf);

    void analyze(art::Event const& event) override;
    void beginRun(art::Run const&) override;
    void beginJob() override;
    void endJob() override;

    void summary_fill(IntensityInfoDQMHistoContainer *histos, 
		      const mu2e::CaloHitCollection        *CAPHRIHits,
		      const mu2e::IntensityInfoCalo        *CaloInfos, 
		      const mu2e::IntensityInfoTrackerHits *TrkInfos);
    void PlotRate(art::Event const& e);

  private:
    Config                    conf_;
    int                       port_;
    std::string               address_;
    std::string               moduleTag_;
    std::vector<std::string>  histType_;
    int                       freqDQM_,  diagLevel_, evtCounter_;
    art::ServiceHandle<art::TFileService> tfs;
    IntensityInfoDQMHistoContainer* summary_histos  = new IntensityInfoDQMHistoContainer();
    HistoSender*              histSender_;
    bool                      doOnspillHist_, doOffspillHist_;
    std::string               moduleTag;
    
  };
} // namespace ots

ots::IntensityInfoDQM::IntensityInfoDQM(Parameters const& conf)
  : art::EDAnalyzer(conf), conf_(conf()), port_(conf().port()), address_(conf().address()),
    moduleTag_(conf().moduleTag()), histType_(conf().histType()), 
    freqDQM_(conf().freqDQM()), diagLevel_(conf().diag()), evtCounter_(0), 
    doOnspillHist_(false), doOffspillHist_(false) {
  histSender_  = new HistoSender(address_, port_);
  
  if (diagLevel_>0){
    __COUT__ << "[IntensityInfoDQM::analyze] DQM for "<< histType_[0] << std::endl;
  }

  for (std::string name : histType_) {
    if (name == "Onspill") {
      doOnspillHist_ = true;
    }
    if (name == "Offspill") {
      doOffspillHist_ = true;
    }
  }
}

void ots::IntensityInfoDQM::beginJob() {
  __COUT__ << "[IntensityInfoDQM::beginJob] Beginning job" << std::endl;
  summary_histos->BookSummaryHistos(tfs,
				    "CAPHRI hits; nCAPHRIHits; Events",
				    100, 0, 100);
  //caloInfo
  summary_histos->BookSummaryHistos(tfs,
				    "IntensityInfo Calo, nHits; nCaloHits; Events/60"  ,
				    200, 0, 12e3);
  summary_histos->BookSummaryHistos(tfs,
				    "IntensityInfo Calo, caloEnergy; E[MeV]; Events/(5 MeV)"  , 
				    400, 0, 2e3);

  //tracker info
  summary_histos->BookSummaryHistos(tfs,
				    "IntensityInfo Tracker; nTrkHits", 200, 0, 12e3);
}

void ots::IntensityInfoDQM::analyze(art::Event const& event) {
  ++evtCounter_;
 
  auto const caphriH   = event.getValidHandle<mu2e::CaloHitCollection>("CaloHitMakerFast::caphri");
  const mu2e::CaloHitCollection         *caphriHits = caphriH.product();
  
  auto const caloH   = event.getValidHandle<mu2e::IntensityInfoCalo>("CaloHitMakerFast");
  const mu2e::IntensityInfoCalo         *caloInfos = caloH.product();

  auto const trkH   = event.getValidHandle<mu2e::IntensityInfoTrackerHits>("TTmakeSH");
  const mu2e::IntensityInfoTrackerHits  *trkInfos = trkH.product();

  
  summary_fill(summary_histos, caphriHits, caloInfos, trkInfos);
  

  if (evtCounter_ % freqDQM_  != 0) return;

  //send a packet AND reset the histograms
  std::map<std::string,std::vector<TH1*>>   hists_to_send;
  
  //send the summary hists
  for (size_t i = 0; i < summary_histos->histograms.size(); i++) {
    __COUT__ << "[IntensityInfoDQM::analyze] collecting summary histogram "<< summary_histos->histograms[i]._Hist << std::endl;
    hists_to_send[moduleTag_+"_summary"].push_back((TH1*)summary_histos->histograms[i]._Hist->Clone());
    summary_histos->histograms[i]._Hist->Reset();
  }

  histSender_->sendHistograms(hists_to_send);

}


void ots::IntensityInfoDQM::summary_fill(IntensityInfoDQMHistoContainer       *histos, 
					 const mu2e::CaloHitCollection        *CAPHRIHits,
					 const mu2e::IntensityInfoCalo        *CaloInfos, 
					 const mu2e::IntensityInfoTrackerHits *TrkInfos) {
  //  __COUT__ << "filling Summary histograms..."<< std::endl;

  if (histos->histograms.size() == 0) {
    __COUT__ << "No histograms booked. Should they have been created elsewhere?"
	     << std::endl;
  } else {
      
    // Used to get the number of triggered events from each trigger path
    histos->histograms[0]._Hist->Fill(CAPHRIHits->size()); 
    histos->histograms[1]._Hist->Fill(CaloInfos->nCaloHits());
    histos->histograms[2]._Hist->Fill(CaloInfos->caloEnergy());
    histos->histograms[3]._Hist->Fill(TrkInfos->nTrackerHits());
  }
}

void ots::IntensityInfoDQM::endJob() {}

void ots::IntensityInfoDQM::beginRun(const art::Run& run) {}

DEFINE_ART_MODULE(ots::IntensityInfoDQM)
