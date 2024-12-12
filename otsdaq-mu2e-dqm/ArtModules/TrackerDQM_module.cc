// Author: E. Croft, adapted from code by S. Middleton
// This module (should) produce histograms of data from the straw tracker

#include <TBufferFile.h>
#include <TH1F.h>

//#pragma GCC diagnostic push
//#pragma GCC diagnostic ignored "-Wpedantic"
#include "Offline/TrkHitReco/inc/PeakFit.hh"
//#pragma GCC diagnostic pop
#include "Offline/DataProducts/inc/StrawId.hh"
#include "Offline/DataProducts/inc/TrkTypes.hh"

#include <artdaq-core/Data/Fragment.hh>

#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art_root_io/TFileService.h"
#include "artdaq-core-mu2e/Data/TrackerDataDecoder.hh"
#include "artdaq-core-mu2e/Overlays/DTCEventFragment.hh"
#include "artdaq-core-mu2e/Overlays/FragmentType.hh"
#include "fhiclcpp/types/OptionalAtom.h"
#include "otsdaq-mu2e-dqm/ArtModules/TrackerDQM.h"
#include "otsdaq-mu2e-dqm/ArtModules/TrackerDQMHistoContainer.h"
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/Macros/ProcessorPluginMacros.h"
#include "otsdaq/MessageFacility/MessageFacility.h"
#include "otsdaq/NetworkUtilities/TCPSendClient.h"

namespace ots {
class TrackerDQM : public art::EDAnalyzer {
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
    fhicl::Atom<std::string> moduleTag{Name("moduleTag"),
                                       Comment("Module tag name")};
    fhicl::Atom<int> fittype { Name( "FitType"),
	Comment("Waveform Fit Type")};
    fhicl::Sequence<std::string> histType{
        Name("histType"),
        Comment("This parameter determines which quantity is histogrammed")};
    fhicl::Atom<int> freqDQM{
        Name("freqDQM"),
        Comment("Frequency for sending histograms to the data-receiver")};
    fhicl::Atom<int> diag{Name("diagLevel"), Comment("Diagnostic level"), 0};
  };

  typedef art::EDAnalyzer::Table<Config> Parameters;

  explicit TrackerDQM(Parameters const& conf);

  void analyze(art::Event const& event) override;
  void beginRun(art::Run const&) override;
  void beginJob() override;
  void endJob() override;

  void PlotRate(art::Event const& e);

 private:
  Config conf_;
  int port_;
  std::string address_;
  std::string moduleTag_;
  bool useADCWF_;
  std::vector<std::string> histType_;
  int freqDQM_, diagLevel_, evtCounter_;
  art::ServiceHandle<art::TFileService> tfs;
  TrackerDQMHistoContainer* pedestal_histos = new TrackerDQMHistoContainer();
  TrackerDQMHistoContainer* panel_histos = new TrackerDQMHistoContainer();
  TrackerDQMHistoContainer* summary_histos = new TrackerDQMHistoContainer();
  HistoSender* histSender_;
  bool doPedestalHist_, doPanelHist_;
  std::string moduleTag;
  void analyze_tracker_(const mu2e::TrackerDataDecoder& cc);
};
}  // namespace ots

ots::TrackerDQM::TrackerDQM(Parameters const& conf)
    : art::EDAnalyzer(conf),
      conf_(conf()),
      port_(conf().port()),
      address_(conf().address()),
      moduleTag_(conf().moduleTag()),
      useADCWF_(conf().fittype() != mu2e::TrkHitReco::FitType::firmwarepmp ),    
      histType_(conf().histType()),
      freqDQM_(conf().freqDQM()),
      diagLevel_(conf().diag()),
      evtCounter_(0),
      doPedestalHist_(false),
      doPanelHist_(false) {
  histSender_ = new HistoSender(address_, port_);

  if (diagLevel_ > 0) {
    __COUT__ << "[TrackerDQM::analyze] DQM for " << histType_[0] << std::endl;
  }

  for (std::string name : histType_) {
    if (name == "pedestals") {
      doPedestalHist_ = true;
    }
    if (name == "panels") {
      doPanelHist_ = true;
    }
  }
}

void ots::TrackerDQM::beginJob() {
  __COUT__ << "[TrackerDQM::beginJob] Beginning job" << std::endl;
  summary_histos->BookSummaryHistos(tfs, "PanelOccupancy", 220, 0, 220);
  summary_histos->BookSummaryHistos(tfs, "PlaneOccupancy", 40, 0, 40);

  if (doPedestalHist_) {
    for (int plane = 0; plane < mu2e::StrawId::_nplanes; plane++) {
      for (int panel = 0; panel < mu2e::StrawId::_npanels; panel++) {
        for (int straw = 0; straw < mu2e::StrawId::_nstraws; straw++) {
          pedestal_histos->BookHistos(tfs,
                                      "Pedestal_" + std::to_string(plane) +
                                          "_" + std::to_string(panel) + "_" +
                                          std::to_string(straw),
                                      plane, panel, straw);
        }
      }
    }
  }

  if (doPanelHist_) {
    for (int plane = 0; plane < mu2e::StrawId::_nplanes; plane++) {
      for (int panel = 0; panel < mu2e::StrawId::_npanels; panel++) {
        std::string hName =
            "Panel_" + std::to_string(plane) + "_" + std::to_string(panel);
        panel_histos->BookHistos(tfs, hName, plane, panel, -1);
      }
    }
  }
}

void ots::TrackerDQM::analyze(art::Event const& event) {
  ++evtCounter_;

  auto fragmentHandles = event.getMany<std::vector<mu2e::TrackerDataDecoder>>();

  for (const auto& handle : fragmentHandles) {
    if (!handle.isValid() || handle->empty()) {
      continue;
    }

    for (auto frag : *handle) {
      analyze_tracker_(frag);
    }
  }
}

void ots::TrackerDQM::analyze_tracker_(const mu2e::TrackerDataDecoder& cc) {
  for (size_t curBlockIdx = 0; curBlockIdx < cc.block_count();
       curBlockIdx++) {  // iterate over straws
    auto block_data = cc.dataAtBlockIndex(curBlockIdx);
    if (block_data == nullptr) {
      mf::LogError("TrackerDQM") << "Unable to retrieve header from block "
                                 << curBlockIdx << "!" << std::endl;
      continue;
    }
    auto hdr = block_data->GetHeader();
    if (hdr->GetPacketCount() > 0) {
      auto trkDatas = cc.GetTrackerData(curBlockIdx, useADCWF_); 
      if (trkDatas.empty()) {
        mf::LogError("TrackerDQM")
            << "Error retrieving Tracker data from DataBlock " << curBlockIdx
            << "!";
        continue;
      }

      for (auto& trkData : trkDatas) {
        mu2e::StrawId sid(trkData.first->StrawIndex);
        // mu2e::TrkTypes::TDCValues tdc = {
        // static_cast<uint16_t>(trkData.first->TDC0()),
        // static_cast<uint16_t>(trkData.first->TDC1()) };
        // mu2e::TrkTypes::TOTValues tot = { trkData.first->TOT0,
        // trkData.first->TOT1 };
        mu2e::TrkTypes::ADCWaveform adcs(trkData.second.begin(),
                                         trkData.second.end());
        summary_fill(summary_histos, sid);

        for (std::string name : histType_) {
          if (name == "pedestals") {
            pedestal_fill(pedestal_histos, pedestal_est(adcs), "Pedestal", sid);
          } else if (name == "panels") {
            panel_fill(panel_histos, "Panel", sid);
          } else {
            __COUT_ERR__ << "Unrecognized histogram type" << std::endl;
          }
        }
      }
    }
  }

  if (evtCounter_ % freqDQM_ != 0) return;

  if (diagLevel_ > 0) {
    __COUT__ << "[TrackerDQM::analyze] preparing the BUFFER..." << std::endl;
  }

  // send a packet AND reset the histograms
  std::map<std::string, std::vector<TH1*>> hists_to_send;

  // send the summary hists
  for (size_t i = 0; i < summary_histos->histograms.size(); i++) {
    __COUT__ << "[TrackerDQM::analyze] collecting summary histogram "
             << summary_histos->histograms[i]._Hist << std::endl;
    hists_to_send[moduleTag_ + "_summary"].push_back(
        (TH1*)summary_histos->histograms[i]._Hist->Clone());
    summary_histos->histograms[i]._Hist->Reset();
  }

  for (std::string name : histType_) {
    if (diagLevel_ > 0) {
      __COUT__ << "[TrackerDQM::analyze] collecting histograms from the block: "
               << name << std::endl;
    }
    if (name == "pedestals") {
      // prepare the vector of histograms
      for (size_t i = 0; i < pedestal_histos->histograms.size(); i++) {
        hists_to_send[moduleTag_ + "_" + name + "/plane_" +
                      std::to_string(pedestal_histos->histograms[i].plane) +
                      "/panel_" +
                      std::to_string(pedestal_histos->histograms[i].panel)]
            .push_back((TH1*)pedestal_histos->histograms[i]._Hist->Clone());
        pedestal_histos->histograms[i]._Hist->Reset();
      }
    } else if (name == "panels") {
      if (diagLevel_ > 0) {
        __COUT__ << Form(
                        "[%s::analyze] preparing the collection of hists for  ",
                        moduleTag_.data())
                 << name << " histograms" << std::endl;
      }
      // prepare the vector of histograms
      __COUT__ << Form("[%sDQM::analyze] N hists =  ", moduleTag_.data())
               << panel_histos->histograms.size() << std::endl;
      for (size_t i = 0; i < panel_histos->histograms.size(); i++) {
        std::string refName = moduleTag_ + "_" + name + "/plane_" +
                              std::to_string(panel_histos->histograms[i].plane);
        hists_to_send[refName].push_back(
            (TH1*)panel_histos->histograms[i]._Hist->Clone());
        panel_histos->histograms[i]._Hist->Reset();
      }
    }
  }

  histSender_->sendHistograms(hists_to_send);
}

void ots::TrackerDQM::endJob() {}

void ots::TrackerDQM::beginRun(const art::Run& run) {}

DEFINE_ART_MODULE(ots::TrackerDQM)
