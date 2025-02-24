// Author: M. MacKenzie
// Sam Grant: made a copy for testing offline
// This module produces histograms for simple integration studies/DQM

#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art_root_io/TFileService.h"
#include "fhiclcpp/types/OptionalAtom.h"

#include <TBufferFile.h>
#include <TH1.h>

#include "artdaq-core-mu2e/Data/CRVDataDecoder.hh"

#include "otsdaq-mu2e-dqm/ArtModules/SimpleDQMHistoContainer.h"
#include "otsdaq-mu2e-dqm/ArtModules/IntegrationDQMHistoContainer.h"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/Macros/ProcessorPluginMacros.h"
#include "otsdaq/MessageFacility/MessageFacility.h"
#include "otsdaq/NetworkUtilities/TCPSendClient.h"
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"
#include "otsdaq-mu2e-dqm/ArtModules/CaloIERCPulseFitter.h"

#include "Offline/RecoDataProducts/inc/CaloDigi.hh"
#include "Offline/RecoDataProducts/inc/StrawDigi.hh"
#include "Offline/RecoDataProducts/inc/CrvDigi.hh"
#include "Offline/RecoDataProducts/inc/STMWaveformDigi.hh"

#include <string>
#include <map>
#include <set>

namespace ots {
  class IntegrationDQMOffline : public art::EDAnalyzer {
  public:
    struct Config {
      using Name = fhicl::Name;
      using Comment = fhicl::Comment;
      fhicl::Atom<int>             port         { Name("port")         , Comment("This parameter sets the port where the histogram will be sent") };
      fhicl::Atom<std::string>     address      { Name("address")      , Comment("This paramter sets the IP address where the histogram will be sent") };
      fhicl::Atom<std::string>     caloDigiTag  { Name("caloDigiTag")  , Comment("Calo digi collection tag name"), "" };
      fhicl::Atom<std::string>     trkDigiTag   { Name("trkDigiTag")   , Comment("Tracker digi collection tag name"), "" };
      fhicl::Atom<std::string>     crvDigiTag   { Name("crvDigiTag")   , Comment("CRV digi collection tag name"), "" };
      fhicl::Atom<std::string>     stmDigiTag   { Name("stmDigiTag")   , Comment("STM digi collection tag name"), "" };
      fhicl::Atom<std::string>     caloFitter   { Name("caloFitter")   , Comment("Calo pulse waveform fit data file name")};
      fhicl::Atom<int>             freqDQM      { Name("freqDQM")      , Comment("Frequency for sending histograms to the data-receiver") };
      fhicl::Atom<int>             freqCaloPulse{ Name("freqCaloPulse"), Comment("Frequency per summary send for sending Calo pulse histograms to the data-receiver"), 10};
      fhicl::Atom<int>             nCaloSiPMs   { Name("nCaloSiPMs")   , Comment("N(Calorimeter SiPM IDs)"), [this](){ return caloDigiTag() != "";}};
      fhicl::Atom<int>             diag         { Name("diagLevel")    , Comment("Diagnostic level"), 0 };
      fhicl::Atom<bool>            runOffline   { Name("runOffline")   , Comment("Run without sending histograms"), false };
    };

    typedef art::EDAnalyzer::Table<Config> Parameters;

    explicit IntegrationDQMOffline(Parameters const& conf);

    void analyze(art::Event const& event) override;
    void beginRun(art::Run const&) override;
    void beginJob() override;
    void endJob() override;

    void retrieve_data      (art::Event const& event);

    void summary_calo_fill  (art::Event const& event, SimpleDQMHistoContainer& hists, const bool fill_pulses);
    void summary_trk_fill   (art::Event const& event, SimpleDQMHistoContainer& hists, const bool fill_pulses);
    void summary_crv_fill   (art::Event const& event, SimpleDQMHistoContainer& hists, const bool fill_pulses);
    void summary_stm_fill   (art::Event const& event, SimpleDQMHistoContainer& hists, const bool fill_pulses);
    void summary_global_fill(art::Event const& event, SimpleDQMHistoContainer& hists);

    std::vector<mu2e::CRVDataDecoder::CRVGlobalRunInfo> decode_crv(const std::vector<mu2e::CRVDataDecoder>* decoders);

    void PlotRate(art::Event const& e);

  private:
    Config                    conf_;
    int                       port_;
    std::string               address_;
    std::string               caloDigiTag_;
    std::string               trkDigiTag_;
    std::string               crvDigiTag_;
    std::string               stmDigiTag_;
    std::vector<std::string>  histType_;
    int                       freqDQM_,  freqCaloPulse_, nCaloSiPMs_, diagLevel_, evtCounter_;
    CaloIERCPulseFitter       calo_pulse_fitter_;
    art::ServiceHandle<art::TFileService> tfs_;
    SimpleDQMHistoContainer   calo_hists_, trk_hists_, crv_hists_, stm_hists_, global_hists_;
    HistoSender*              histSender_;
    size_t                    nCaloSummary_, nTrkSummary_, nCRVSummary_, nSTMSummary_;
    bool                      doOnspillHist_, doOffspillHist_;
    std::string               moduleTag;
    std::map<int,size_t>      sipmIDs_;

    // data
    const mu2e::CaloDigiCollection*          calo_digis_  ;
    const mu2e::StrawDigiCollection*         trk_digis_   ;
    const mu2e::CrvDigiCollection*           crv_digis_   ;
    const mu2e::STMWaveformDigiCollection*   stm_digis_   ;
    const std::vector<mu2e::CRVDataDecoder>* crv_decoders_;
    
  };
} // namespace ots

ots::IntegrationDQMOffline::IntegrationDQMOffline(Parameters const& conf)
  : art::EDAnalyzer(conf), conf_(conf()), port_(conf().port()), address_(conf().address()),
    caloDigiTag_(conf().caloDigiTag()), trkDigiTag_(conf().trkDigiTag()), crvDigiTag_(conf().crvDigiTag()), stmDigiTag_(conf().stmDigiTag()),
    freqDQM_(conf().freqDQM()), freqCaloPulse_(conf().freqCaloPulse()),
    nCaloSiPMs_(conf().nCaloSiPMs()), diagLevel_(conf().diag()), evtCounter_(0),
    calo_pulse_fitter_(conf().caloFitter().c_str()),
    histSender_(conf().runOffline() ? nullptr : new HistoSender(address_, port_)),
    doOnspillHist_(true), doOffspillHist_(true) {
  ;
  
  if (diagLevel_>0){
    __COUT__ << "[IntegrationDQMOffline::IntegrationDQMOffline]"
	     << " caloDigiTag = " << caloDigiTag_.c_str()
	     << " trkDigiTag = " << trkDigiTag_.c_str()
	     << " crvDigiTag = " << crvDigiTag_.c_str()
	     << " stmDigiTag = " << stmDigiTag_.c_str()
	     << std::endl;
  }

}

void ots::IntegrationDQMOffline::beginJob() {
  __COUT__ << "[IntegrationDQMOffline::beginJob] Beginning job" << std::endl;

  // Calorimeter plots
  if(caloDigiTag_ != "") {
    // Book summary calo histograms
    calo_hists_.BookSummaryHistos(tfs_,"calo_digi_count"  , "Calo digi count;N(digis);Counts"                   ,  50,    0.f,   50.f, "IntegrationDQMOffline");
    calo_hists_.BookSummaryHistos(tfs_,"calo_digi_t0"     , "Calo digi t_{0};digi t_{0};Counts"                 , 100,    0.f, 50.e3f, "IntegrationDQMOffline");
    calo_hists_.BookSummaryHistos(tfs_,"calo_digi_dt0"    , "Calo digi #Deltat_{0};digi #Deltat_{0};Counts"     , 101,-252.5f, 252.5f, "IntegrationDQMOffline");
    calo_hists_.BookSummaryHistos(tfs_,"calo_digi_tfit"   , "Calo digi t_{fit};digi t_{fit};Counts"             , 100,    0.f, 50.e3f, "IntegrationDQMOffline");
    calo_hists_.BookSummaryHistos(tfs_,"calo_digi_dtfit"  , "Calo digi #Deltat_{fit};digi #Deltat_{fit};Counts" , 800, -100.f,  100.f, "IntegrationDQMOffline");
    calo_hists_.BookSummaryHistos(tfs_,"calo_digi_tfit_t0", "Calo #Deltat_{fit};digi t_{fit} - t_{0};Counts"    , 500,  -10.f,   10.f, "IntegrationDQMOffline");
    nCaloSummary_ = calo_hists_.histograms.size();

    // Book calo pulse histograms
    for(int ihist = 0; ihist < nCaloSiPMs_; ++ihist) {
      calo_hists_.BookSummaryHistos(tfs_,Form("calo_digi_wave_%i", ihist), "Calo SiPM waveform;bin;Counts", 30, 0.f, 30.f, "IntegrationDQMOffline", "replace");
    }
  }

  //Tracker plots
  if(trkDigiTag_ != "") {
    // Book summary trk histograms
    trk_hists_.BookSummaryHistos(tfs_,"trk_digi_count", "Tracker digi count;N(digis);Counts", 500, 0.f,  500.f, "IntegrationDQMOffline");
    trk_hists_.BookSummaryHistos(tfs_,"trk_digi_t0", "Tracker digi t_{0};digi t_{0};Counts" , 100, 0.f, 50.e3f, "IntegrationDQMOffline");
    nTrkSummary_ = trk_hists_.histograms.size();
  }

  //CRV plots
  if(crvDigiTag_ != "") {

    // Book summary crv histograms
    crv_hists_.BookSummaryHistos(tfs_,"crv_digi_count", "CRV digi count;N(digis);Counts",  30, 0.f,   30.f, "IntegrationDQMOffline");
    crv_hists_.BookSummaryHistos(tfs_,"crv_digi_t0", "CRV digi t_{0};digi t_{0};Counts" , 100, 0.f, 50.e3f, "IntegrationDQMOffline");
    
    // Book CRV timing histograms
    crv_hists_.BookSummaryHistos(tfs_,"crv_ewt_dist", "EWT Distribution;EWT;Entries", 1021, -10.5f, 1010.5f, "IntegrationDQMOffline");
    crv_hists_.BookSummaryHistos(tfs_,"crv_injection_time", "Injection Time Distribution;#Deltat_{0};Entries", 100, 0.f, 1000.f, "IntegrationDQMOffline");
    crv_hists_.BookSummaryHistos(tfs_,"crv_injection_window", "Injection Window Distribution;t_{0};Entries", 100, 0.f, 1000.f, "IntegrationDQMOffline");
    crv_hists_.BookSummaryHistos(tfs_,"crv_marker_count", "Marker Counter;EWT;Marker Counter", 1021, -10.5f, 1010.5f, "IntegrationDQMOffline");
    crv_hists_.BookSummaryHistos(tfs_,"crv_ewt_count", "EWT Counter;EWT;EWT Counter", 1021, -10.5f, 1010.5f, "IntegrationDQMOffline");

    nCRVSummary_ = crv_hists_.histograms.size();
  }

  //STM plots
  if(stmDigiTag_ != "") {
    // Book summary stm histograms
    stm_hists_.BookSummaryHistos(tfs_,"stm_digi_count", "STM digi count;N(digis);Counts"   ,  30, 0.f,  30.f, "IntegrationDQMOffline");
    stm_hists_.BookSummaryHistos(tfs_,"stm_digi_t0"   , "STM digi DetID;digi DetID;Counts" ,   2, 0.f,   2.f, "IntegrationDQMOffline");
    nSTMSummary_ = stm_hists_.histograms.size();

    // Book stm pulse histograms
    stm_hists_.BookSummaryHistos(tfs_, "stm_digi_wave", "STM SiPM waveform;waveform;Counts", 50, 0.f, 50.f, "IntegrationDQMOffline", "replace");
  }

  // Global plots, always initialize to keep the indices aligned
   global_hists_.BookSummaryHistos(tfs_,"trk_calo_deltat0", "Track - Calo t_{0};#Deltat_{0};Counts", 500, -500.f,  500.f, "IntegrationDQMOffline");
   global_hists_.BookSummaryHistos(tfs_,"trk_crv_deltat0" , "Track - CRV t_{0};#Deltat_{0};Counts" , 500, -500.f,  500.f, "IntegrationDQMOffline");
   global_hists_.BookSummaryHistos(tfs_,"trk_stm_deltat0" , "Track - STM t_{0};#Deltat_{0};Counts" , 500, -500.f,  500.f, "IntegrationDQMOffline");
   global_hists_.BookSummaryHistos(tfs_,"calo_crv_deltat0", "Calo - CRV t_{0};#Deltat_{0};Counts"  , 500, -500.f,  500.f, "IntegrationDQMOffline");
   global_hists_.BookSummaryHistos(tfs_,"calo_stm_deltat0", "Calo - STM t_{0};#Deltat_{0};Counts"  , 500, -500.f,  500.f, "IntegrationDQMOffline");
   global_hists_.BookSummaryHistos(tfs_,"crv_stm_deltat0" , "CRV - STM t_{0};#Deltat_{0};Counts"   , 500, -500.f,  500.f, "IntegrationDQMOffline");

   global_hists_.BookSummaryHistos(tfs_,"detector_digis" , "Subdetector events above digi threshold;;N(events)", 5,0,5, "IntegrationDQMOffline");
}

void ots::IntegrationDQMOffline::analyze(art::Event const& event) {
  ++evtCounter_;
  const bool send_summary = evtCounter_ % freqDQM_  == 0;
  const bool fill_pulses  = evtCounter_ % freqCaloPulse_ == 0 && send_summary;

  const bool fillCalo = caloDigiTag_ != "";
  const bool fillTrk  = trkDigiTag_  != "";
  const bool fillCRV  = crvDigiTag_  != "";
  const bool fillSTM  = stmDigiTag_  != "";

  crv_decoders_ = nullptr; // initialize to null at first

  retrieve_data(event); // retrieve event data

  // Fill event summary info FIXME: By-subdetector pulse fill rates
  if(fillCalo) summary_calo_fill(event, calo_hists_, fill_pulses);
  if(fillTrk ) summary_trk_fill (event, trk_hists_ , fill_pulses);
  if(fillCRV ) summary_crv_fill (event, crv_hists_ , fill_pulses);
  if(fillSTM ) summary_stm_fill (event, stm_hists_ , fill_pulses);
  summary_global_fill(event, global_hists_);

  // Decide whether or not to send the current histograms
  if(!send_summary) return;

  if (!conf_.runOffline()) {

    //send a packet AND reset the histograms
    std::map<std::string,std::vector<TH1*>> hists_to_send;

    // Send calorimeter histograms
    if(fillCalo) {
      //send the summary hists
      for (size_t i = 0; i < nCaloSummary_; i++) {
        __COUT__ << "[IntegrationDQMOffline::analyze] collecting summary histogram "<< calo_hists_.histograms[i]._Hist << std::endl;
        hists_to_send["integration_summary/calo"].push_back((TH1*)calo_hists_.histograms[i]._Hist->Clone());
        calo_hists_.histograms[i]._Hist->Reset();
      }

      //send calo waveforms
      if(fill_pulses) {
        for (size_t i = nCaloSummary_; i < calo_hists_.histograms.size(); i++) {
    __COUT__ << "[IntegrationDQMOffline::analyze] collecting summary histogram "<< calo_hists_.histograms[i]._Hist << std::endl;
    hists_to_send["integration_summary/calo:replace"].push_back((TH1*)calo_hists_.histograms[i]._Hist->Clone());
    calo_hists_.histograms[i]._Hist->Reset();
        }
      }
    }

    // Send tracker histograms
    if(fillTrk) {
      //send the summary hists
      for (size_t i = 0; i < nTrkSummary_; i++) {
        __COUT__ << "[IntegrationDQMOffline::analyze] collecting summary histogram "<< trk_hists_.histograms[i]._Hist << std::endl;
        hists_to_send["integration_summary/trk"].push_back((TH1*)trk_hists_.histograms[i]._Hist->Clone());
        trk_hists_.histograms[i]._Hist->Reset();
      }

      //send trk waveforms
      if(fill_pulses) {
        for (size_t i = nTrkSummary_; i < trk_hists_.histograms.size(); i++) {
    __COUT__ << "[IntegrationDQMOffline::analyze] collecting summary histogram "<< trk_hists_.histograms[i]._Hist << std::endl;
    hists_to_send["integration_summary/trk:replace"].push_back((TH1*)trk_hists_.histograms[i]._Hist->Clone());
    trk_hists_.histograms[i]._Hist->Reset();
        }
      }
    }


    // Send CRV histograms
    if(fillCRV) {
      //send the summary hists
      for (size_t i = 0; i < nCRVSummary_; i++) {
        __COUT__ << "[IntegrationDQMOffline::analyze] collecting summary histogram "<< crv_hists_.histograms[i]._Hist << std::endl;
        hists_to_send["integration_summary/crv"].push_back((TH1*)crv_hists_.histograms[i]._Hist->Clone());
        crv_hists_.histograms[i]._Hist->Reset();
      }

      //send CRV waveforms
      if(fill_pulses) {
        for (size_t i = nCRVSummary_; i < crv_hists_.histograms.size(); i++) {
    __COUT__ << "[IntegrationDQMOffline::analyze] collecting summary histogram "<< crv_hists_.histograms[i]._Hist << std::endl;
    hists_to_send["integration_summary/crv:replace"].push_back((TH1*)crv_hists_.histograms[i]._Hist->Clone());
    crv_hists_.histograms[i]._Hist->Reset();
        }
      }
    }

    // Send STM histograms
    if(fillSTM) {
      //send the summary hists
      for (size_t i = 0; i < nSTMSummary_; i++) {
        __COUT__ << "[IntegrationDQMOffline::analyze] collecting summary histogram "<< stm_hists_.histograms[i]._Hist << std::endl;
        hists_to_send["integration_summary/stm"].push_back((TH1*)stm_hists_.histograms[i]._Hist->Clone());
        stm_hists_.histograms[i]._Hist->Reset();
      }

      //send stm waveforms
      if(fill_pulses) {
        if(stm_hists_.histograms[nSTMSummary_]._Hist->Integral() > 0.) { //only refresh if there's a waveform to send
    __COUT__ << "[IntegrationDQMOffline::analyze] collecting summary histogram "<< stm_hists_.histograms[nSTMSummary_]._Hist << std::endl;
    hists_to_send["integration_summary/stm:replace"].push_back((TH1*)stm_hists_.histograms[nSTMSummary_]._Hist->Clone());
    stm_hists_.histograms[nSTMSummary_]._Hist->Reset();
        }
      }
    }

    // send global histograms
    for (size_t i = 0; i < global_hists_.histograms.size(); i++) {
      __COUT__ << "[IntegrationDQMOffline::analyze] collecting summary histogram "<< global_hists_.histograms[i]._Hist << std::endl;
      hists_to_send["integration_summary/global"].push_back((TH1*)global_hists_.histograms[i]._Hist->Clone());
      global_hists_.histograms[i]._Hist->Reset();
    }

    // Send histograms if histSender exists
    if (histSender_) {
      histSender_->sendHistograms(hists_to_send);
    }

  }

}

void ots::IntegrationDQMOffline::retrieve_data(art::Event const& event) {
  // Initialize to null
  calo_digis_   = nullptr;
  trk_digis_    = nullptr;
  crv_digis_    = nullptr;
  crv_decoders_ = nullptr;
  stm_digis_    = nullptr;

  // Get the calorimeter data
  if(caloDigiTag_ != "") {
    art::Handle<mu2e::CaloDigiCollection> digisH;
    if(!event.getByLabel(caloDigiTag_, digisH)) {
      __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No calo digis found with tag " << caloDigiTag_.c_str() << std::endl;
    } else {
      calo_digis_ = digisH.product();
    }
  }

  // Get the tracker data
  if(trkDigiTag_ != "") {
    art::Handle<mu2e::StrawDigiCollection> trkDigisH;
    if(!event.getByLabel(trkDigiTag_, trkDigisH)) {
      __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No Trk digis found with tag " << trkDigiTag_.c_str() << std::endl;
    } else {
      trk_digis_  = trkDigisH.product();
    }
  }

  // Get the CRV data
  const bool gr4_processing(true);
  if(crvDigiTag_ != "") {
    if(gr4_processing) {
      art::Handle<std::vector<mu2e::CRVDataDecoder>> crvDecoderH;
      if(!event.getByLabel(crvDigiTag_, crvDecoderH)) {
	__COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No CRV decoders found with tag " << crvDigiTag_.c_str() << std::endl;
      } else {
	crv_decoders_ = crvDecoderH.product();
      }
    } else {
      art::Handle<mu2e::CrvDigiCollection> crvDigisH;
      if(!event.getByLabel(crvDigiTag_, crvDigisH)) {
	__COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No CRV digis found with tag " << crvDigiTag_.c_str() << std::endl;
      } else {
	crv_digis_ = crvDigisH.product();
      }
    }
  }

  // Get the STM data
  if(stmDigiTag_ != "") {
    art::Handle<mu2e::STMWaveformDigiCollection> digisH;
    if(!event.getByLabel(stmDigiTag_, digisH)) {
      __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No stm digis found with tag " << stmDigiTag_.c_str() << std::endl;
    } else {
      stm_digis_ = digisH.product();
    }
  }
}

void ots::IntegrationDQMOffline::summary_calo_fill(art::Event const& event, SimpleDQMHistoContainer& hists, const bool fill_pulses) {
  if (hists.histograms.size() < nCaloSummary_) {
    __COUT__ << "Not enough histograms booked" << std::endl;
    return;
  }
  auto digis = calo_digis_;
  if(!digis) {
    __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No calo digis found with tag " << caloDigiTag_.c_str() << std::endl;
    hists.histograms[0]._Hist->Fill(0);
    return;
  } else if(diagLevel_ > 0) {
    __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] Calo digi collection found\n";
  }

  hists.histograms[0]._Hist->Fill(digis->size());

  std::map<int, float> sipm_t0s, sipm_times;
  for(auto& digi : *digis) {
    const float t0 = digi.t0()*5.f;
    const auto time_fit_res = calo_pulse_fitter_.fit_pulse(&digi);
    const float time = time_fit_res._time;
    const int sipmID = digi.SiPMID();
    hists.histograms[1]._Hist->Fill(t0);
    hists.histograms[3]._Hist->Fill(time);
    hists.histograms[5]._Hist->Fill(time - t0);
    // store the earliest t0 for each SiPM
    sipm_t0s  [sipmID] = (sipm_t0s  .count(sipmID)) ? std::min(sipm_t0s  [sipmID], t0  ) : t0  ;
    sipm_times[sipmID] = (sipm_times.count(sipmID)) ? std::min(sipm_times[sipmID], time) : time;

    // fill pulses if requested
    if(fill_pulses) {
      if(!sipmIDs_.count(sipmID)) sipmIDs_[sipmID] = sipmIDs_.size() + nCaloSummary_;
      const size_t index = sipmIDs_[sipmID];
      if(index >= hists.histograms.size()) continue;
      TH1* h = hists.histograms[index]._Hist;
      h->SetTitle(Form("Calo SiPM %i waveform;waveform;Counts", sipmID));
      for(size_t bin = 0; bin < digi.waveform().size(); ++bin) {
	if(int(bin+1) > h->GetNbinsX()) break;
	h->SetBinContent(bin+1, digi.waveform()[bin]);
      } //end waveform loop
    }
  } //end digi loop

  //fill time comparisons
  std::set<int> checked;
  for(auto entry : sipm_t0s) {
    const int sipm_1(entry.first);
    const float t0_1(entry.second), time_1(sipm_times[sipm_1]);
    if(checked.count(sipm_1)) continue;
    checked.insert(sipm_1);
    //ID = 120*DTC + 20*ROC + channel
    const int roc_1 = sipm_1 / 20 % 6;
    const int dtc = sipm_1 / 120;
    // Find another ROC on the same board
    for(int iroc = 0; iroc < 6; ++iroc) {
      if(iroc == roc_1) continue;
      // Find the first digi in this roc
      for(int ichannel = 0; ichannel < 20; ++ichannel) {
	const int sipm_2 = dtc*120 + iroc*20 + ichannel;
	if(checked.count(sipm_2)) continue;
	if(sipm_t0s.count(sipm_2)) {
	  checked.insert(sipm_2);
	  const float t0_2   = sipm_t0s  [sipm_2];
	  const float time_2 = sipm_times[sipm_2];
	  const float delta_t0   = (sipm_1 < sipm_2) ? t0_2   - t0_1   : t0_1   - t0_2;
	  const float delta_time = (sipm_1 < sipm_2) ? time_2 - time_1 : time_1 - time_2;
	  const float offset = 0.f; //dtc*20.f - 60.f; //FIXME: Offsetting to show more on one plot
	  hists.histograms[2]._Hist->Fill(delta_t0   + offset);
	  hists.histograms[4]._Hist->Fill(delta_time + offset);
	}
      }
    }
  }
}

void ots::IntegrationDQMOffline::summary_trk_fill(art::Event const& event, SimpleDQMHistoContainer& hists, const bool fill_pulses) {
  if (hists.histograms.size() == 0) {
    __COUT__ << __func__ << ": No histograms booked" << std::endl;
    return;
  }
  if (hists.histograms.size() < nTrkSummary_) {
    __COUT__ << __func__ << ": Not enough histograms booked" << std::endl;
    return;
  }

  const auto digis  = trk_digis_;
  if(!digis) {
    __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No Trk digis found with tag " << trkDigiTag_.c_str() << std::endl;
    hists.histograms[0]._Hist->Fill(0);
    return;
  }

  hists.histograms[0]._Hist->Fill(digis->size());
  for(auto& digi : *digis) {
    hists.histograms[1]._Hist->Fill(digi.TDC()[0]*1.f); //FIXME: Convert to ns
  }

}

void ots::IntegrationDQMOffline::summary_crv_fill(art::Event const& event, SimpleDQMHistoContainer& hists, const bool fill_pulses) {

  // Input validation
  if (hists.histograms.size() == 0) {
    __COUT__ << __func__ << ": No histograms booked" << std::endl;
    return;
  }
  if (hists.histograms.size() < nCRVSummary_) {
    __COUT__ << __func__ << ": Not enough histograms booked" << std::endl;
    return;
  }

  // Check for valid CRV decoders
  if(!crv_decoders_) {
    __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No CRV decoders found with tag " << crvDigiTag_.c_str() << std::endl;
    hists.histograms[0]._Hist->Fill(0);
    return;
  }

  // Get number of subevents and fill counter
  const size_t nSubEvents = crv_decoders_->size();
  hists.histograms[0]._Hist->Fill(nSubEvents);

  // Decode CRV data
  auto gr_infos = decode_crv(crv_decoders_);

  // https://github.com/Mu2e/artdaq_core_mu2e/blob/develop/artdaq-core-mu2e/Data/CRVDataDecoder.hh#L144
  //   CRVGlobalRunInfo()
  //   : word0(0)
  //   , EWTCount(0)
  //   , markerCount(0)
  //   , lastEWT(0)
  //   , lock(0)
  //   , unused(0)
  //   , PLL(0)
  //   , CRC(0)
  //   , injectionWindow(0)
  //   , injectionTime(0)
  //   , word7(0)
  // {}

  // Process each global run info object
  for(const auto& gr_info : gr_infos) {    
    // Fill basic timing histogram 
    hists.histograms[1]._Hist->Fill(gr_info.injectionTime * 12.5f); // ns
    // Fill EWT distribution
    hists.histograms[2]._Hist->Fill(gr_info.EWTCount);
    // Fill injection time and window histograms
    hists.histograms[3]._Hist->Fill(injection_time_ns);
    hists.histograms[4]._Hist->Fill(gr_info.injectionWindow);
    // Marker count vs EWT
    hists.histograms[5]._Hist->Fill(gr_info.EWTCount, gr_info.markerCount);
    // EWT counter vs EWT for self-consistency check
    hists.histograms[6]._Hist->Fill(gr_info.EWTCount, gr_info.EWTCount);

  }
  
}

// void ots::IntegrationDQMOffline::summary_crv_fill(art::Event const& event, SimpleDQMHistoContainer& hists, const bool fill_pulses) {
//   if (hists.histograms.size() == 0) {
//     __COUT__ << __func__ << ": No histograms booked" << std::endl;
//     return;
//   }
//   if (hists.histograms.size() < nCRVSummary_) {
//     __COUT__ << __func__ << ": Not enough histograms booked" << std::endl;
//     return;
//   }

//   if(!crv_decoders_) {
//     __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No CRV decoders found with tag " << crvDigiTag_.c_str() << std::endl;
//     hists.histograms[0]._Hist->Fill(0);
//     return;
//   }


  // Based on Simon's reference plots

//   const size_t nSubEvents = crv_decoders_->size();
//   hists.histograms[0]._Hist->Fill(nSubEvents);
//   auto gr_infos = decode_crv(crv_decoders_);
//   for(const auto& gr_info : gr_infos) {
//     hists.histograms[1]._Hist->Fill(gr_info.injectionTime*12.5f);
//   }

  
//   if(false) {
//     art::Handle<mu2e::CrvDigiCollection> crvDigisH;
//     if(!event.getByLabel(crvDigiTag_, crvDigisH)) {
//       __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No CRV digis found with tag " << crvDigiTag_.c_str() << std::endl;
//       hists.histograms[0]._Hist->Fill(0);
//       return;
//     }
//     const auto digis  = crv_digis_;
//     if(!digis) {
//       __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No CRV digis found with tag " << crvDigiTag_.c_str() << std::endl;
//       hists.histograms[0]._Hist->Fill(0);
//       return;
//     }

//     hists.histograms[0]._Hist->Fill(digis->size());
//     for(auto& digi : *digis) {
//       hists.histograms[1]._Hist->Fill(digi.GetStartTDC()*1.f); //FIXME: Convert to ns
//     }
//   }
// }

//STM
void ots::IntegrationDQMOffline::summary_stm_fill(art::Event const& event, SimpleDQMHistoContainer& hists, const bool fill_pulses) {
  if (hists.histograms.size() == 0) {
    __COUT__ << __func__ << ": No histograms booked" << std::endl;
    return;
  }
  if (hists.histograms.size() < nSTMSummary_) {
    __COUT__ << __func__ << ": Not enough histograms booked" << std::endl;
    return;
  }
  auto digis = stm_digis_;
  if(!digis) {
    __COUT__ << "[IntegrationDQMOffline::" << __func__ << "] No stm digis found with tag " << stmDigiTag_.c_str() << std::endl;
    hists.histograms[0]._Hist->Fill(0);
    return;
  }
  hists.histograms[0]._Hist->Fill(digis->size());
  for(auto& digi : *digis) {
    hists.histograms[1]._Hist->Fill(digi.DetID());
  }

  // fill pulses if requested
  if(fill_pulses) {
    for(auto& digi : *digis) {
      TH1* h = hists.histograms[2]._Hist;
      h->SetTitle("STM waveform;waveform;Counts");
      for(size_t bin = 0; bin < digi.adcs().size(); ++bin) {
	if(int(bin+1) > h->GetNbinsX()) break;
	h->SetBinContent(bin+1, digi.adcs()[bin]);
      }
    }
  }
}

void ots::IntegrationDQMOffline::summary_global_fill(art::Event const& event, SimpleDQMHistoContainer& hists) {
  if (hists.histograms.size() == 0) {
    __COUT__ << __func__ << ": No histograms booked" << std::endl;
    return;
  }

  bool fillCalo  = calo_digis_   != nullptr;
  bool fillTrk   = trk_digis_    != nullptr;
  bool fillCRV   = crv_digis_    != nullptr;
  bool fillSTM   = stm_digis_    != nullptr;
  bool fillCRVGR = crv_decoders_ != nullptr; //for using the CRV GR decoders

  // Retrieve the input data

  const auto caloDigis    = calo_digis_;
  const auto trkDigis     = trk_digis_;
  const auto crvDigis     = crv_digis_;
  const auto stmDigis     = stm_digis_;
  const auto crv_gr_infos = decode_crv(crv_decoders_);

  // Fill the histograms

  const size_t digi_count_index = 6; //FIXME: Make accessor
  if(digi_count_index < hists.histograms.size()) {
      TH1* hdigi_count = hists.histograms[digi_count_index]._Hist;
      if(fillCalo  && caloDigis  ->size() > 0) hdigi_count->Fill("Calo", 1.);
      if(fillTrk   && trkDigis   ->size() > 0) hdigi_count->Fill("Trk" , 1.);
      if(fillCRV   && crvDigis   ->size() > 0) hdigi_count->Fill("CRV" , 1.);
      if(fillCRVGR && crv_gr_infos.size() > 0) hdigi_count->Fill("CRV" , 1.);
      if(fillSTM   && stmDigis   ->size() > 0) hdigi_count->Fill("STM" , 1.);
  } else {
    __COUT__ <<  "[IntegrationDQMOffline::" << __func__ << "] Histogram list not large enough! Index = " << digi_count_index
	     << " size = " << hists.histograms.size() << std::endl;
  }

  float trk_t0  = 1.e10f;
  float calo_t0 = 1.e10f;
  float crv_t0  = 1.e10f;
  float stm_t0  = 1.e10f;

  if(fillCalo) {
    for(auto& digi : *caloDigis) {
      calo_t0 = std::min(calo_t0, digi.t0()*5.f); //t0 is in 5 ns clock ticks
    }
  }
  if(fillTrk) {
    for(auto& digi : *trkDigis) {
      trk_t0 = std::min(trk_t0, digi.TDC()[0]*1.f); //FIXME: Convert to ns (?)
    }
  }
  if(fillCRV) {
    for(auto& digi : *crvDigis) {
      crv_t0 = std::min(crv_t0, digi.GetStartTDC()*1.f); //FIXME: Convert to ns
    }
  }
  if(fillCRVGR) {
    crv_t0 = 1.e10f;
    for(auto& gr_info : crv_gr_infos) {
      crv_t0 = std::min(crv_t0, gr_info.injectionTime*12.5f);
    }
    fillCRV = fillCRVGR; //reuse the CRV digi filling flag
  }
  if(fillSTM) {
    for(auto& digi : *stmDigis) {
      stm_t0 = std::min(stm_t0, digi.trigTimeOffset()*1.f); //FIXME: Convert to ns
    }
  }

  // Fill the subdetector time difference
  if(fillTrk  && fillCalo) hists.histograms[0]._Hist->Fill(trk_t0  - calo_t0);
  if(fillTrk  && fillCRV ) hists.histograms[1]._Hist->Fill(trk_t0  - crv_t0 );
  if(fillTrk  && fillSTM ) hists.histograms[2]._Hist->Fill(trk_t0  - stm_t0 );
  if(fillCalo && fillCRV ) hists.histograms[3]._Hist->Fill(calo_t0 - crv_t0 );
  if(fillCalo && fillSTM ) hists.histograms[4]._Hist->Fill(calo_t0 - stm_t0 );
  if(fillCRV  && fillSTM ) hists.histograms[5]._Hist->Fill(crv_t0  - stm_t0 );

}

// Decode CRV GR data from a vector of decoders
// Returns a vector of CRVGlobalRunInfo objects
std::vector<mu2e::CRVDataDecoder::CRVGlobalRunInfo> ots::IntegrationDQMOffline::decode_crv(const std::vector<mu2e::CRVDataDecoder>* decoders) {
  // Return empty vector if input pointer is null
  if(!decoders) return {};

  // Get number of subevents
  const auto nSubEvents = decoders->size();
  // Container for decoded global run information
  std::vector<mu2e::CRVDataDecoder::CRVGlobalRunInfo> globalRunInfos;

  // Iterate through sub-events in the decoders
  for(size_t iSubEvent = 0; iSubEvent < nSubEvents; ++iSubEvent) {
    // Get reference to current decoder
    const mu2e::CRVDataDecoder& CRVDataDecoder((*decoders)[iSubEvent]);
    // Initialise event processing
    CRVDataDecoder.setup_event();

    // Process each data block within the current decoder
    for(size_t iDataBlock = 0; iDataBlock < CRVDataDecoder.block_count(); ++iDataBlock) {
      // Get pointer to current data block
      auto block = CRVDataDecoder.dataAtBlockIndex(iDataBlock);
      if(block == nullptr) continue;  // Skip if block
      
      // Get block header and validate it
      auto header = block->GetHeader();
      if(!header->isValid()) continue;  // Skip if header is invalid
      
      // Verify this is CRV data
      if(header->GetSubsystemID() != DTCLib::DTC_Subsystem::DTC_Subsystem_CRV) continue;

      // Process blocks that contain packets
      if(header->GetPacketCount() > 0) {
          // Get ROC status packet
          std::unique_ptr<mu2e::CRVDataDecoder::CRVROCStatusPacket> crvRocHeader = CRVDataDecoder.GetCRVROCStatusPacket(iDataBlock);
          if(crvRocHeader==nullptr) continue;  // Skip if ROC header is invalid
          
        // Create new global run info object and add to vector
        globalRunInfos.push_back(mu2e::CRVDataDecoder::CRVGlobalRunInfo());
        auto& globalRunInfo = globalRunInfos.back();
        // Decode global run information from the data block
        CRVDataDecoder.GetCRVGlobalRunInfo(iDataBlock, globalRunInfo);
        

        // mu2e::CRVDataDecoder::CRVGlobalRunPayload globalRunPayload;
        // CRVDataDecoder.GetCRVGlobalRunPayload(iDataBlock, globalRunPayload);
      }     
    }      
  }        
  
  return globalRunInfos;  // Return CRVGlobalRunInfo objects
}

void ots::IntegrationDQMOffline::endJob() {}

void ots::IntegrationDQMOffline::beginRun(const art::Run& run) {}

DEFINE_ART_MODULE(ots::IntegrationDQMOffline)
