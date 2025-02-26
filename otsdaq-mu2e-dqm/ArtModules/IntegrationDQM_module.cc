// Author: M. MacKenzie
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

#include "otsdaq-mu2e-dqm/ArtModules/CaloIERCPulseFitter.h"
#include "otsdaq-mu2e-dqm/ArtModules/IntegrationDQMHistoContainer.h"
#include "otsdaq-mu2e-dqm/ArtModules/SimpleDQMHistoContainer.h"
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/Macros/ProcessorPluginMacros.h"
#include "otsdaq/MessageFacility/MessageFacility.h"
#include "otsdaq/NetworkUtilities/TCPSendClient.h"

#include "Offline/RecoDataProducts/inc/CaloDigi.hh"
#include "Offline/RecoDataProducts/inc/CrvDigi.hh"
#include "Offline/RecoDataProducts/inc/STMWaveformDigi.hh"
#include "Offline/RecoDataProducts/inc/StrawDigi.hh"

#include <map>
#include <set>
#include <string>

namespace ots {
class IntegrationDQM : public art::EDAnalyzer {
public:
  struct Config {
    using Name = fhicl::Name;
    using Comment = fhicl::Comment;
    fhicl::Atom<int> port{
        Name("port"),
        Comment(
            "This parameter sets the port where the histogram will be sent")};
    fhicl::Atom<std::string> address{
        Name("address"), Comment("This paramter sets the IP address where the "
                                 "histogram will be sent")};
    fhicl::Atom<bool> sendHists{Name("sendHists"), Comment("Send histograms"),
                                true};
    fhicl::Atom<std::string> caloDigiTag{
        Name("caloDigiTag"), Comment("Calo digi collection tag name"), ""};
    fhicl::Atom<std::string> trkDigiTag{
        Name("trkDigiTag"), Comment("Tracker digi collection tag name"), ""};
    fhicl::Atom<std::string> crvDigiTag{
        Name("crvDigiTag"), Comment("CRV digi collection tag name"), ""};
    fhicl::Atom<std::string> stmDigiTag{
        Name("stmDigiTag"), Comment("STM digi collection tag name"), ""};
    fhicl::Atom<std::string> caloFitter{
        Name("caloFitter"), Comment("Calo pulse waveform fit data file name")};
    fhicl::Atom<int> freqDQM{
        Name("freqDQM"),
        Comment("Frequency for sending histograms to the data-receiver")};
    fhicl::Atom<int> freqCaloPulse{
        Name("freqCaloPulse"),
        Comment("Frequency per summary send for sending Calo pulse histograms "
                "to the data-receiver"),
        10};
    fhicl::Atom<int> nCaloSiPMs{Name("nCaloSiPMs"),
                                Comment("N(Calorimeter SiPM IDs)"),
                                [this]() { return caloDigiTag() != ""; }};
    fhicl::Atom<int> diag{Name("diagLevel"), Comment("Diagnostic level"), 0};
  };

  typedef art::EDAnalyzer::Table<Config> Parameters;

  explicit IntegrationDQM(Parameters const &conf);

  void analyze(art::Event const &event) override;
  void beginRun(art::Run const &) override;
  void beginJob() override;
  void endJob() override;

  void retrieve_data(art::Event const &event);

  void summary_calo_fill(art::Event const &event, const bool fill_pulses);
  void summary_trk_fill(art::Event const &event, const bool fill_pulses);
  void summary_crv_fill(art::Event const &event, const bool fill_pulses);
  void summary_stm_fill(art::Event const &event, const bool fill_pulses);
  void summary_global_fill(art::Event const &event);

  // void FormatHists(SimpleDQMHistoContainer& hists);

  std::vector<mu2e::CRVDataDecoder::CRVGlobalRunInfo>
  decode_crv(const std::vector<mu2e::CRVDataDecoder> *decoders);

  void PlotRate(art::Event const &e);

private:
  Config conf_;
  int port_;
  std::string address_;
  const bool sendHists_;
  std::string caloDigiTag_;
  std::string trkDigiTag_;
  std::string crvDigiTag_;
  std::string stmDigiTag_;
  std::vector<std::string> histType_;
  int freqDQM_, freqCaloPulse_, nCaloSiPMs_, diagLevel_, evtCounter_;
  CaloIERCPulseFitter calo_pulse_fitter_;
  art::ServiceHandle<art::TFileService> tfs_;
  IntegrationDQMHistoContainer hists_;
  HistoSender *histSender_;
  size_t nCaloSummary_, nTrkSummary_, nCRVSummary_, nSTMSummary_;
  bool doOnspillHist_, doOffspillHist_;
  std::string moduleTag;
  std::map<int, size_t> sipmIDs_;

  // conversion factors
  const float calo_tick_time_;
  const float trk_tick_time_;
  const float crv_tick_time_;
  const float stm_tick_time_;

  // data
  const mu2e::CaloDigiCollection *calo_digis_;
  const mu2e::StrawDigiCollection *trk_digis_;
  const mu2e::CrvDigiCollection *crv_digis_;
  const mu2e::STMWaveformDigiCollection *stm_digis_;
  const std::vector<mu2e::CRVDataDecoder> *crv_decoders_;
};
} // namespace ots

ots::IntegrationDQM::IntegrationDQM(Parameters const &conf)
    : art::EDAnalyzer(conf), conf_(conf()), port_(conf().port()),
      address_(conf().address()), sendHists_(conf().sendHists()),
      caloDigiTag_(conf().caloDigiTag()), trkDigiTag_(conf().trkDigiTag()),
      crvDigiTag_(conf().crvDigiTag()), stmDigiTag_(conf().stmDigiTag()),
      freqDQM_(conf().freqDQM()), freqCaloPulse_(conf().freqCaloPulse()),
      nCaloSiPMs_(conf().nCaloSiPMs()), diagLevel_(conf().diag()),
      evtCounter_(0),
      calo_pulse_fitter_(conf().caloFitter().c_str(), diagLevel_),
      histSender_((sendHists_) ? new HistoSender(address_, port_) : nullptr),
      doOnspillHist_(true), doOffspillHist_(true), calo_tick_time_(5.f),
      trk_tick_time_(1.f /*FIXME*/), crv_tick_time_(12.5f),
      stm_tick_time_(1.f /*FIXME*/) {

  if (diagLevel_ > 0) {
    __COUT__ << "[IntegrationDQM::IntegrationDQM]"
             << " caloDigiTag = " << caloDigiTag_.c_str()
             << " trkDigiTag = " << trkDigiTag_.c_str()
             << " crvDigiTag = " << crvDigiTag_.c_str()
             << " stmDigiTag = " << stmDigiTag_.c_str() << std::endl;
  }
}

// void ots::IntegrationDQM::FormatHists(SimpleDQMHistoContainer& hists) { //MM:
// Moved to IntegrationHistoContainer::BookHist

void ots::IntegrationDQM::beginJob() {
  __COUT__ << "[IntegrationDQM::beginJob] Beginning job" << std::endl;

  // Calorimeter plots
  if (caloDigiTag_ != "")
    hists_._calo_hists.init(tfs_, "IntegrationDQM", nCaloSiPMs_);

  // Tracker plots
  if (trkDigiTag_ != "")
    hists_._trk_hists.init(tfs_, "IntegrationDQM", 0);

  // CRV plots
  if (crvDigiTag_ != "")
    hists_._crv_hists.init(tfs_, "IntegrationDQM", 0);

  // STM plots
  if (stmDigiTag_ != "")
    hists_._stm_hists.init(tfs_, "IntegrationDQM");

  hists_._global_hists.init(tfs_, "IntegrationDQM");
}

void ots::IntegrationDQM::analyze(art::Event const &event) {
  ++evtCounter_;
  const bool send_summary = evtCounter_ % freqDQM_ == 0;
  const bool fill_pulses = evtCounter_ % freqCaloPulse_ == 0 && send_summary;

  const bool fillCalo = caloDigiTag_ != "";
  const bool fillTrk = trkDigiTag_ != "";
  const bool fillCRV = crvDigiTag_ != "";
  const bool fillSTM = stmDigiTag_ != "";

  retrieve_data(event); // retrieve event data

  // Fill event summary info FIXME: By-subdetector pulse fill rates
  if (fillCalo)
    summary_calo_fill(event, fill_pulses);
  if (fillTrk)
    summary_trk_fill(event, fill_pulses);
  if (fillCRV)
    summary_crv_fill(event, fill_pulses);
  if (fillSTM)
    summary_stm_fill(event, fill_pulses);
  summary_global_fill(event);

  // Decide whether or not to send the current histograms
  if (!send_summary || !sendHists_)
    return;

  // send a packet, replace histogram on other end so no need to reset the
  // histograms
  std::map<std::string, std::vector<TH1 *>> hists_to_send;

  // Send calorimeter histograms
  if (fillCalo)
    hists_to_send["integration_summary/calo:replace"] =
        hists_._calo_hists.hists_to_send();

  // Send tracker histograms
  if (fillTrk)
    hists_to_send["integration_summary/trk:replace"] =
        hists_._trk_hists.hists_to_send();

  // Send CRV histograms
  if (fillCRV)
    hists_to_send["integration_summary/crv:replace"] =
        hists_._crv_hists.hists_to_send();

  // Send STM histograms
  if (fillSTM)
    hists_to_send["integration_summary/stm:replace"] =
        hists_._stm_hists.hists_to_send();

  // send global histograms
  hists_to_send["integration_summary/global:replace"] =
      hists_._global_hists.hists_to_send();

  // append run number to all histograms
  for (auto &entry : hists_to_send) {
    for (auto hist : entry.second)
      hist->SetTitle(Form("%s: Run %i", hist->GetTitle(), event.run()));
  }

  histSender_->sendHistograms(hists_to_send);
}

void ots::IntegrationDQM::retrieve_data(art::Event const &event) {
  // Initialize to null
  calo_digis_ = nullptr;
  trk_digis_ = nullptr;
  crv_digis_ = nullptr;
  crv_decoders_ = nullptr;
  stm_digis_ = nullptr;

  // Get the calorimeter data
  if (caloDigiTag_ != "") {
    art::Handle<mu2e::CaloDigiCollection> digisH;
    if (!event.getByLabel(caloDigiTag_, digisH)) {
      __COUT__ << "[IntegrationDQM::" << __func__
               << "] No calo digis found with tag " << caloDigiTag_.c_str()
               << std::endl;
    } else {
      calo_digis_ = digisH.product();
    }
  }

  // Get the tracker data
  if (trkDigiTag_ != "") {
    art::Handle<mu2e::StrawDigiCollection> trkDigisH;
    if (!event.getByLabel(trkDigiTag_, trkDigisH)) {
      __COUT__ << "[IntegrationDQM::" << __func__
               << "] No Trk digis found with tag " << trkDigiTag_.c_str()
               << std::endl;
    } else {
      trk_digis_ = trkDigisH.product();
    }
  }

  // Get the CRV data
  const bool gr4_processing(true);
  if (crvDigiTag_ != "") {
    if (gr4_processing) {
      art::Handle<std::vector<mu2e::CRVDataDecoder>> crvDecoderH;
      if (!event.getByLabel(crvDigiTag_, crvDecoderH)) {
        __COUT__ << "[IntegrationDQM::" << __func__
                 << "] No CRV decoders found with tag " << crvDigiTag_.c_str()
                 << std::endl;
      } else {
        crv_decoders_ = crvDecoderH.product();
      }
    } else {
      art::Handle<mu2e::CrvDigiCollection> crvDigisH;
      if (!event.getByLabel(crvDigiTag_, crvDigisH)) {
        __COUT__ << "[IntegrationDQM::" << __func__
                 << "] No CRV digis found with tag " << crvDigiTag_.c_str()
                 << std::endl;
      } else {
        crv_digis_ = crvDigisH.product();
      }
    }
  }

  // Get the STM data
  if (stmDigiTag_ != "") {
    art::Handle<mu2e::STMWaveformDigiCollection> digisH;
    if (!event.getByLabel(stmDigiTag_, digisH)) {
      __COUT__ << "[IntegrationDQM::" << __func__
               << "] No stm digis found with tag " << stmDigiTag_.c_str()
               << std::endl;
    } else {
      stm_digis_ = digisH.product();
    }
  }
}

void ots::IntegrationDQM::summary_calo_fill(art::Event const &event,
                                            const bool fill_pulses) {
  auto &hists = hists_._calo_hists;
  auto digis = calo_digis_;
  if (!digis) {
    __COUT__ << "[IntegrationDQM::" << __func__
             << "] No calo digis found with tag " << caloDigiTag_.c_str()
             << std::endl;
    hists.digi_count._Hist->Fill(0);
    return;
  } else if (diagLevel_ > 0) {
    __COUT__ << "[IntegrationDQM::" << __func__
             << "] Calo digi collection found\n";
  }

  hists.digi_count._Hist->Fill(digis->size());

  std::map<int, float> sipm_t0s, sipm_times;
  if (diagLevel_ > 0)
    std::cout << "[IntegrationDQM::" << __func__
              << "] Calo data: " << digis->size() << " digis\n";
  for (auto &digi : *digis) {
    const float t0 = digi.t0() * calo_tick_time_;
    const float peak = digi.peakpos() * calo_tick_time_;
    const auto time_fit_res = calo_pulse_fitter_.fit_pulse(&digi);
    const float time = time_fit_res._time;
    const int sipmID = digi.SiPMID();
    const bool new_sipm = !sipmIDs_.count(sipmID);
    const int roc = sipmID / 20 % 6;
    const int dtc = sipmID / 120;
    const int channel = sipmID % 20;
    if (diagLevel_ > 0)
      std::cout << "--> Calo digi: "
                << " t0 = " << t0 << " t(fit) = " << time
                << " (diff = " << time - t0 << ") peak = " << peak
                << " DTC = " << dtc << " ROC = " << roc
                << " Channel = " << channel << std::endl;
    hists.digi_t0._Hist->Fill(t0);
    hists.digi_tfit._Hist->Fill(time);
    hists.digi_tfit_t0._Hist->Fill(time - t0);
    hists.digi_fit_status._Hist->Fill(time_fit_res._status);
    hists.digi_peak._Hist->Fill(peak);
    hists.sipms._Hist->Fill(Form("D%02iR%iC%02i", dtc, roc, channel),
                            1.); // Fill the SiPM ID as the x-axis labels
    if (new_sipm) { // FIXME: Sorting the labels here doesn't appear to work,
                    // perhaps needs to be done when drawing
      try {
        hists.sipms._Hist->GetXaxis()->LabelsOption(
            "a"); // re-sort the bin labels if a new one is added
      } catch (...) {
      }
    }
    // store the earliest t0 for each SiPM
    sipm_t0s[sipmID] =
        (sipm_t0s.count(sipmID)) ? std::min(sipm_t0s[sipmID], t0) : t0;
    sipm_times[sipmID] =
        (sipm_times.count(sipmID)) ? std::min(sipm_times[sipmID], time) : time;

    // fill pulses if requested
    if (fill_pulses) {
      if (new_sipm)
        sipmIDs_[sipmID] = sipmIDs_.size();
      const size_t index = sipmIDs_[sipmID];
      if (index >= hists.pulses.size())
        continue;
      TH1 *h = hists.pulses[index]._Hist;
      h->Reset();
      h->SetTitle(Form("Calo SiPM %i waveform;waveform;Counts", sipmID));
      for (size_t bin = 0; bin < digi.waveform().size(); ++bin) {
        if (int(bin + 1) > h->GetNbinsX())
          break;
        h->SetBinContent(bin + 1, digi.waveform()[bin]);
      } // end waveform loop
    }
  } // end digi loop

  // fill time comparisons
  std::set<int> checked;
  for (auto entry : sipm_t0s) {
    const int sipm_1(entry.first);
    const float t0_1(entry.second), time_1(sipm_times[sipm_1]);
    if (checked.count(sipm_1))
      continue;
    checked.insert(sipm_1);
    // ID = 120*DTC + 20*ROC + channel
    const int roc_1 = sipm_1 / 20 % 6;
    const int dtc = sipm_1 / 120;
    // Find another ROC on the same board
    for (int iroc = 0; iroc < 6; ++iroc) {
      if (iroc == roc_1)
        continue;
      // Find the first digi in this roc
      for (int ichannel = 0; ichannel < 20; ++ichannel) {
        const int sipm_2 = dtc * 120 + iroc * 20 + ichannel;
        if (checked.count(sipm_2))
          continue;
        if (sipm_t0s.count(sipm_2)) {
          checked.insert(sipm_2);
          const float t0_2 = sipm_t0s[sipm_2];
          const float time_2 = sipm_times[sipm_2];
          const float delta_t0 = (sipm_1 < sipm_2) ? t0_2 - t0_1 : t0_1 - t0_2;
          const float delta_time =
              (sipm_1 < sipm_2) ? time_2 - time_1 : time_1 - time_2;
          const float offset = 0.f; // dtc*20.f - 60.f; //FIXME: Offsetting to
                                    // show more on one plot
          hists.digi_dt0._Hist->Fill(delta_t0 + offset);
          hists.digi_dtfit._Hist->Fill(delta_time + offset);
        }
      }
    }
  }
}

void ots::IntegrationDQM::summary_trk_fill(art::Event const &event,
                                           const bool fill_pulses) {
  auto &hists = hists_._trk_hists;
  const auto digis = trk_digis_;
  if (!digis) {
    __COUT__ << "[IntegrationDQM::" << __func__
             << "] No Trk digis found with tag " << trkDigiTag_.c_str()
             << std::endl;
    hists.digi_count._Hist->Fill(0);
    return;
  }

  hists.digi_count._Hist->Fill(digis->size());
  for (auto &digi : *digis) {
    hists.digi_t0._Hist->Fill(digi.TDC()[0] *
                              trk_tick_time_); // FIXME: Convert to ns
  }
}

void ots::IntegrationDQM::summary_crv_fill(art::Event const &event,
                                           const bool fill_pulses) {
  auto &hists = hists_._crv_hists;

  const bool using_decoders = crv_decoders_ != nullptr || crv_digis_ == nullptr;

  if (using_decoders) {
    if (!crv_decoders_) {
      __COUT__ << "[IntegrationDQM::" << __func__
               << "] No CRV decoders found with tag " << crvDigiTag_.c_str()
               << std::endl;
      hists.digi_count._Hist->Fill(0);
      return;
    }

    // Get number of subevents and fill counter
    const size_t nSubEvents = crv_decoders_->size();
    hists.digi_count._Hist->Fill(nSubEvents);

    // Decode CRV data
    auto gr_infos = decode_crv(crv_decoders_);

    // sgrant additions:

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

    // histograms based on Simon's reference plots
    // https://mu2e-elog.physics.northwestern.edu/elog/CRV/70

    // Process each global run info object
    if (diagLevel_ > 0)
      std::cout << "[IntegrationDQM::" << __func__
                << "] CRV data: " << nSubEvents << " sub-events, "
                << gr_infos.size() << " GR Infos\n";
    for (const auto &gr_info : gr_infos) {
      if (diagLevel_ > 0)
        std::cout << "--> CRV data entry: EWT = " << gr_info.EWTCount
                  << " inj-time = " << gr_info.injectionTime * crv_tick_time_
                  << " inj-window = " << gr_info.injectionWindow
                  << " marker-count = " << gr_info.markerCount << std::endl;
      // Fill EWT distribution
      hists.ewt._Hist->Fill(gr_info.EWTCount);
      // Fill injection time and window histograms
      hists.injection_time._Hist->Fill(gr_info.injectionTime *
                                       crv_tick_time_); // ns
      hists.injection_window._Hist->Fill(gr_info.injectionWindow);
      // Marker count vs EWT
      hists.marker_count._Hist->Fill(gr_info.EWTCount, gr_info.markerCount);
      // EWT counter vs EWT for self-consistency check
      hists.ewt_count._Hist->Fill(gr_info.EWTCount, gr_info.EWTCount);
    }
  } else { // using digis, not decoders (not for GR4)

    // I don't think we have digis in the GR data?
    // Also not sure if this is how Ralf does it

    const auto digis = crv_digis_;
    if (!digis) {
      __COUT__ << "[IntegrationDQM::" << __func__
               << "] No CRV digis found with tag " << crvDigiTag_.c_str()
               << std::endl;
      hists.digi_count._Hist->Fill(0);
      return;
    }

    hists.digi_count._Hist->Fill(digis->size());
    // for(auto& digi : *digis) {
    //   hists.digi_t0_._Hist->Fill(digi.GetStartTDC()*crv_tick_time_);
    // }
  }
}

// STM
void ots::IntegrationDQM::summary_stm_fill(art::Event const &event,
                                           const bool fill_pulses) {
  auto &hists = hists_._stm_hists;

  auto digis = stm_digis_;
  if (!digis) {
    __COUT__ << "[IntegrationDQM::" << __func__
             << "] No stm digis found with tag " << stmDigiTag_.c_str()
             << std::endl;
    hists.digi_count._Hist->Fill(0);
    return;
  }
  hists.digi_count._Hist->Fill(digis->size());
  for (auto &digi : *digis) {
    hists.digi_ids._Hist->Fill(digi.DetID());
    hists.digi_t0._Hist->Fill(digi.trigTimeOffset() * stm_tick_time_);
  }

  // fill pulses if requested
  if (fill_pulses) {
    for (auto &digi : *digis) {
      TH1 *h = hists.pulse._Hist;
      h->Reset();
      h->SetTitle("STM waveform;waveform;Counts");
      for (size_t bin = 0; bin < digi.adcs().size(); ++bin) {
        if (int(bin + 1) > h->GetNbinsX())
          break;
        h->SetBinContent(bin + 1, digi.adcs()[bin]);
      }
    }
  }
}

void ots::IntegrationDQM::summary_global_fill(art::Event const &event) {
  auto &hists = hists_._global_hists;

  bool fillCalo = calo_digis_ != nullptr;
  bool fillTrk = trk_digis_ != nullptr;
  bool fillCRV = crv_digis_ != nullptr;
  bool fillSTM = stm_digis_ != nullptr;
  bool fillCRVGR = crv_decoders_ != nullptr; // for using the CRV GR decoders

  // Retrieve the input data

  const auto caloDigis = calo_digis_;
  const auto trkDigis = trk_digis_;
  const auto crvDigis = crv_digis_;
  const auto stmDigis = stm_digis_;
  const auto crv_gr_infos = decode_crv(crv_decoders_);

  // Fill the histograms

  TH1 *hdigi_count = hists.detector_digis._Hist;
  if (fillCalo && caloDigis->size() > 0)
    hdigi_count->Fill("Calo", 1.);
  if (fillTrk && trkDigis->size() > 0)
    hdigi_count->Fill("Trk", 1.);
  if (fillCRV && crvDigis->size() > 0)
    hdigi_count->Fill("CRV", 1.);
  if (fillCRVGR && crv_gr_infos.size() > 0)
    hdigi_count->Fill("CRV", 1.);
  if (fillSTM && stmDigis->size() > 0)
    hdigi_count->Fill("STM", 1.);

  // check for issues in the digi count
  double min_bin(hdigi_count->GetMaximum()), max_bin(0.);
  for (int ibin = 1; ibin <= hdigi_count->GetNbinsX(); ++ibin) {
    const double binc = hdigi_count->GetBinContent(ibin);
    if (binc <= 0)
      continue;
    min_bin = std::min(binc, min_bin);
    max_bin = std::max(binc, max_bin);
  }
  if (max_bin > 0.) {
    const double rel_diff = (max_bin - min_bin) / 0.5 / (min_bin + max_bin);
    if (rel_diff > 0.01)
      IntegrationDQMHistoContainer::BadHist(hdigi_count);
    else if (rel_diff > 0.)
      IntegrationDQMHistoContainer::WarningHist(hdigi_count);
    else
      IntegrationDQMHistoContainer::GoodHist(hdigi_count);
  }

  float trk_t0 = 1.e10f;
  float calo_t0 = 1.e10f;
  float crv_t0 = 1.e10f;
  float stm_t0 = 1.e10f;

  if (fillCalo) {
    for (auto &digi : *caloDigis) {
      calo_t0 = std::min(calo_t0, digi.t0() * calo_tick_time_);
    }
  }
  if (fillTrk) {
    for (auto &digi : *trkDigis) {
      trk_t0 = std::min(trk_t0, digi.TDC()[0] * trk_tick_time_);
    }
  }
  if (fillCRV) {
    for (auto &digi : *crvDigis) {
      crv_t0 = std::min(crv_t0, digi.GetStartTDC() * crv_tick_time_);
    }
  }
  if (fillCRVGR) {
    crv_t0 = 1.e10f;
    for (auto &gr_info : crv_gr_infos) {
      crv_t0 = std::min(crv_t0, gr_info.injectionTime * crv_tick_time_);
    }
    fillCRV = fillCRVGR; // reuse the CRV digi filling flag
  }
  if (fillSTM) {
    for (auto &digi : *stmDigis) {
      stm_t0 = std::min(stm_t0, digi.trigTimeOffset() * stm_tick_time_);
    }
  }

  // Fill the subdetector time difference
  if (fillTrk && fillCalo)
    hists.trk_calo_deltat0._Hist->Fill(trk_t0 - calo_t0);
  if (fillTrk && fillCRV)
    hists.trk_crv_deltat0._Hist->Fill(trk_t0 - crv_t0);
  if (fillTrk && fillSTM)
    hists.trk_stm_deltat0._Hist->Fill(trk_t0 - stm_t0);
  if (fillCalo && fillCRV)
    hists.calo_crv_deltat0._Hist->Fill(calo_t0 - crv_t0);
  if (fillCalo && fillSTM)
    hists.calo_stm_deltat0._Hist->Fill(calo_t0 - stm_t0);
  if (fillCRV && fillSTM)
    hists.crv_stm_deltat0._Hist->Fill(crv_t0 - stm_t0);
}

std::vector<mu2e::CRVDataDecoder::CRVGlobalRunInfo>
ots::IntegrationDQM::decode_crv(
    const std::vector<mu2e::CRVDataDecoder> *decoders) {
  if (!decoders)
    return {};
  const auto nSubEvents = decoders->size();
  // Decode the info
  std::vector<mu2e::CRVDataDecoder::CRVGlobalRunInfo> globalRunInfos;
  for (size_t iSubEvent = 0; iSubEvent < nSubEvents; ++iSubEvent) {
    const mu2e::CRVDataDecoder &CRVDataDecoder((*decoders)[iSubEvent]);
    CRVDataDecoder.setup_event();

    for (size_t iDataBlock = 0; iDataBlock < CRVDataDecoder.block_count();
         ++iDataBlock) {
      auto block = CRVDataDecoder.dataAtBlockIndex(iDataBlock);
      if (block == nullptr)
        continue;
      auto header = block->GetHeader();
      if (!header->isValid())
        continue;
      if (header->GetSubsystemID() != DTCLib::DTC_Subsystem::DTC_Subsystem_CRV)
        continue;

      if (header->GetPacketCount() > 0) {
        std::unique_ptr<mu2e::CRVDataDecoder::CRVROCStatusPacket> crvRocHeader =
            CRVDataDecoder.GetCRVROCStatusPacket(iDataBlock);
        if (crvRocHeader == nullptr)
          continue;
        globalRunInfos.push_back(mu2e::CRVDataDecoder::CRVGlobalRunInfo());
        auto &globalRunInfo = globalRunInfos.back();
        CRVDataDecoder.GetCRVGlobalRunInfo(iDataBlock, globalRunInfo);
        // mu2e::CRVDataDecoder::CRVGlobalRunPayload globalRunPayload;
        // CRVDataDecoder.GetCRVGlobalRunPayload(iDataBlock, globalRunPayload);
      } // end parsing CRV DataBlocks
    }   // loop over DataBlocks within CRVDataDecoders
  }     // Close loop over fragments
  return globalRunInfos;
}

void ots::IntegrationDQM::endJob() {}

void ots::IntegrationDQM::beginRun(const art::Run &run) {}

DEFINE_ART_MODULE(ots::IntegrationDQM)
