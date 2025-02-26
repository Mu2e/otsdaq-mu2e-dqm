//
// An EDAnalyzer module that reads the Trigger Info
//
// Original author G. Pezzullo
//
#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art/Framework/Principal/Provenance.h"
#include "art/Framework/Principal/Run.h"
#include "art/Framework/Principal/Selector.h"
#include "art/Framework/Principal/SubRun.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art/Framework/Services/System/TriggerNamesService.h"
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"
#include "artdaq/ArtModules/ArtdaqSharedMemoryServiceInterface.h"
#include "artdaq/DAQdata/Globals.hh"
#include "canvas/Persistency/Common/TriggerResults.h"
#include "cetlib_except/exception.h"
#include "fhiclcpp/ParameterSet.h"
#include "fhiclcpp/ParameterSetRegistry.h"

#include "messagefacility/MessageLogger/MessageLogger.h"
// #include "canvas/Utilities/InputTag.h"
#include "Offline/BFieldGeom/inc/BFieldManager.hh"
#include "Offline/GeometryService/inc/DetectorSystem.hh"
#include "Offline/GeometryService/inc/GeomHandle.hh"
#include "Offline/TrackerGeom/inc/Tracker.hh"

// Dataproducts
#include "Offline/DataProducts/inc/GenVector.hh"
#include "Offline/GlobalConstantsService/inc/GlobalConstantsHandle.hh"
#include "Offline/GlobalConstantsService/inc/ParticleDataList.hh"
#include "Offline/RecoDataProducts/inc/CaloCluster.hh"
#include "Offline/RecoDataProducts/inc/CaloDigi.hh"
#include "Offline/RecoDataProducts/inc/CaloTrigSeed.hh"
#include "Offline/RecoDataProducts/inc/ComboHit.hh"
#include "Offline/RecoDataProducts/inc/HelixSeed.hh"
#include "Offline/RecoDataProducts/inc/KalSeed.hh"
#include "Offline/RecoDataProducts/inc/StrawDigi.hh"
#include "Offline/RecoDataProducts/inc/TriggerInfo.hh"
#include "Offline/RecoDataProducts/inc/TrkQual.hh"

// Utilities
#include "Offline/Mu2eUtilities/inc/HelixTool.hh"
#include "Offline/Mu2eUtilities/inc/TriggerResultsNavigator.hh"

// ROOT
#include "TH1F.h"
#include "TH2F.h"

#include <cmath>
// #include <iostream>
#include <sstream>
#include <string>
// #include <map>
#include <vector>

namespace mu2e
{

class ReadTriggerCounts : public art::EDAnalyzer
{
  public:
	unordered_map<string, int> triggerStreamCounts;

	enum
	{
		kNTrigInfo     = 40,
		kNTrackTrig    = 40,
		kNTrackTrigVar = 50,
		kNHelixTrig    = 40,
		kNHelixTrigVar = 130,
		kNCaloCalib    = 5,
		kNCaloCalibVar = 30,
		kNCaloOnly     = 5,
		kNCaloOnlyVar  = 30,
		kNOcc          = 100,
		kNOccVar       = 100
	};

	struct trigInfo_
	{
		int         counts;
		int         exclusive_counts;
		std::string label;

		trigInfo_() : counts(0), exclusive_counts(0), label("") {}
	};

	explicit ReadTriggerCounts(fhicl::ParameterSet const& pset);
	///////////////////////////////////////////////
	virtual ~ReadTriggerCounts() = default;

	// virtual void beginJob();
	// virtual void endJob();
	virtual void endSubRun(const art::SubRun& sr);

	// This is called for each event.
	virtual void analyze(const art::Event& e);
	virtual void beginRun(const art::Run& run);
	void findTrigIndex(std::vector<trigInfo_>& Vec, std::string& ModuleLabel, int& Index);
	void findCorrelatedEvents(std::vector<string>& VecLabels, double& NCorrelated);
	void evalTriggerRate();
	bool goodTrkTanDip(const mu2e::KalSeed* Ks);

  private:
	int                      _diagLevel;
	size_t                   _nMaxTrig;
	int                      _nTrackTrig;
	int                      _nCaloTrig;
	int                      _nCaloCalibTrig;
	std::vector<std::string> _trigPaths;
	art::InputTag            _trigAlgTag;
	art::InputTag            _sdTag;
	art::InputTag            _chTag;
	art::InputTag            _cdTag;
	art::InputTag            _evtWeightTag;
	art::InputTag            _hsCprTag;
	art::InputTag            _hsTprTag;
	art::InputTag            _ksTag;
	art::InputTag            _trkQualTag;
	art::InputTag            _vdTag;

	double              _duty_cycle;
	string              _processName;
	std::vector<size_t> _effBits;
	float               _trkMinTanDip;
	float               _trkMaxTanDip;
	float               _trkMaxD0;
	float               _trkMinMVA;
	float               _nProcess;
	double              _bz0;

	double _nPOT;

	std::vector<trigInfo_> _trigAll;
	std::vector<trigInfo_> _trigFinal;
	std::vector<trigInfo_> _trigCaloOnly;
	std::vector<trigInfo_> _trigCaloCalib;
	std::vector<trigInfo_> _trigTrack;
	std::vector<trigInfo_> _trigHelix;
	std::vector<trigInfo_> _trigEvtPS;

	const mu2e::Tracker* _tracker;

	// the following pointer is needed to navigate the MC truth info of the
	// strawHits
	const mu2e::ComboHitCollection*  _chcol;
	const art::Event*                _event;
	const mu2e::HelixSeedCollection* _hsCprCol;
	const mu2e::HelixSeedCollection* _hsTprCol;

	float _minPOT, _maxPOT;
};

ReadTriggerCounts::ReadTriggerCounts(fhicl::ParameterSet const& pset)
    : art::EDAnalyzer(pset)
    ,

    _nMaxTrig(pset.get<size_t>("nPathIDs", 200))
    , _nTrackTrig(pset.get<size_t>("nTrackTriggers", 4))
    , _nCaloTrig(pset.get<size_t>("nCaloTriggers", 4))
    , _nCaloCalibTrig(pset.get<size_t>("nCaloCalibTriggers", 4))
    , _trigPaths(pset.get<std::vector<std::string>>("triggerPathsList"))
    , _sdTag(pset.get<art::InputTag>("strawDigiCollection", "makeSD"))
    , _chTag(pset.get<art::InputTag>("comboHitCollection", "TTmakeSH"))
    , _cdTag(pset.get<art::InputTag>("caloDigiCollection", "CaloDigiFromShower"))
    , _evtWeightTag(pset.get<art::InputTag>("protonBunchIntensity", "PBISim"))
    , _hsCprTag(pset.get<art::InputTag>("cprHelixSeedCollection",
                                        "CalHelixFinderDe:Positive"))
    ,  // , "KFFDeMHPar")),
    _hsTprTag(pset.get<art::InputTag>("tprHelixSeedCollection",
                                      "HelixFinderDe:Positive"))
    ,  // , "KFFDeMHPar")),
    _ksTag(pset.get<art::InputTag>("kalSeedCollection", "KFFDeMHPar"))
    , _trkQualTag(pset.get<art::InputTag>("trackQualCollection", "TrkQualDeMHPar"))
    , _vdTag(pset.get<art::InputTag>("vdStepPoints", "NOTNOW"))
    ,  // , "compressDigiMCs:virtualdetector")),
    _duty_cycle(pset.get<float>("dutyCycle", 1.))
    , _processName(pset.get<string>("processName", "globalTrigger"))
    , _trkMinTanDip(pset.get<float>("trkMinTanDip", 0.5))
    , _trkMaxTanDip(pset.get<float>("trkMaxTanDip", 1.))
    , _trkMaxD0(pset.get<float>("trkMaxD0", 100.))
    , _trkMinMVA(pset.get<float>("trkMinMVA", 0.8))
    , _nProcess(pset.get<float>("nEventsProcessed", 1.))
    , _minPOT(1e6)
    , _maxPOT(4e8)
{
	art::ServiceHandle<ArtdaqSharedMemoryServiceInterface> shm;
	_trigAll.resize(_nMaxTrig);
	_trigFinal.resize(_nMaxTrig);
	_trigCaloOnly.resize(_nMaxTrig);
	_trigCaloCalib.resize(_nMaxTrig);
	_trigTrack.resize(_nMaxTrig);
	_trigHelix.resize(_nMaxTrig);
	_trigEvtPS.resize(_nMaxTrig);
}

//================================================================
void ReadTriggerCounts::beginRun(const art::Run& run)
{
	// get bfield
	GeomHandle<BFieldManager>  bfmgr;
	GeomHandle<DetectorSystem> det;
	CLHEP::Hep3Vector vpoint_mu2e = det->toMu2e(CLHEP::Hep3Vector(0.0, 0.0, 0.0));
	_bz0                          = bfmgr->getBField(vpoint_mu2e).z();

	mu2e::GeomHandle<mu2e::Tracker> th;
	_tracker = th.get();
}

void ReadTriggerCounts::endSubRun(const art::SubRun& sr) {}

bool ReadTriggerCounts::goodTrkTanDip(const mu2e::KalSeed* Ks)
{
	const mu2e::KalSegment* kSeg   = &(Ks->segments().at(0));
	float                   tanDip = kSeg->helix().tanDip();
	if((tanDip > _trkMinTanDip) && (tanDip < _trkMaxTanDip))
		return true;

	return false;
}

//--------------------------------------------------------------------------------

void ReadTriggerCounts::analyze(const art::Event& event)
{
	// get the TriggerResult
	std::ostringstream oss;
	oss << "TriggerResults::" << _processName;
	art::InputTag const tag{oss.str()};
	auto const          trigResultsH = event.getValidHandle<art::TriggerResults>(tag);
	const art::TriggerResults* trigResults = trigResultsH.product();
	TriggerResultsNavigator    trigNavig(trigResults);

	// fill the histogram with the trigger bits
	//     for (unsigned i=0; i<trigResults->size(); ++i){
	bool hasNotPassedTrigger = true;

	metricMan->sendMetric(
	    "TriggerCounts.TotalEvents", 1, "events", 2, artdaq::MetricMode::Accumulate);
	metricMan->sendMetric("TriggerCounts.TotalEventsCumulative",
	                      1,
	                      "events",
	                      2,
	                      artdaq::MetricMode::LastPoint);
	for(unsigned int i = 0; i < trigNavig.getTrigPaths().size(); ++i)
	{
		//      if (trigResults->accept(i)){
		std::string path =
		    trigNavig.getTrigPathName(i);  // incorporates the reconstruction algorithm
		if(trigNavig.accepted(path))
		{
			triggerStreamCounts["TriggerCounts." + path]++;
			metricMan->sendMetric(
			    "TriggerCounts." + path, 1, "events", 2, artdaq::MetricMode::Accumulate);
			metricMan->sendMetric("TriggerCounts." + path + "Cumulative",
			                      1,
			                      "events",
			                      2,
			                      artdaq::MetricMode::LastPoint);
			// Avoid double counting for total passed: has this event passed a trigger
			// stream previously?
			if(hasNotPassedTrigger)
			{
				triggerStreamCounts["TriggerCounts.TotalAccepted"]++;
				metricMan->sendMetric("TriggerCounts.TotalPassedTrigger",
				                      1,
				                      "events",
				                      2,
				                      artdaq::MetricMode::Accumulate);
				metricMan->sendMetric("TriggerCounts.TotalPassedTriggerCumulative",
				                      1,
				                      "events",
				                      2,
				                      artdaq::MetricMode::LastPoint);
				hasNotPassedTrigger = false;
			}
		}
	}
}

}  // namespace mu2e

DEFINE_ART_MODULE(mu2e::ReadTriggerCounts)
