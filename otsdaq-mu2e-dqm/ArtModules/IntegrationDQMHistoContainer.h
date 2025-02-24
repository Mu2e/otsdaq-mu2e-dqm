// -*- mode:c++ -*-
#ifndef _IntegrationDQMHistoContainer_h_
#define _IntegrationDQMHistoContainer_h_

#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"
#include "otsdaq/NetworkUtilities/TCPPublishServer.h"
#include "otsdaq/Macros/CoutMacros.h"
#include <TH1F.h>
#include <string>

namespace ots {

  class IntegrationDQMHistoContainer {
  public:
    IntegrationDQMHistoContainer(){};
    virtual ~IntegrationDQMHistoContainer(void){};
    struct summaryInfoHist_t {
      TH1F *_Hist;
      std::string _type;
      summaryInfoHist_t() { _Hist = nullptr; _type = "add"; }
    };

    // Calo histograms
    struct caloHist_t {
      summaryInfoHist_t digi_count  ;
      summaryInfoHist_t digi_t0     ;
      summaryInfoHist_t digi_tfit   ;
      summaryInfoHist_t digi_dt0    ;
      summaryInfoHist_t digi_dtfit  ;
      summaryInfoHist_t digi_tfit_t0;
      summaryInfoHist_t digi_fit_status;
      summaryInfoHist_t digi_peak   ;
      summaryInfoHist_t sipms       ;

      std::vector<summaryInfoHist_t> pulses;

      void init(art::ServiceHandle<art::TFileService> tfs, const char* folder, const int nSiPMs) {
	IntegrationDQMHistoContainer::BookHist(digi_count   , tfs, "calo_digi_count"  , "Calo digi count;N(digis);Counts"                   ,  50,    0.f,   50.f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_t0      , tfs, "calo_digi_t0"     , "Calo digi t_{0};digi t_{0};Counts"                 , 100,    0.f, 50.e3f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_dt0     , tfs, "calo_digi_dt0"    , "Calo digi #Deltat_{0};digi #Deltat_{0};Counts"     , 100, -250.f,  250.f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_tfit    , tfs, "calo_digi_tfit"   , "Calo digi t_{fit};digi t_{fit};Counts"             , 100,    0.f, 50.e3f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_dtfit   , tfs, "calo_digi_dtfit"  , "Calo digi #Deltat_{fit};digi #Deltat_{fit};Counts" , 500, -100.f,  100.f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_tfit_t0 , tfs, "calo_digi_tfit_t0", "Calo #Deltat_{fit};digi t_{fit} - t_{0};Counts"    , 500,  -10.f,   10.f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_fit_status, tfs, "calo_digi_fit_status", "Calo digi fit status;fit status;Counts"       , 100, -10.5f,  89.5f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_peak    , tfs, "calo_digi_peak"   , "Calo digi peak;digi peak - t_{0};Counts"           ,  40,  -2.5f, 197.5f, folder);
	IntegrationDQMHistoContainer::BookHist(sipms        , tfs, "calo_sipms"       , "Calo SiPM ID;SiPM ID;Counts", 1,  0.f, 1.f, folder);

	// Book pulse histograms
	for(int ihist = 0; ihist < nSiPMs; ++ihist) {
	  IntegrationDQMHistoContainer::BookHist(pulses, tfs, Form("calo_digi_wave_%i", ihist), "Calo SiPM waveform;bin;Counts", 30, 0.f, 30.f, folder, "replace");
	}
      }
      std::vector<TH1*> hists_to_send() {
	std::vector<TH1*> hists = {
	    (TH1*) digi_count     ._Hist->Clone()
	  , (TH1*) digi_t0        ._Hist->Clone()
	  , (TH1*) digi_tfit      ._Hist->Clone()
	  , (TH1*) digi_dt0       ._Hist->Clone()
	  , (TH1*) digi_dtfit     ._Hist->Clone()
	  , (TH1*) digi_tfit_t0   ._Hist->Clone()
	  , (TH1*) digi_fit_status._Hist->Clone()
	  , (TH1*) digi_peak      ._Hist->Clone()
	  , (TH1*) sipms          ._Hist->Clone()
	};
	for(auto& pulse : pulses) hists.push_back((TH1*) pulse._Hist->Clone());
	return hists;
      }
    };

    // Trk histograms
    struct trkHist_t {
      summaryInfoHist_t digi_count;
      summaryInfoHist_t digi_t0   ;
      summaryInfoHist_t digi_dt0  ;

      std::vector<summaryInfoHist_t> pulses;

      void init(art::ServiceHandle<art::TFileService> tfs, const char* folder, const int nStraws) {
	IntegrationDQMHistoContainer::BookHist(digi_count, tfs, "trk_digi_count", "Tracker digi count;N(digis);Counts"               , 500,    0.f,  500.f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_t0   , tfs, "trk_digi_t0"   , "Tracker digi t_{0};digi t_{0};Counts"             , 100,    0.f, 50.e3f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_dt0  , tfs, "trk_digi_dt0"  , "Tracker digi #Deltat_{0};digi #Deltat_{0};Counts" , 100, -250.f,  250.f, folder);

	// Book pulse histograms
	for(int ihist = 0; ihist < nStraws; ++ihist) {
	  IntegrationDQMHistoContainer::BookHist(pulses, tfs, Form("trk_digi_wave_%i", ihist), "Tracker straw waveform;bin;Counts", 30, 0.f, 30.f, folder, "replace");
	}
      }
      std::vector<TH1*> hists_to_send() {
	std::vector<TH1*> hists = {
	    (TH1*) digi_count  ._Hist->Clone()
	  , (TH1*) digi_t0     ._Hist->Clone()
	  , (TH1*) digi_dt0    ._Hist->Clone()
	};
	for(auto& pulse : pulses) hists.push_back((TH1*) pulse._Hist->Clone());
	return hists;
      }
    };

    // CRV histograms
    struct crvHist_t {
      summaryInfoHist_t digi_count      ;
      summaryInfoHist_t ewt             ;
      summaryInfoHist_t injection_time  ;
      summaryInfoHist_t injection_window;
      summaryInfoHist_t marker_count    ;
      summaryInfoHist_t ewt_count       ;

      std::vector<summaryInfoHist_t> pulses;

      void init(art::ServiceHandle<art::TFileService> tfs, const char* folder, const int nChannels) {
	IntegrationDQMHistoContainer::BookHist(digi_count      , tfs, "crv_digi_count"      , "CRV digi count;N(digis);Counts"              ,   30,    0.f,    30.f, folder);
	IntegrationDQMHistoContainer::BookHist(ewt             , tfs, "crv_ewt"             , "CRV event window tags;EWT;Entries"           , 1021, -10.5f, 1010.5f, folder);
	IntegrationDQMHistoContainer::BookHist(marker_count    , tfs, "crv_marker_count"    , "CRV marker counter vs EWT;EWT;Marker counter", 1021, -10.5f, 1010.5f, folder);
	IntegrationDQMHistoContainer::BookHist(ewt_count       , tfs, "crv_ewt_count"       , "CRV EWT counter vs EWT;EWT;EWT counter"      , 1021, -10.5f, 1010.5f, folder);
	IntegrationDQMHistoContainer::BookHist(injection_time  , tfs, "crv_injection_time"  , "CRV injection time;t_{0};Entries"            ,  100,    0.f,  1000.f, folder);
	IntegrationDQMHistoContainer::BookHist(injection_window, tfs, "crv_injection_window", "CRV injection window;#Deltat_{0};Entries"    ,  100,    0.f,  1000.f, folder);

	// Book pulse histograms
	for(int ihist = 0; ihist < nChannels; ++ihist) {
	  IntegrationDQMHistoContainer::BookHist(pulses, tfs, Form("crv_digi_wave_%i", ihist), "CRV waveform;bin;Counts", 30, 0.f, 30.f, folder, "replace");
	}
      }
      std::vector<TH1*> hists_to_send() {
	std::vector<TH1*> hists = {
	    (TH1*) digi_count      ._Hist->Clone()
	  , (TH1*) ewt             ._Hist->Clone()
	  , (TH1*) injection_time  ._Hist->Clone()
	  , (TH1*) injection_window._Hist->Clone()
	  , (TH1*) marker_count    ._Hist->Clone()
	  , (TH1*) ewt_count       ._Hist->Clone()
	};
	for(auto& pulse : pulses) hists.push_back((TH1*) pulse._Hist->Clone());
	return hists;
      }
    };

    // STM histograms
    struct stmHist_t {
      summaryInfoHist_t digi_count;
      summaryInfoHist_t digi_ids  ;
      summaryInfoHist_t digi_t0   ;
      summaryInfoHist_t digi_dt0  ;

      summaryInfoHist_t pulse;

      void init(art::ServiceHandle<art::TFileService> tfs, const char* folder) {
	IntegrationDQMHistoContainer::BookHist(digi_count, tfs, "stm_digi_count", "STM digi count;N(digis);Counts"               ,  30,    0.f,   30.f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_ids  , tfs, "stm_digi_ids"  , "STM digi DetID;DetID;Counts"                  ,   2,    0.f,    2.f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_t0   , tfs, "stm_digi_t0"   , "STM digi t_{0};digi t_{0};Counts"             , 100,    0.f, 50.e3f, folder);
	IntegrationDQMHistoContainer::BookHist(digi_dt0  , tfs, "stm_digi_dt0"  , "STM digi #Deltat_{0};digi #Deltat_{0};Counts" , 100, -250.f,  250.f, folder);

	// Book pulse histogram
	IntegrationDQMHistoContainer::BookHist(pulse, tfs, "stm_digi_wave", "STM waveform;bin;Counts", 50, 0.f, 50.f, folder, "replace");
      }
      std::vector<TH1*> hists_to_send() {
	std::vector<TH1*> hists = {
	    (TH1*) digi_count  ._Hist->Clone()
	  , (TH1*) digi_t0     ._Hist->Clone()
	  , (TH1*) digi_dt0    ._Hist->Clone()
	  , (TH1*) pulse       ._Hist->Clone()
	};
	return hists;
      }
    };

    // Global histograms
    struct globalHist_t {
      summaryInfoHist_t trk_calo_deltat0; //relative timing
      summaryInfoHist_t trk_crv_deltat0 ;
      summaryInfoHist_t trk_stm_deltat0 ;
      summaryInfoHist_t calo_crv_deltat0;
      summaryInfoHist_t calo_stm_deltat0;
      summaryInfoHist_t crv_stm_deltat0 ;
      summaryInfoHist_t detector_digis  ; //digis by detector

      void init(art::ServiceHandle<art::TFileService> tfs, const char* folder) {
	IntegrationDQMHistoContainer::BookHist(trk_calo_deltat0, tfs, "trk_calo_deltat0", "Track - Calo t_{0};#Deltat_{0};Counts", 500, -500.f,  500.f, folder);
	IntegrationDQMHistoContainer::BookHist(trk_crv_deltat0 , tfs, "trk_crv_deltat0" , "Track - CRV t_{0};#Deltat_{0};Counts" , 500, -500.f,  500.f, folder);
	IntegrationDQMHistoContainer::BookHist(trk_stm_deltat0 , tfs, "trk_stm_deltat0" , "Track - STM t_{0};#Deltat_{0};Counts" , 500, -500.f,  500.f, folder);
	IntegrationDQMHistoContainer::BookHist(calo_crv_deltat0, tfs, "calo_crv_deltat0", "Calo - CRV t_{0};#Deltat_{0};Counts"  , 500, -500.f,  500.f, folder);
	IntegrationDQMHistoContainer::BookHist(calo_stm_deltat0, tfs, "calo_stm_deltat0", "Calo - STM t_{0};#Deltat_{0};Counts"  , 500, -500.f,  500.f, folder);
	IntegrationDQMHistoContainer::BookHist(crv_stm_deltat0 , tfs, "crv_stm_deltat0" , "CRV - STM t_{0};#Deltat_{0};Counts"   , 500, -500.f,  500.f, folder);
	IntegrationDQMHistoContainer::BookHist(detector_digis  , tfs, "detector_digis" , "Subdetector events above digi threshold;;N(events)", 5,0,5, folder);
      }
      std::vector<TH1*> hists_to_send() {
	std::vector<TH1*> hists = {
	    (TH1*) trk_calo_deltat0._Hist->Clone()
	  , (TH1*) trk_crv_deltat0 ._Hist->Clone()
	  , (TH1*) trk_stm_deltat0 ._Hist->Clone()
	  , (TH1*) calo_crv_deltat0._Hist->Clone()
	  , (TH1*) calo_stm_deltat0._Hist->Clone()
	  , (TH1*) crv_stm_deltat0 ._Hist->Clone()
	  , (TH1*) detector_digis  ._Hist->Clone()
	};
	return hists;
      }
    };

    globalHist_t _global_hists;
    caloHist_t   _calo_hists  ;
    trkHist_t    _trk_hists   ;
    crvHist_t    _crv_hists   ;
    stmHist_t    _stm_hists   ;

    // Histogram colors by status
    static void GoodHist(TH1* h) {
      h->SetLineColor(kAzure+2);
      h->SetFillColor(kAzure-9);
    }
    static void WarningHist(TH1* h) {
      h->SetLineColor(kOrange+6);
      h->SetFillColor(kOrange-4);
    }
    static void BadHist(TH1* h) {
      h->SetLineColor(kRed-4);
      h->SetFillColor(kRed-6);
    }

    static void BookHist(summaryInfoHist_t& h, art::ServiceHandle<art::TFileService> tfs, std::string Name, std::string Title,
			 const int nBins, const float xmin, const float xmax, const char* folder, std::string type = "add") {
      art::TFileDirectory testDir = tfs->mkdir(folder);
      h._Hist = testDir.make<TH1F>(Name.c_str(), Title.c_str(), nBins, xmin, xmax);
      h._type = type;

      // Initialize histogram style
      h._Hist->SetLineWidth(2);
      h._Hist->GetXaxis()->SetTitleSize(.04);
      h._Hist->GetYaxis()->SetTitleSize(.04);
      GoodHist(h._Hist);
    }

    static void BookHist(std::vector<summaryInfoHist_t>& histograms, art::ServiceHandle<art::TFileService> tfs, std::string Name, std::string Title,
			 const int nBins, const float xmin, const float xmax, const char* folder, std::string type = "add") {
      histograms.push_back(summaryInfoHist_t());
      BookHist(histograms.back(), tfs, Name, Title, nBins, xmin, xmax, folder, type);
    }
  };

} // namespace ots

#endif
