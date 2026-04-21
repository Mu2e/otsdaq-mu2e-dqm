//
// Analyze and Plot LumiStream Information for Data Quality Monitoring
// Original author: Hope Applegate, 2026
//


// ROOT includes
#include "TH1.h"
#include "TH2.h"
#include "TGraph.h"

// art includes
#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/SubRun.h"
#include "art/Framework/Principal/Handle.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"
#include "artdaq-core-mu2e/Data/EventHeader.hh"
#include "fhiclcpp/ParameterSet.h"

// Mu2e includes
#include "Offline/RecoDataProducts/inc/IntensityInfoCalo.hh"
#include "Offline/RecoDataProducts/inc/IntensityInfoTimeCluster.hh"
#include "Offline/RecoDataProducts/inc/IntensityInfoTrackerHits.hh"

// std includes
#include <iostream>
#include <memory>
#include <string>
#include <map>
#include <vector>
#include <cmath>

// LumiAna includes
#include "otsdaq-mu2e-dqm/ArtModules/RecoNPOT.hh" 


// ======================================================================
namespace mu2e {

  class LumiDQM : public art::EDAnalyzer {

  public:

    
    enum class SubRunStatus {
      OK   = 0,
      WARN = 1,
      FLAG = 2
    };

    // ------------------------------------------------------------------
    // fhicl config
    // ------------------------------------------------------------------
    struct Config {
      fhicl::Atom<int> diagLevel {
        fhicl::Name("diagLevel"),
        fhicl::Comment("diagnostic level: 0=quiet, 1=subrun summary, 2=verbose"),
        0
      };
      fhicl::Atom<double> minPOTthreshold {
        fhicl::Name("minPOTthreshold"),
        fhicl::Comment("FLAG if avg recoNPOT per spill falls below this -- beam dropout"),
        1.e6
      };
      fhicl::Atom<double> maxPOTthreshold {
        fhicl::Name("maxPOTthreshold"),
        fhicl::Comment("WARN if avg recoNPOT per spill exceeds this -- unusually high"),
        2.e8
      };
      fhicl::Atom<double> maxEstimatorDiff {
        fhicl::Name("maxEstimatorDiff"),
        fhicl::Comment("WARN if |trackerEstimator - caloD1Estimator| exceeds this"),
        5.e6
      };
      fhicl::Atom<int> minCaloHits {
        fhicl::Name("minCaloHits"),
        fhicl::Comment("FLAG if avg nCaloHits per spill falls below this -- dead calorimeter"),
        10
      };
      fhicl::Atom<int> maxCaloHits {
        fhicl::Name("maxCaloHits"),
        fhicl::Comment("WARN if avg nCaloHits per spill exceeds this -- noisy calorimeter"),
        8000
      };
      fhicl::Atom<int> minTrackerHits {
        fhicl::Name("minTrackerHits"),
        fhicl::Comment("FLAG if avg nTrackerHits per spill falls below this -- dead tracker"),
        10
      };
      fhicl::Atom<int> maxTrackerHits {
        fhicl::Name("maxTrackerHits"),
        fhicl::Comment("WARN if avg nTrackerHits per spill exceeds this -- noisy tracker"),
        12000
      };
      fhicl::Atom<int> nEventsPerBin {
        fhicl::Name("nEventsPerBin"),
        fhicl::Comment("number of events to average over in time-ordered trending histograms"),
        500
      };
    };

    explicit LumiDQM(const art::EDAnalyzer::Table<Config>& config);
    virtual ~LumiDQM() {}

    virtual void beginJob()                       override;
    virtual void endJob()                         override;
    virtual void analyze(const art::Event& e)     override;
    virtual void endSubRun(const art::SubRun& sr) override;

    void accumulateCalo(const mu2e::IntensityInfoCalo& info);
    void accumulateTrackerHits(const mu2e::IntensityInfoTrackerHits& info);
    void accumulateTimeClusters(const mu2e::IntensityInfoTimeCluster& info);
    void fillEventHeaders(std::vector<art::Handle<mu2e::EventHeaders>>& handles);

    SubRunStatus evaluateSubRunStatus(double avgPOT,
                                      double avgNCaloHits,
                                      double avgNTrackerHits,
                                      double estimatorDiff) const;

    static const char* statusString(SubRunStatus s) {
      switch(s) {
      case SubRunStatus::OK   : return "OK";
      case SubRunStatus::WARN : return "WARN";
      case SubRunStatus::FLAG : return "FLAG <<<";
      }
      return "UNKNOWN";
    }

    struct EventAverage {
      //------observables------
      double AsumNCaloHits                   = 0;
      double AsumNCaloHitsD0                 = 0;
      double AsumNCaloHitsD1                 = 0;
      double AsumCaloEnergy                  = 0;
      double AsumNCaphriHits                 = 0;
      double AsumNTrackerHits                = 0;
      double AsumNProtonsDF                  = 0;
      //------reconstucted NPOT------
      double AsumRecoNPOT_primary            = 0;
      double AsumRecoNPOT_trackerHits        = 0;
      double AsumRecoNPOT_caloHitsD1         = 0;
      long unsigned int AnEventsPerBin       = 0;  //to increment (until _nEventsPerBin)
    };

    EventAverage _avg;

  private:


    // ------------------------------------------------------------------
    // Configuration members initialized in constructor
    // ------------------------------------------------------------------
    int                 _diagLevel;
    double              _minPOTthreshold;
    double              _maxPOTthreshold;
    double              _maxEstimatorDiff;
    int                 _minCaloHits;
    int                 _maxCaloHits;
    int                 _minTrackerHits;
    int                 _maxTrackerHits;
    long unsigned int   _nEventsPerBin; 


    // ------------------------------------------------------------------
    // Event / subrun counters
    // ------------------------------------------------------------------
    std::map<std::string, size_t> _counter_by_object;

    // ------------------------------------------------------------------
    // Per-subrun accumulator
    // ------------------------------------------------------------------
    struct SubRunAccumulator {
      //------observables------
      double sumNCaloHits;
      double sumNCaloHitsD0;
      double sumNCaloHitsD1;
      double sumCaloEnergy;
      double sumNCaphriHits;
      double sumNTrackerHits;
      double sumNProtonsDF;
      //------reconstucted NPOT------
      double sumRecoNPOT_primary;
      double sumRecoNPOT_trackerHits;
      double sumRecoNPOT_caloEnergy;
      double sumRecoNPOT_caloHits;
      double sumRecoNPOT_caloHitsD0;
      double sumRecoNPOT_caloHitsD1;
      double sumRecoNPOT_caphriHits;
      double sumRecoNPOT_nProtonsDF;
      //------number of events-----
      size_t nEvents;
      //------Reset at end of each subrun after averages computed-----
      void reset() {
        sumNCaloHits            = 0.;
        sumNCaloHitsD0          = 0.;
        sumNCaloHitsD1          = 0.;
        sumCaloEnergy           = 0.;
        sumNCaphriHits          = 0.;
        sumNTrackerHits         = 0.;
        sumNProtonsDF           = 0.;
        sumRecoNPOT_primary     = 0.;
        sumRecoNPOT_trackerHits = 0.;
        sumRecoNPOT_caloEnergy  = 0.;
        sumRecoNPOT_caloHits    = 0.;
        sumRecoNPOT_caloHitsD0  = 0.;
        sumRecoNPOT_caloHitsD1  = 0.;
        sumRecoNPOT_caphriHits  = 0.;
        sumRecoNPOT_nProtonsDF  = 0.;
        nEvents                 = 0 ;
      }
    };
    
    SubRunAccumulator _acc;

    
    // ------------------------------------------------------------------
    // Histograms
    // ------------------------------------------------------------------

    //------Observables per event-------
    TH1* _NCaloHits;
    TH1* _NCaloHitsD0;
    TH1* _NCaloHitsD1;
    TH1* _CaloEnergy;
    TH1* _NCaphriHits;
    TH1* _NTrackerHits;
    TH1* _NProtonsDF;

    //-------RecoNPOT per event-------
    TH1* _RecoNPOT_primary;
    TH1* _RecoNPOT_trackerHits;
    TH1* _RecoNPOT_caloEnergy;
    TH1* _RecoNPOT_caloHits;
    TH1* _RecoNPOT_caloHitsD0;
    TH1* _RecoNPOT_caloHitsD1;
    TH1* _RecoNPOT_caphriHits;
    TH1* _RecoNPOT_nProtonsDF;

    //-------Cross-estimator consistency------
    TH1* _EstimatorDiff;
    TH2* _EstimatorDiff_vs_POT;

    //------Per-subrun summary------
    TH1* _SubRunStatus;
    TH1* _AvgRecoNPOT_perSubRun; //using primary Reco
    TH1* _NEvents_perSubRun;

    //------Average value over Nevents TGraphs-------
    TGraph* _RecoNPOT_primary_vs_Nevents;
    TGraph* _RecoNPOT_trackerHits_vs_Nevents;
    TGraph* _RecoNPOT_caloD1_vs_Nevents;
    TGraph* _EstimatorDiff_vs_Nevents;
    TGraph* _NCaloHits_vs_Nevents;
    TGraph* _NTrackerHits_vs_Nevents;
    TGraph* _NCaloHitsD0_vs_Nevents;
    TGraph* _NCaloHitsD1_vs_Nevents;
    TGraph* _NCaphriHits_vs_Nevents;
    TGraph* _NProtonsDF_vs_Nevents;
    TGraph* _CaloEnergy_vs_Nevents;

    //-------Per-subrun TGraphs-----
    TGraph* _AvgNTrackerHits_vs_subrun;
    TGraph* _AvgNCaloHitsD1_vs_subrun;
    TGraph* _AvgRecoNPOT_vs_subrun;
    TGraph* _AvgReco_NTrackerHits_vs_subrun;
    TGraph* _AvgReco_NCaloHitsD1_vs_subrun;
    TGraph* _EstimatorDiff_vs_subrun;
    TGraph* _SubRunStatus_vs_subrun;

    //-------Keep track of index for subrun plots------
    int _subrunGraphIndex      = 1;
    //-------Keep track of index for per event plots------
    int _globalEventIndex      = 0;
    int _averageEventIndex     = 1;


  }; 

  // ======================================================================
  // Constructor
  // ======================================================================
  LumiDQM::LumiDQM(const art::EDAnalyzer::Table<Config>& config)
    : art::EDAnalyzer{config}
    , _diagLevel                 (config().diagLevel())
    , _minPOTthreshold           (config().minPOTthreshold())
    , _maxPOTthreshold           (config().maxPOTthreshold())
    , _maxEstimatorDiff          (config().maxEstimatorDiff())
    , _minCaloHits               (config().minCaloHits())
    , _maxCaloHits               (config().maxCaloHits())
    , _minTrackerHits            (config().minTrackerHits())
    , _maxTrackerHits            (config().maxTrackerHits())
    , _nEventsPerBin             (config().nEventsPerBin())
  {
    _acc.reset(); // zero-initialize the accumulator
  }

  // ======================================================================
  // beginJob
  // ======================================================================
  void LumiDQM::beginJob() {

    art::ServiceHandle<art::TFileService> tfs;
    TH1::SetDefaultSumw2();

    //set up folders
    art::TFileDirectory dirObs     = tfs->mkdir("observables");
    art::TFileDirectory dirReco    = tfs->mkdir("recoNPOT");
    art::TFileDirectory dirConsist = tfs->mkdir("consistency");
    art::TFileDirectory dirSubRun  = tfs->mkdir("subrun_summary");
    art::TFileDirectory dirTrend   = tfs->mkdir("trending");

    constexpr double maxPOT = 2.e8;
    //----------------------------------
    //      Make the Histograms!
    //----------------------------------
    //-------Observable distributions (filled per event)------
    _NCaloHits    = dirObs.make<TH1F>("hNCaloHits"   , "N(Calorimeter hits);N(hits);Events"    , 100, 0., 5000. );
    _NCaloHitsD0  = dirObs.make<TH1F>("hNCaloHitsD0" , "N(Calo hits disk 0);N(hits);Events"    , 100, 0., 3000. );
    _NCaloHitsD1  = dirObs.make<TH1F>("hNCaloHitsD1" , "N(Calo hits disk 1);N(hits);Events"    , 100, 0., 2000. );
    _CaloEnergy   = dirObs.make<TH1F>("hCaloEnergy"  , "Calorimeter energy;Energy (MeV);Events", 100, 0., 12000.);
    _NCaphriHits  = dirObs.make<TH1F>("hNCaphriHits" , "N(CAPHRI hits);N(hits);Events"         ,  30, 0., 30.   );
    _NTrackerHits = dirObs.make<TH1F>("hNTrackerHits", "N(Tracker hits);N(hits);Events"        , 100, 0., 10000.);
    _NProtonsDF   = dirObs.make<TH1F>("hNProtonsDF"  , "N(DF proton TCs);N(TCs);Events"        ,  40, 0., 40.   );

    //------RecoNPOT distributions (filled per event)--------
    _RecoNPOT_primary     = dirReco.make<TH1F>("RecoNPOT_primary"    , "Primary recoNPOT [avg(trkHits,caloD1)];N(POT);Events", 100, 0., maxPOT);
    _RecoNPOT_trackerHits = dirReco.make<TH1F>("RecoNPOT_trackerHits", "RecoNPOT from tracker hits;N(POT);Events"             , 100, 0., maxPOT);
    _RecoNPOT_caloEnergy  = dirReco.make<TH1F>("RecoNPOT_caloEnergy" , "RecoNPOT from calo energy;N(POT);Events"              , 100, 0., maxPOT);
    _RecoNPOT_caloHits    = dirReco.make<TH1F>("RecoNPOT_caloHits"   , "RecoNPOT from total calo hits;N(POT);Events"          , 100, 0., maxPOT);
    _RecoNPOT_caloHitsD0  = dirReco.make<TH1F>("RecoNPOT_caloHitsD0" , "RecoNPOT from calo hits disk 0;N(POT);Events"         , 100, 0., maxPOT);
    _RecoNPOT_caloHitsD1  = dirReco.make<TH1F>("RecoNPOT_caloHitsD1" , "RecoNPOT from calo hits disk 1;N(POT);Events"         , 100, 0., maxPOT);
    _RecoNPOT_caphriHits  = dirReco.make<TH1F>("RecoNPOT_caphriHits" , "RecoNPOT from CAPHRI hits;N(POT);Events"              , 100, 0., maxPOT);
    _RecoNPOT_nProtonsDF  = dirReco.make<TH1F>("RecoNPOT_nProtonsDF" , "RecoNPOT from DF time clusters;N(POT);Events"         , 100, 0., maxPOT);

    //------Cross-estimator consistency (filled per event)------
    _EstimatorDiff        = dirConsist.make<TH1F>("EstimatorDiff", "Tracker recoNPOT - CaloD1 recoNPOT;#Delta N(POT);Events", 100, -5.e7, 5.e7);
    _EstimatorDiff_vs_POT = dirConsist.make<TH2F>("EstimatorDiff_vs_POT", "Estimator diff vs primary recoNPOT;Primary recoNPOT;#Delta N(POT)", 100, 0., maxPOT, 100, -5.e7, 5.e7);

    //------Per-subrun summary hists------
    _SubRunStatus          = dirSubRun.make<TH1F>("SubRunStatus", "DQM status per sub-run;Sub-Run Index;Status (0=OK 1=WARN 2=FLAG)", 500, 0., 500.);
    _AvgRecoNPOT_perSubRun = dirSubRun.make<TH1F>("AvgRecoNPOT_perSubRun", "Avg primary recoNPOT per sub-run;Sub-Run Index;Avg recoNPOT", 500, 0., 500.);
    _NEvents_perSubRun     = dirSubRun.make<TH1F>("NEvents_perSubRun", "N(events) per sub-run;Sub-Run Index;N(events)", 500, 0., 500.);

    //------Filled with averages Per-Nevents: TGraphs-------
    std::string xTitle = "Bin index (each contains " + std::to_string(_nEventsPerBin) + " events)";     //changes as _nEventsPerBin as input changes
    _RecoNPOT_primary_vs_Nevents     = dirTrend.makeAndRegister<TGraph>("RecoNPOT_primary_vs_Nevents",     ("Primary recoNPOT  avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTObs/RecoFromObs#GT primary recoNPOT").c_str());
    _RecoNPOT_trackerHits_vs_Nevents = dirTrend.makeAndRegister<TGraph>("RecoNPOT_trackerHits_vs_Nevents", ("Tracker recoNPOT  avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTObs/RecoFromObs#GT recoNPOT (tracker hits)").c_str());
    _RecoNPOT_caloD1_vs_Nevents      = dirTrend.makeAndRegister<TGraph>("RecoNPOT_caloD1_vs_Nevents",      ("CaloD1 recoNPOT  avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTObs/RecoFromObs#GT recoNPOT (calo disk 1)").c_str());
    _EstimatorDiff_vs_Nevents        = dirTrend.makeAndRegister<TGraph>("EstimatorDiff_vs_Nevents",        ("Estimator diff avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTObs/RecoFromObs#GT (tracker recoNPOT - CaloD1 recoNPOT)").c_str());
    _NCaloHits_vs_Nevents            = dirTrend.makeAndRegister<TGraph>("NCaloHits_vs_Nevents",            ("N(calo hits) avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTN(calo hits)#GT").c_str());
    _NCaloHitsD0_vs_Nevents          = dirTrend.makeAndRegister<TGraph>("NCaloHitsD0_vs_Nevents",          ("N(calo hits disk 0) avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTN(calo hits D0)#GT").c_str());
    _NCaloHitsD1_vs_Nevents          = dirTrend.makeAndRegister<TGraph>("NCaloHitsD1_vs_Nevents",          ("N(calo hits disk 1) avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTN(calo hits D1)#GT").c_str());
    _NTrackerHits_vs_Nevents         = dirTrend.makeAndRegister<TGraph>("NTrackerHits_vs_Nevents",         ("N(tracker hits) avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTN(tracker hits)#GT").c_str());
    _NCaphriHits_vs_Nevents          = dirTrend.makeAndRegister<TGraph>("NCaphriHits_vs_Nevents",          ("N(CAPHRI hits) avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTN(CAPHRI hits)#GT").c_str());
    _NProtonsDF_vs_Nevents           = dirTrend.makeAndRegister<TGraph>("NProtonsDF_vs_Nevents",           ("N(DF proton TCs) avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTN(DF proton TCs)#GT").c_str());
    _CaloEnergy_vs_Nevents           = dirTrend.makeAndRegister<TGraph>("CaloEnergy_vs_Nevents",           ("Calo energy avg over " + std::to_string(_nEventsPerBin) + " events vs bin index;" + xTitle + ";#LTCalo energy#GT (MeV)").c_str());

    //--------Per-subrun TGraphs---------
    _AvgRecoNPOT_vs_subrun           = dirSubRun.makeAndRegister<TGraph>("AvgRecoNPOT_vs_subrun", "Avg primary recoNPOT vs sub-run;Sub-Run Index;Avg recoNPOT");
    _AvgReco_NTrackerHits_vs_subrun  = dirSubRun.makeAndRegister<TGraph>("AvgReco_NTrackerHits_vs_subrun", "Avg recoNPOT from NTrackerHits vs sub-run;Sub-Run Index;Avg recoNPOT from NTrackerHits");
    _AvgReco_NCaloHitsD1_vs_subrun   = dirSubRun.makeAndRegister<TGraph>("AvgReco_NCaloHitsD1_vs_subrun", "Avg recoNPOT from NCaloHitsD1 vs sub-run;Sub-Run Index;Avg recoNPOT from NCaloHitsD1");
    _AvgNTrackerHits_vs_subrun       = dirSubRun.makeAndRegister<TGraph>("AvgNTrackerHits_vs_subrun", "Avg N(tracker hits) vs sub-run;Sub-Run Index;Avg N(tracker hits)");
    _AvgNCaloHitsD1_vs_subrun        = dirSubRun.makeAndRegister<TGraph>("AvgNCaloHitsD1_vs_subrun", "Avg N(calo hitsD1) vs sub-run;Sub-Run Index;Avg N(calo hitsD1)");
    _EstimatorDiff_vs_subrun         = dirSubRun.makeAndRegister<TGraph>("EstimatorDiff_vs_subrun", "Avg estimator diff vs sub-run;Sub-Run Index;|Tracker - CaloD1| recoNPOT");
    _SubRunStatus_vs_subrun          = dirSubRun.makeAndRegister<TGraph>("SubRunStatus_vs_subrun", "DQM status vs sub-run;Sub-Run Index;Status (0=OK 1=WARN 2=FLAG)");
  }

  // ======================================================================
  // endJob
  // ======================================================================
  void LumiDQM::endJob() {
    std::cout << "LumiDQM::" << __func__
              << ": Processed " << _globalEventIndex<< " events"
              << " across "     << _subrunGraphIndex << " sub-runs\n";
  }

  // ======================================================================
  // accumulateCalo
  // ======================================================================
  void LumiDQM::accumulateCalo(const mu2e::IntensityInfoCalo& info) {

    //-----get observables------
    const int    nCaloHits   = info.nCaloHits();
    const int    nCaloHitsD0 = info.nCaloHitsD0();
    const int    nCaloHitsD1 = info.nCaloHitsD1();
    const double caloEnergy  = info.caloEnergy();
    const int    nCaphri     = info.nCaphriHits();

    //------calculate reconstructed NPOT------
    const double recoNPOT_caloE    = RecoNPOT::POTfromCaloEnergy(caloEnergy);
    const double recoNPOT_caloHits = RecoNPOT::POTfromCaloHits(nCaloHits);
    const double recoNPOT_caloD0   = RecoNPOT::POTfromnHitsD0(nCaloHitsD0);
    const double recoNPOT_caloD1   = RecoNPOT::POTfromnHitsD1(nCaloHitsD1);
    const double recoNPOT_caphri   = RecoNPOT::POTfromCAPHRI(nCaphri);

    //------Fill observable histograms with value-------
    _NCaloHits  ->Fill(nCaloHits);
    _NCaloHitsD0->Fill(nCaloHitsD0);
    _NCaloHitsD1->Fill(nCaloHitsD1);
    _CaloEnergy ->Fill(caloEnergy);
    _NCaphriHits->Fill(nCaphri);

    //------fill reco histograms with value------
    _RecoNPOT_caloEnergy ->Fill(recoNPOT_caloE);
    _RecoNPOT_caloHits   ->Fill(recoNPOT_caloHits);
    _RecoNPOT_caloHitsD0 ->Fill(recoNPOT_caloD0);
    _RecoNPOT_caloHitsD1 ->Fill(recoNPOT_caloD1);
    _RecoNPOT_caphriHits ->Fill(recoNPOT_caphri);

    
    //-------Add to the accumulated sum to average over the subrun-----
    _acc.sumNCaloHits           += nCaloHits;
    _acc.sumNCaloHitsD0         += nCaloHitsD0;
    _acc.sumNCaloHitsD1         += nCaloHitsD1;
    _acc.sumCaloEnergy          += caloEnergy;
    _acc.sumNCaphriHits         += nCaphri;
    _acc.sumRecoNPOT_caloEnergy += recoNPOT_caloE;
    _acc.sumRecoNPOT_caloHits   += recoNPOT_caloHits;
    _acc.sumRecoNPOT_caloHitsD0 += recoNPOT_caloD0;
    _acc.sumRecoNPOT_caloHitsD1 += recoNPOT_caloD1;
    _acc.sumRecoNPOT_caphriHits += recoNPOT_caphri;

    //-----Add to the average accumulating to average over events------
    _avg.AsumNCaloHits           += nCaloHits;
    _avg.AsumNCaloHitsD0         += nCaloHitsD0;
    _avg.AsumNCaloHitsD1         += nCaloHitsD1;
    _avg.AsumCaloEnergy          += caloEnergy;
    _avg.AsumNCaphriHits         += nCaphri;
    _avg.AsumRecoNPOT_caloHitsD1 += recoNPOT_caloD1;
    _avg.AsumRecoNPOT_primary    += recoNPOT_caloD1;
    
  }

  // ======================================================================
  // accumulateTrackerHits
  // ======================================================================
  void LumiDQM::accumulateTrackerHits(const mu2e::IntensityInfoTrackerHits& info) {

    //-----get observables and calculate Reco------
    const int    nTrkHits         = info.nTrackerHits();
    const double recoNPOT_trkHits = RecoNPOT::POTfromTrackerHits(nTrkHits);

    //-----Fill histograms------
    _NTrackerHits        ->Fill(nTrkHits);
    _RecoNPOT_trackerHits->Fill(recoNPOT_trkHits);

    //-----add to the sums------
    _acc.sumNTrackerHits         += nTrkHits;
    _acc.sumRecoNPOT_trackerHits += recoNPOT_trkHits;


    //-----Add to the average accumulating to average over events------
    _avg.AsumNTrackerHits         += nTrkHits;
    _avg.AsumRecoNPOT_trackerHits += recoNPOT_trkHits;
    _avg.AsumRecoNPOT_primary     += recoNPOT_trkHits;
    
  }

  // ======================================================================
  // accumulateTimeClusters
  // ======================================================================
  void LumiDQM::accumulateTimeClusters(const mu2e::IntensityInfoTimeCluster& info) {
    //-----get observables and calculate Reco------
    const int    nProtons = info.nProtonTCs();
    const double recoNPOT_nProtonsDF = RecoNPOT::POTfromDFTCs(nProtons);

    //-----Fill histograms and add to running sums-----
   
    _NProtonsDF                 ->Fill(nProtons);
    _RecoNPOT_nProtonsDF        ->Fill(recoNPOT_nProtonsDF);
    
    _acc.sumNProtonsDF          += nProtons;
    _acc.sumRecoNPOT_nProtonsDF += recoNPOT_nProtonsDF;

    //-----Add to the average accumulating to average over events------
    _avg.AsumNProtonsDF          += nProtons;    


  }


  // ======================================================================
  // fillEventHeaders
  // ======================================================================
  void LumiDQM::fillEventHeaders(std::vector<art::Handle<mu2e::EventHeaders>>& handles) {
    if(_diagLevel > 0) {
      std::cout << "LumiDQM::" << __func__
                << ": Processing " << handles.size() << " handles\n";
    }
    for(const auto& handle : handles) {
      if(!handle.isValid() || handle->empty()) continue;
      _acc.nEvents                  += handle->size();
      _counter_by_object["headers"] += handle->size();
    }
  }

  // ======================================================================
  // evaluateSubRunStatus
  // ======================================================================
  LumiDQM::SubRunStatus LumiDQM::evaluateSubRunStatus(double avgPOT,
                                                      double avgNCaloHits,
                                                      double avgNTrackerHits,
                                                      double estimatorDiff) const {
    SubRunStatus status = SubRunStatus::OK;

    // FLAG checks
    if(avgPOT          < _minPOTthreshold) status = SubRunStatus::FLAG;
    if(avgNCaloHits    < _minCaloHits    ) status = SubRunStatus::FLAG;
    if(avgNTrackerHits < _minTrackerHits ) status = SubRunStatus::FLAG;

    // WARN checks
    if(status != SubRunStatus::FLAG) {
      if(avgPOT          > _maxPOTthreshold            ) status = SubRunStatus::WARN;
      if(avgNCaloHits    > _maxCaloHits                ) status = SubRunStatus::WARN;
      if(avgNTrackerHits > _maxTrackerHits             ) status = SubRunStatus::WARN;
      if(std::abs(estimatorDiff) > _maxEstimatorDiff   ) status = SubRunStatus::WARN;
    }

    return status;
  }
  // ======================================================================
  // analyze -- per-event
  // ======================================================================
  void LumiDQM::analyze(const art::Event& event)  {
    //All intensity info products are stored at SubRun level therefore per-event processing happens in endSubRun()
  }
  

  // ======================================================================
  // endSubRun
  // ======================================================================
  void LumiDQM::endSubRun(const art::SubRun& sr) {

    const art::SubRunNumber_t subrunNum = sr.subRun();
    const art::RunNumber_t    runNum    = sr.run();

    size_t nEntries = 0;

    auto caloHandles   = sr.getMany<mu2e::IntensityInfosCalo>();
    auto trkHandles    = sr.getMany<mu2e::IntensityInfosTrackerHits>();
    auto tcHandles     = sr.getMany<mu2e::IntensityInfosTimeCluster>();
    auto headerHandles = sr.getMany<mu2e::EventHeaders>();

   
    if(_diagLevel > 0) {
      std::cout << "LumiDQM::" << __func__
                << ": SubRun " << runNum << ":" << subrunNum
                << " -- retrieved " << caloHandles.size()   << " calo handles"
                << ", "             << trkHandles.size()    << " tracker handles"
                << ", "             << tcHandles.size()     << " TC handles"
                << ", "             << headerHandles.size() << " header handles\n";
    }

   
    if(!caloHandles.empty()  && caloHandles[0].isValid() &&
       !trkHandles.empty()   && trkHandles[0].isValid()  &&
       !tcHandles.empty()    && tcHandles[0].isValid()) {

      const auto& caloVec = *caloHandles[0];
      const auto& trkVec  = *trkHandles[0];
      const auto& tcVec   = *tcHandles[0];
      nEntries = std::min({caloVec.size(), trkVec.size(), tcVec.size()});

      for(size_t i = 0; i < nEntries; ++i) {
        
        accumulateCalo        (caloVec[i]);
        accumulateTrackerHits (trkVec[i]);
        accumulateTimeClusters(tcVec[i]);

        //----Fill Primary Reco per event and calc Estimator Differences----
        const double recoD1_this  = RecoNPOT::POTfromnHitsD1(caloVec[i].nCaloHitsD1());
        const double recoTrk_this = RecoNPOT::POTfromTrackerHits(trkVec[i].nTrackerHits());
        const double primary_this = (recoD1_this + recoTrk_this) / 2.0;
        const double diff_this    = recoTrk_this - recoD1_this;

        _RecoNPOT_primary    ->Fill(primary_this);
        _EstimatorDiff       ->Fill(diff_this);
        _EstimatorDiff_vs_POT->Fill(primary_this, diff_this);

        _avg.AnEventsPerBin++;
        _globalEventIndex++;

        //------TGraphs every _nEventsPerBin------
        if(_avg.AnEventsPerBin >= _nEventsPerBin) {
          const double n = static_cast<double>(_nEventsPerBin);

          const double avgTrk     = _avg.AsumRecoNPOT_trackerHits / n;
          const double avgCaloD1  = _avg.AsumRecoNPOT_caloHitsD1  / n;
          const double avgPrimary = _avg.AsumRecoNPOT_primary / (2.0 * n);
          const double avgDiff    =  avgTrk - avgCaloD1;

          //-----averages of reco------
          _RecoNPOT_primary_vs_Nevents    ->AddPoint(_averageEventIndex, avgPrimary);
          _RecoNPOT_trackerHits_vs_Nevents->AddPoint(_averageEventIndex, avgTrk);
          _RecoNPOT_caloD1_vs_Nevents     ->AddPoint(_averageEventIndex, avgCaloD1);
          _EstimatorDiff_vs_Nevents       ->AddPoint(_averageEventIndex, avgDiff);
          //-----averages of observables------
          _NCaloHits_vs_Nevents           ->AddPoint(_averageEventIndex, _avg.AsumNCaloHits    / n);
          _NCaloHitsD0_vs_Nevents         ->AddPoint(_averageEventIndex, _avg.AsumNCaloHitsD0  / n);
          _NCaloHitsD1_vs_Nevents         ->AddPoint(_averageEventIndex, _avg.AsumNCaloHitsD1  / n);
          _NTrackerHits_vs_Nevents        ->AddPoint(_averageEventIndex, _avg.AsumNTrackerHits / n);
          _NCaphriHits_vs_Nevents         ->AddPoint(_averageEventIndex, _avg.AsumNCaphriHits  / n);
          _NProtonsDF_vs_Nevents          ->AddPoint(_averageEventIndex, _avg.AsumNProtonsDF   / n);
          _CaloEnergy_vs_Nevents          ->AddPoint(_averageEventIndex, _avg.AsumCaloEnergy   / n);

          // reset accumulators and index events
          _avg.AsumNCaloHits            = 0;
          _avg.AsumNCaloHitsD0          = 0;
          _avg.AsumNCaloHitsD1          = 0;
          _avg.AsumCaloEnergy           = 0;
          _avg.AsumNCaphriHits          = 0;
          _avg.AsumNTrackerHits         = 0;
          _avg.AsumNProtonsDF           = 0;
          _avg.AsumRecoNPOT_trackerHits = 0;
          _avg.AsumRecoNPOT_caloHitsD1  = 0;
          _avg.AsumRecoNPOT_primary     = 0;
          _avg.AnEventsPerBin           = 0;
          _averageEventIndex ++;
        }
      }
    }
    fillEventHeaders(headerHandles);
    // compute subrun averages (divide summed recoNPOT by the number of events)
    
    const double n = (_acc.nEvents > 0) ? static_cast<double>(_acc.nEvents) : 1.0;    

    const double avgRecoNPOT_trackerHits = _acc.sumRecoNPOT_trackerHits / n;
    const double avgRecoNPOT_caloD1      = _acc.sumRecoNPOT_caloHitsD1  / n;
    const double avgRecoNPOT_primary     = (avgRecoNPOT_trackerHits + avgRecoNPOT_caloD1) / 2.0;

    const double avgNCaloHitsD1_         = _acc.sumNCaloHitsD1  / n;
    const double avgNTrackerHits_        = _acc.sumNTrackerHits / n;
   
    const double estimatorDiff_          = avgRecoNPOT_trackerHits - avgRecoNPOT_caloD1;

    const double avgNCaloHits_           = _acc.sumNCaloHits    / n;
    const SubRunStatus status = evaluateSubRunStatus(avgRecoNPOT_primary, avgNCaloHits_, avgNTrackerHits_, estimatorDiff_); 
   
    _SubRunStatus             ->SetBinContent(_subrunGraphIndex, static_cast<int>(status));
    _AvgRecoNPOT_perSubRun    ->SetBinContent(_subrunGraphIndex, avgRecoNPOT_primary);
    _NEvents_perSubRun        ->SetBinContent(_subrunGraphIndex, static_cast<double>(_acc.nEvents));

    _AvgRecoNPOT_vs_subrun          ->AddPoint(_subrunGraphIndex, avgRecoNPOT_primary);
    _AvgReco_NTrackerHits_vs_subrun ->AddPoint(_subrunGraphIndex, avgRecoNPOT_trackerHits);
    _AvgReco_NCaloHitsD1_vs_subrun  ->AddPoint(_subrunGraphIndex, avgRecoNPOT_caloD1);
    _AvgNTrackerHits_vs_subrun      ->AddPoint(_subrunGraphIndex, avgNTrackerHits_);
    _AvgNCaloHitsD1_vs_subrun       ->AddPoint(_subrunGraphIndex, avgNCaloHitsD1_);
    _EstimatorDiff_vs_subrun        ->AddPoint(_subrunGraphIndex, std::abs(estimatorDiff_));
    _SubRunStatus_vs_subrun         ->AddPoint(_subrunGraphIndex, static_cast<double>(status));
    // formatted summary printout
    std::cout << "\n";
    std::cout << "===================================================\n";
    std::cout << "  LumiDQM SubRun Summary: Run " << runNum << " SubRun " << subrunNum << "\n";
    std::cout << "  Events processed        : " << _acc.nEvents             << "\n";
    std::cout << "  -- Primary estimator -----------------------------------\n";
    std::cout << "  Avg recoNPOT [primary]  : " << avgRecoNPOT_primary      << "\n";
    std::cout << "  Avg recoNPOT [trkHits]  : " << avgRecoNPOT_trackerHits  << "\n";
    std::cout << "  Avg recoNPOT [caloD1]   : " << avgRecoNPOT_caloD1       << "\n";
    std::cout << "  Estimator diff (trk-D1) : " << estimatorDiff_            << "\n";
    std::cout << "  -- Detector observables --------------------------------\n";
    std::cout << "  Avg N(calo hits in D1)  : " << avgNCaloHitsD1_          << "\n";
    std::cout << "  Avg N(calo hits D0)     : " << _acc.sumNCaloHitsD0 / n  << "\n";
    std::cout << "  Avg N(calo hits)        : " << avgNCaloHits_            << "\n";
    std::cout << "  Avg calo energy (MeV)   : " << _acc.sumCaloEnergy  / n  << "\n";
    std::cout << "  Avg N(tracker hits)     : " << avgNTrackerHits_         << "\n";
    std::cout << "  Avg N(CAPHRI hits)      : " << _acc.sumNCaphriHits / n  << "\n";
    std::cout << "  Avg N(DF proton TCs)    : " << _acc.sumNProtonsDF  / n  << "\n";
    std::cout << "  -- DQM Status ------------------------------------------\n";
    std::cout << "  STATUS                  : " << statusString(status)      << "\n";

    if(status != SubRunStatus::OK) {
      std::cout << "  Triggered checks:\n";
      if(avgRecoNPOT_primary     < _minPOTthreshold  ) std::cout << "    [FLAG] avg recoNPOT below minPOTthreshold (" << _minPOTthreshold << ")\n";
      if(avgNCaloHits_            < _minCaloHits     ) std::cout << "    [FLAG] avg nCaloHits below minCaloHits (" << _minCaloHits << ")\n";
      if(avgNTrackerHits_         < _minTrackerHits  ) std::cout << "    [FLAG] avg nTrackerHits below minTrackerHits (" << _minTrackerHits << ")\n";
      if(avgRecoNPOT_primary     > _maxPOTthreshold  ) std::cout << "    [WARN] avg recoNPOT above maxPOTthreshold (" << _maxPOTthreshold << ")\n";
      if(avgNCaloHits_           > _maxCaloHits      ) std::cout << "    [WARN] avg nCaloHits above maxCaloHits (" << _maxCaloHits << ")\n";
      if(avgNTrackerHits_         > _maxTrackerHits  ) std::cout << "    [WARN] avg nTrackerHits above maxTrackerHits (" << _maxTrackerHits << ")\n";
      if(std::abs(estimatorDiff_) > _maxEstimatorDiff) std::cout << "    [WARN] estimator diff above maxEstimatorDiff (" << _maxEstimatorDiff << ")\n";
    }
    std::cout << "===================================================\n\n";

    if(_diagLevel > 1) {
      std::cout << "  Total counts by object type:\n";
      for(const auto& entry : _counter_by_object) {
        std::cout << "    " << entry.first << " : " << entry.second << "\n";
      }
    }

    _acc.reset();
    _subrunGraphIndex++;
  }
 
} // end namespace mu2e

DEFINE_ART_MODULE(mu2e::LumiDQM)
