#include "Offline/ProditionsService/inc/ProditionsHandle.hh"
#include "Offline/CaloConditions/inc/CaloDAQMap.hh"
#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "messagefacility/MessageLogger/MessageLogger.h"
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"



#include "Offline/RecoDataProducts/inc/CaloDigi.hh"
#include "art_root_io/TFileService.h"
#include "art_root_io/TFileDirectory.h"


#include "TH1F.h"
#include "TProfile.h"
#include "TROOT.h"
#include "TBrowser.h"
#include "TH2D.h"
#include "TCanvas.h"



#include "ROOT/RCanvas.hxx"
#include "ROOT/RHist.hxx"
#include "ROOT/RHistDrawable.hxx"
#include "ROOT/RDirectory.hxx"
#include "ROOT/RPadBase.hxx"


#include <map>
#include <vector>
#include <string>
#include <cmath>
#include <sstream>
#include <numeric>
#include <memory>
#include <utility>
#include <tuple>

namespace mu2e {

  class CaloDQMOffline : public art::EDAnalyzer {
  public:
    struct Config {
      fhicl::Atom<std::string> address { fhicl::Name("address") };
      fhicl::Atom<int> port { fhicl::Name("port") };
      fhicl::Atom<std::string> moduleTag { fhicl::Name("moduleTag") };
      fhicl::Atom<int> freqDQM { fhicl::Name("freqDQM") };

      fhicl::Atom<std::string> caloDigiModuleLabel { fhicl::Name("caloDigiModuleLabel") };
      fhicl::Atom<bool> enableBoardHistos { fhicl::Name("enableBoardHistos"), true };
      fhicl::Atom<int> maxBoardHistos { fhicl::Name("maxBoardHistos"), -1 };
      fhicl::Atom<bool> enableLogging { fhicl::Name("enableLogging"), false };
    };

    explicit CaloDQMOffline(const art::EDAnalyzer::Table<Config>& config);
    void analyze(art::Event const& event) override;
    void endJob() override;

  private:
    std::string caloDigiModuleLabel_;
    bool enableBoardHistos_;
    int maxBoardHistos_;
    bool enableLogging_;
    int freqDQM_;
    std::string address_;
    int port_;
    std::string moduleTag_;
    bool sendHists_;

    ots::HistoSender* histSender_;
    int eventCounter_ = 0;

    std::map<std::pair<int, int>, std::map<std::string, TH1F*>> boardHistos_;
    std::map<std::pair<int, int>, std::unique_ptr<art::TFileDirectory>> cachedHistosDirs_;
    std::map<std::string, TH1F*> channelWaveformHistos_;
    std::map<int, float> maxValueBySiPM;
    std::map<int, int> crystalIdBySiPM;
    std::set<std::string> channelWaveformStored_;
    std::map<std::string, TH1F*> singleWaveformHistos_;
    std::map<std::pair<int, int>, std::unique_ptr<art::TFileDirectory>> cachedChannelsDirs_; 



    std::unique_ptr<art::TFileDirectory> disk0Dir_;
    std::unique_ptr<art::TFileDirectory> disk1Dir_;
    std::unique_ptr<art::TFileDirectory> globalDir_;

    TH1F* h_asymmetry;
    TProfile* h_baseline_vs_disk;
    TH1F* h_occupancy_disk0_;
    TH1F* h_occupancy_disk1_;
    TH1F* h_baseline_disk0_;
    TH1F* h_baseline_disk1_;
    TH1F* h_rms_disk0_;
    TH1F* h_rms_disk1_;
    TH1F* h_maxval_disk0_;
    TH1F* h_maxval_disk1_;
    TH1F* h_global_channel_dist_;
    TH1F* h_global_board_dist_;
    TH2I* h_global_board_vs_channel_;
    TH2D* h_global_waveform_density_;


    mu2e::ProditionsHandle<mu2e::CaloDAQMap> _calodaqconds_h;
  };

  CaloDQMOffline::CaloDQMOffline(const art::EDAnalyzer::Table<Config>& config)
    : EDAnalyzer{config},
      caloDigiModuleLabel_(config().caloDigiModuleLabel()),
      enableBoardHistos_(config().enableBoardHistos()),
      maxBoardHistos_(config().maxBoardHistos()),
      enableLogging_(config().enableLogging()),
      freqDQM_(config().freqDQM()),
      address_(config().address()),
      port_(config().port()),
      moduleTag_(config().moduleTag()),
      sendHists_(true){
    
    histSender_ = (sendHists_) ? new ots::HistoSender(address_, port_) : nullptr;


    art::ServiceHandle<art::TFileService> tfs;
    disk0Dir_ = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk0"));
    disk1Dir_ = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk1"));

    globalDir_ = std::make_unique<art::TFileDirectory>(tfs->mkdir("Global_Histograms"));

    h_occupancy_disk0_ = disk0Dir_->make<TH1F>("h_occ_d0", "Occupancy (Disk 0)", 3920, 11990, 15900);
    h_occupancy_disk0_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
    h_occupancy_disk0_->GetYaxis()->SetTitle("Hit Count");
    

    h_occupancy_disk1_ = disk1Dir_->make<TH1F>("h_occ_d1", "Occupancy (Disk 1)", 3920, 11990, 15900);
    h_occupancy_disk1_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
    h_occupancy_disk1_->GetYaxis()->SetTitle("Hit Count");

    
    h_global_board_vs_channel_ = globalDir_->make<TH2I>("h_board_vs_channel", "Board vs Channel", 160, 0, 160, 20, 0, 20);
    h_global_board_vs_channel_->GetXaxis()->SetTitle("Board ID");
    h_global_board_vs_channel_->GetYaxis()->SetTitle("Channel ID");

    h_global_waveform_density_ = globalDir_->make<TH2D>("h_waveform_density", "Waveform Density", 150, 0, 150, 400, 2000, 4095);
    h_global_waveform_density_->GetXaxis()->SetTitle("Tick");
    h_global_waveform_density_->GetYaxis()->SetTitle("ADC Value");
    
    h_baseline_disk0_ = disk0Dir_->make<TH1F>("h_base_d0", "Baseline (Disk 0)", 3920, 11990, 15900);
    h_baseline_disk0_->SetMarkerStyle(20);
    h_baseline_disk0_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
    h_baseline_disk0_->GetYaxis()->SetTitle("Baseline");

    

    h_baseline_disk1_ = disk1Dir_->make<TH1F>("h_base_d1", "Baseline (Disk 1)", 3920, 11990, 15900);
    h_baseline_disk1_->SetMarkerStyle(20);
    h_baseline_disk1_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
    h_baseline_disk1_->GetYaxis()->SetTitle("Baseline");


    h_rms_disk0_ = disk0Dir_->make<TH1F>("h_rms_d0", "RMS (Disk 0)", 3920, 11990, 15900);
    h_rms_disk0_->SetMarkerStyle(20);
    h_rms_disk0_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
    h_rms_disk0_->GetYaxis()->SetTitle("Baseline RMS");


    h_rms_disk1_ = disk1Dir_->make<TH1F>("h_rms_d1", "RMS (Disk 1)", 3920, 11990, 15900);
    h_rms_disk1_->SetMarkerStyle(20);
    h_rms_disk1_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
    h_rms_disk1_->GetYaxis()->SetTitle("Baseline RMS");

    
    h_maxval_disk0_ = disk0Dir_->make<TH1F>("h_max_d0", "Max ADC (Disk 0)", 3920, 11990, 15900);
    h_maxval_disk0_->SetMarkerStyle(20);
    h_maxval_disk0_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
    h_maxval_disk0_->GetYaxis()->SetTitle("Maximum ADC Value");
    
    h_maxval_disk1_ = disk1Dir_->make<TH1F>("h_max_d1", "Max ADC (Disk 1)", 3920, 11990, 15900);
    h_maxval_disk1_->SetMarkerStyle(20);
    h_maxval_disk1_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
    h_maxval_disk1_->GetYaxis()->SetTitle("Maximum ADC Value");
    
    h_baseline_vs_disk = globalDir_->make<TProfile>("h_base_vs_d", "Baseline vs Disk", 2, 0, 2);
    h_baseline_vs_disk->GetXaxis()->SetBinLabel(1, "Disk 0");
    h_baseline_vs_disk->GetXaxis()->SetBinLabel(2, "Disk 1");
    h_baseline_vs_disk->GetYaxis()->SetTitle("Mean Baseline [ADC]");

    h_asymmetry = globalDir_->make<TH1F>("h_asym", "Left-Right Asymmetry", 100, -1.0, 1.0);
    h_asymmetry->GetXaxis()->SetTitle("(L - R)/(L + R)");
    h_asymmetry->GetYaxis()->SetTitle("Frequency");

    h_global_channel_dist_ = globalDir_->make<TH1F>("h_channel_dist", "Global Channel Distribution", 20, 0, 20);
    h_global_channel_dist_->GetXaxis()->SetTitle("Channel ID");
    h_global_channel_dist_->GetYaxis()->SetTitle("Frequency");

    h_global_board_dist_ = globalDir_->make<TH1F>("h_board_dist", "Global Board Distribution", 160, 0, 160);
    h_global_board_dist_->GetXaxis()->SetTitle("Board ID");
    h_global_board_dist_->GetYaxis()->SetTitle("Frequency");
  }



  void CaloDQMOffline::analyze(art::Event const& event) {
    art::ServiceHandle<art::TFileService> tfs;
    const auto& caloDigis = *event.getValidHandle(consumes<CaloDigiCollection>(caloDigiModuleLabel_));
    const auto& calodaqconds = _calodaqconds_h.get(event.id());

    for (const auto& digi : caloDigis) {
      const auto& waveform = digi.waveform();
      if (waveform.size() < 5 || digi.peakpos() >= (int)waveform.size()) continue;

      float baseline = std::accumulate(waveform.begin(), waveform.begin() + 5, 0.0f) / 5.0f;
      float mean_sq = std::inner_product(waveform.begin(), waveform.begin() + 5, waveform.begin(), 0.0f) / 5.0f;
      float rms = (mean_sq > baseline * baseline) ? std::sqrt(mean_sq - baseline * baseline) : 0.0f;
      if (!std::isfinite(baseline) || !std::isfinite(rms)) continue;

      int sipmId = digi.SiPMID();
      int crystalId = sipmId / 2;
      crystalIdBySiPM[sipmId] = crystalId;
      maxValueBySiPM[sipmId] = waveform[digi.peakpos()];

      int partnerSiPM = (sipmId % 2 == 0) ? sipmId + 1 : sipmId - 1;
      if (maxValueBySiPM.count(partnerSiPM) && crystalIdBySiPM[sipmId] == crystalIdBySiPM[partnerSiPM]) {
	float L = (sipmId % 2 == 0) ? maxValueBySiPM[sipmId] : maxValueBySiPM[partnerSiPM];
	float R = (sipmId % 2 == 1) ? maxValueBySiPM[sipmId] : maxValueBySiPM[partnerSiPM];
	float asym = (L + R > 0) ? (L - R) / (L + R) : 0.0;
	h_asymmetry->Fill(asym);
      }

      int rawId = calodaqconds.rawId(mu2e::CaloSiPMId(sipmId)).id();
      if (rawId == 9999) continue;

      int boardID = rawId / 20;
      int chanID = rawId % 20;
      int disk = boardID / 80;

      int encoded = boardID * 100 + chanID;

      h_global_board_dist_->Fill(boardID);
      h_global_channel_dist_->Fill(chanID);

      (disk == 0 ? h_occupancy_disk0_ : h_occupancy_disk1_)->Fill(encoded);
      (disk == 0 ? h_baseline_disk0_ : h_baseline_disk1_)->Fill(encoded, baseline);
      (disk == 0 ? h_rms_disk0_ : h_rms_disk1_)->Fill(encoded, rms);
      (disk == 0 ? h_maxval_disk0_ : h_maxval_disk1_)->Fill(encoded, waveform[digi.peakpos()]);
      h_baseline_vs_disk->Fill(disk == 0 ? 0.5 : 1.5, baseline);

      if (enableBoardHistos_ && (maxBoardHistos_ < 0 || (int)boardHistos_.size() < maxBoardHistos_)) {
	std::pair<int, int> boardKey = std::make_pair(disk, boardID);
	if (cachedHistosDirs_.find(boardKey) == cachedHistosDirs_.end()) {
	  art::TFileDirectory boardDir = (disk == 0 ? *disk0Dir_ : *disk1Dir_).mkdir("Board_" + std::to_string(boardID));
	  cachedHistosDirs_[boardKey] = std::make_unique<art::TFileDirectory>(boardDir.mkdir("Histograms"));
	  cachedChannelsDirs_[boardKey] = std::make_unique<art::TFileDirectory>(boardDir.mkdir("Channels"));
	}
	art::TFileDirectory& histosDir = *cachedHistosDirs_[boardKey];

	
	

	auto& histos = boardHistos_[boardKey];
	if (histos.empty()) {
	  histos["occ"] = histosDir.make<TH1F>(Form("D%d_B%03d_Occupancy", disk, boardID), Form("Occupancy for D%d B%03d", disk, boardID), 20, 0, 20);
	  histos["occ"]->GetXaxis()->SetTitle("Channel ID");
	  histos["occ"]->GetYaxis()->SetTitle("Count");

	  histos["base"] = histosDir.make<TH1F>(Form("D%d_B%03d_Baseline", disk, boardID), Form("Baseline for D%d B%03d", disk, boardID), 20, 0, 20);
	  histos["base"]->GetXaxis()->SetTitle("Channel ID");
	  histos["base"]->GetYaxis()->SetTitle("Count");
	  histos["base"]->SetMarkerStyle(20);
	  
	  histos["rms"] = histosDir.make<TH1F>(Form("D%d_B%03d_RMS", disk, boardID), Form("RMS for D%d B%03d", disk, boardID), 20, 0, 20);
	  histos["rms"]->GetXaxis()->SetTitle("Channel ID");
	  histos["rms"]->GetYaxis()->SetTitle("ADC RMS");
	  histos["rms"]->SetMarkerStyle(20);
	  
	  histos["max"] = histosDir.make<TH1F>(Form("D%d_B%03d_Max", disk, boardID), Form("Max for D%d B%03d", disk, boardID), 20, 0, 20);
	  histos["max"]->GetXaxis()->SetTitle("Channel ID");
	  histos["max"]->GetYaxis()->SetTitle("Max ADC");
	  histos["max"]->SetMarkerStyle(20);
	}

	histos["occ"]->Fill(chanID);
	histos["base"]->Fill(chanID, baseline);
	histos["rms"]->Fill(chanID, rms);
	histos["max"]->Fill(chanID, waveform[digi.peakpos()]);

	std::string wf_key = Form("D%d_B%03d_C%02d", disk, boardID, chanID);
	if (!channelWaveformHistos_.count(wf_key)) {
	  TString cname = Form("%s_Waveform", wf_key.c_str());
	  TString ctitle = Form("Waveform for %s (encoded: %05d, sipmid: %04d)", wf_key.c_str(), encoded, sipmId);
	  channelWaveformHistos_[wf_key] = histosDir.make<TH1F>(cname, ctitle, waveform.size(), 0, waveform.size());
	  channelWaveformHistos_[wf_key]->GetYaxis()->SetTitle("ADC Value");
	  channelWaveformHistos_[wf_key]->GetXaxis()->SetTitle("Tick");
	}

	TH1F* chanHist = channelWaveformHistos_[wf_key];
	for (size_t i = 0; i < waveform.size(); ++i) {
	  chanHist->SetBinContent(i + 1, waveform[i]);
	}

	std::string onehit_key = Form("D%d_B%03d_C%02d", disk, boardID, chanID);
	if (channelWaveformStored_.count(onehit_key) == 0) {
	  art::TFileDirectory& chanDir = *cachedChannelsDirs_[boardKey];

	  TString cname = Form("%s_OneHitWaveform", onehit_key.c_str());
	  TString ctitle = Form("One-Hit Waveform for %s (encoded: %05d, sipmid: %04d)", onehit_key.c_str(), encoded, sipmId);
	  TH1F* onehitHist = chanDir.make<TH1F>(cname, ctitle, waveform.size(), 0, waveform.size());
	  onehitHist->GetYaxis()->SetTitle("ADC Value");
	  onehitHist->GetXaxis()->SetTitle("Tick");

	  for (size_t i = 0; i < waveform.size(); ++i) {
	    onehitHist->SetBinContent(i + 1, waveform[i]);
	  }

	  singleWaveformHistos_[onehit_key] = onehitHist;
	  channelWaveformStored_.insert(onehit_key);

	  h_global_board_vs_channel_->Fill(boardID, chanID);

	  for (size_t i = 0; i < waveform.size(); ++i) {
	    h_global_waveform_density_->Fill(i, waveform[i]);
	  }

	}
      }
    }
    ++eventCounter_;
    if (eventCounter_ % freqDQM_ != 0) return;

    std::map<std::string, std::vector<TH1*>> hists_to_send;

    hists_to_send[moduleTag_ + "/Global:replace"] = {
      h_occupancy_disk0_, h_occupancy_disk1_,
      h_baseline_disk0_, h_baseline_disk1_,
      h_rms_disk0_, h_rms_disk1_,
      h_maxval_disk0_, h_maxval_disk1_,
      h_asymmetry,
      h_global_channel_dist_, h_global_board_dist_,
      h_global_board_vs_channel_, h_global_waveform_density_, h_baseline_vs_disk
    };


    for (auto& [key, hist] : channelWaveformHistos_) {
      int disk, boardID, chanID;
      sscanf(key.c_str(), "D%d_B%03d_C%02d", &disk, &boardID, &chanID);
      std::string groupPath = Form("%s/Waveforms/Disk%d/Board%03d:replace", moduleTag_.c_str(), disk, boardID);
      hists_to_send[groupPath].push_back(hist);
    }

    for (auto& [key, hist] : singleWaveformHistos_) {
      int disk, boardID, chanID;
      sscanf(key.c_str(), "D%d_B%03d_C%02d", &disk, &boardID, &chanID);
      std::string groupPath = Form("%s/OneHitWaveforms/Disk%d/Board%03d:replace", moduleTag_.c_str(), disk, boardID);
      hists_to_send[groupPath].push_back(hist);
    }

    for (auto& [boardKey, hmap] : boardHistos_) {
      int disk = boardKey.first;
      int boardID = boardKey.second;
      std::string groupPath = Form("%s/Disk%d/Board%03d:replace", moduleTag_.c_str(), disk, boardID);
      for (auto& [_, h] : hmap)
	hists_to_send[groupPath].push_back(h);
    }


    histSender_->sendHistograms(hists_to_send);
    std::cout << "Sending " << hists_to_send.size() << " histogram groups" << std::endl;
    for (const auto& [dir, vec] : hists_to_send)
      std::cout << "Group: " << dir << " - " << vec.size() << " hists" << std::endl;




  }

void mu2e::CaloDQMOffline::endJob() {
    
}
  
}  // namespace mu2e

DEFINE_ART_MODULE(mu2e::CaloDQMOffline);
