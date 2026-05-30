////////////////////////////////////////////////////////////////////////////////////
// CaloDigiDQM_module.cc
//
// Mu2e calorimeter DQM analyzer.
//
// Responsibilities:
//   - read CaloDigi objects
//   - map offline SiPM IDs to electronics IDs via CaloDAQMap
//   - Prebook the expected board/channel ROOT structure from CaloDAQMap at the start
//     of the first event, while still tracking which channels actually appear in the input data.
//   - fill ROOT histograms for global, disk, board, channel, and laser monitoring
//   - optionally stream selected histograms to otsdaq via HistoSender
//
// Maintainer notes:
//   - boardID is a global board index across both disks
//   - board 160 is treated as the dedicated laser board
//   - channelIndex(...) returns a compact global index for regular channels only
//   - activeRegularChannels_ and activeLaserChannels_ contain channels seen in data,
//     not all prebooked channels
//   - disk maps are always saved to ROOT; enableDiskMaps controls streaming only
//   - reference histograms are optional; missing reference files or objects do not
//     stop the module
//   - live waveform streaming is update-driven to reduce network load
//   - live waveform representatives are selected by highest amplitude per regular SiPM
//     and per laser channel
//   - pair/asymmetry calculations use all same-event regular SiPM candidates and select the
//     best time-matched even/odd SiPM pair
//   - normalized occupancy histograms are streamed; raw occupancy histograms are
//     still saved to the ROOT file
////////////////////////////////////////////////////////////////////////////////////
#include "Offline/CaloConditions/inc/CaloDAQMap.hh"
#include "Offline/ProditionsService/inc/ProditionsHandle.hh"

#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"

#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"

#include "canvas/Utilities/InputTag.h"
#include "messagefacility/MessageLogger/MessageLogger.h"

#include "otsdaq-mu2e/ArtModules/HistoSender.hh"

#include "Offline/CaloVisualizer/inc/THMu2eCaloDisk.hh"
#include "Offline/DataProducts/inc/CaloSiPMId.hh"
#include "Offline/RecoDataProducts/inc/CaloDigi.hh"

#include "fhiclcpp/types/Atom.h"
#include "fhiclcpp/types/Sequence.h"
#include "fhiclcpp/types/Table.h"

#include "TFile.h"
#include "TGraph.h"
#include "TH1.h"
#include "TH1F.h"
#include "TH1I.h"
#include "TH2.h"
#include "TH2F.h"
#include "TH2I.h"
#include "TProfile.h"
#include "TProfile2D.h"
#include "TString.h"

#include "cetlib_except/exception.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace mu2e
{
namespace
{
TString channelLabel(int boardID, int chanID, int rawId, int sipmId)
{
	return Form("B%03d C%02d (raw: %d, offline: %d)", boardID, chanID, rawId, sipmId);
}

void styleStatusBar(TH1* h)
{
	if(!h)
		return;

	h->SetStats(0);
	h->SetMinimum(0.0);
	h->SetMaximum(1.05);
	h->SetFillColor(kGreen + 2);
	h->SetLineColor(kBlack);
	h->SetLineWidth(2);
	h->SetBarWidth(0.80);
	h->SetBarOffset(0.10);
	h->SetOption("BAR");
	h->GetXaxis()->SetLabelSize(0.045);
	h->GetYaxis()->SetTitleOffset(1.25);
	h->GetYaxis()->SetLabelSize(0.040);
}

void styleStatusProfile(TProfile* h)
{
	if(!h)
		return;

	h->SetStats(0);
	h->SetMinimum(0.0);
	h->SetMaximum(1.05);
	h->SetMarkerStyle(20);
	h->SetMarkerSize(0.7);
	h->SetLineWidth(3);
	h->SetLineColor(kBlue + 1);
	h->SetMarkerColor(kBlue + 1);
	h->SetOption("P E1");
	h->GetXaxis()->SetLabelSize(0.040);
	h->GetYaxis()->SetLabelSize(0.040);
	h->GetYaxis()->SetTitleOffset(1.25);
}

void styleIssueCounts(TH1* h)
{
	if(!h)
		return;

	h->SetStats(0);
	h->SetFillColor(kOrange + 1);
	h->SetLineColor(kBlack);
	h->SetLineWidth(2);
	h->SetBarWidth(0.80);
	h->SetBarOffset(0.10);
	h->SetOption("BAR");
	h->GetXaxis()->SetLabelSize(0.045);
	h->GetYaxis()->SetTitleOffset(1.25);
}

void styleIssueMap(TH2* h)
{
	if(!h)
		return;

	h->SetStats(0);
	h->SetOption("COLZ");
	h->GetXaxis()->SetLabelSize(0.040);
	h->GetYaxis()->SetLabelSize(0.045);
	h->GetZaxis()->SetTitle("Issue count");
	h->GetZaxis()->SetTitleOffset(1.20);
}

TH1F* bookDist(art::TFileDirectory& dir,
               const char*          name,
               const char*          title,
               int                  bins,
               double               xmin,
               double               xmax,
               const char*          xTitle)
{
	auto* h = dir.make<TH1F>(name, title, bins, xmin, xmax);
	h->GetXaxis()->SetTitle(xTitle);
	h->GetYaxis()->SetTitle("Count");
	return h;
}
}  // namespace

class CaloDigiDQM : public art::EDAnalyzer
{
  public:
	struct Config
	{
		// otsdaq destination and top-level namespace for streamed histograms
		fhicl::Atom<std::string> address{fhicl::Name("address"),
		                                 "mu2e-dl-01-data.fnal.gov"};
		fhicl::Atom<int>         port{fhicl::Name("port"), 6000};
		fhicl::Atom<std::string> moduleTag{fhicl::Name("moduleTag"), "CaloDigiDQM"};
		fhicl::Atom<bool>        sendHists{fhicl::Name("sendHists"), false};

		// Event cadence for streaming
		// freqDQM controls summary histograms
		// freqWaveforms controls live waveform streaming
		fhicl::Atom<int> freqDQM{fhicl::Name("freqDQM"), 100};
		fhicl::Atom<int> freqWaveforms{fhicl::Name("freqWaveforms"),
		                               0};  // 0 = disable waveform streaming

		// art input tag label for the CaloDigiCollection
		fhicl::Atom<std::string> caloDigiModuleLabel{fhicl::Name("caloDigiModuleLabel"),
		                                             "CaloDigisFromDTCEvents"};

		// Disk maps are always saved to the ROOT file.
		// This flag controls disk-map streaming only.
		fhicl::Atom<bool> enableDiskMaps{fhicl::Name("enableDiskMaps"), false};

		// Disk map streaming selection only, examples {"asym"} or {"asym", "sum"}.
		// Disk maps are streamed less frequently than summary histograms.
		fhicl::Sequence<std::string> diskCombines{fhicl::Name("diskCombines"),
		                                          std::vector<std::string>{"asym"}};

		// Optional reference ROOT file for comparison histograms.
		fhicl::Atom<bool>        useReferenceFile{fhicl::Name("useReferenceFile"), false};
		fhicl::Atom<std::string> referenceFile{fhicl::Name("referenceFile"),
		                                       "reference.root"};
	};

	explicit CaloDigiDQM(const art::EDAnalyzer::Table<Config>& config);
	void analyze(art::Event const& event) override;
	void endJob() override;

  private:
	// -----------------------
	// Disk-map streaming cadence
	// -----------------------
	// Disk maps are streamed less frequently than summary histograms.
	// The disk-map period is computed as freqDQM + kDiskMapsExtraPeriod.
	// Example: freqDQM=100 -> disk maps every 200 events.
	static constexpr int kDiskMapsExtraPeriod = 100;

	// -----------------------
	// Map mode infrastructure
	// -----------------------
	enum class MapMode
	{
		Amp,
		Sum,
		Asym,
		Baseline,
		RMS
	};

	static MapMode parseMode(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
			return std::tolower(c);
		});

		if(s == "sum")
			return MapMode::Sum;
		if(s == "asym")
			return MapMode::Asym;
		if(s == "baseline")
			return MapMode::Baseline;
		if(s == "rms")
			return MapMode::RMS;
		if(s == "amp")
			return MapMode::Amp;

		throw cet::exception("BADCONFIG")
		    << "CaloDigiDQM: unknown diskCombines entry '" << s
		    << "'. Allowed values are: amp, sum, asym, baseline, rms.";
	}

	// Short suffix used for object names and streaming groups.
	static const char* modeSuffix(MapMode m)
	{
		switch(m)
		{
		case MapMode::Amp:
			return "Amp";
		case MapMode::Sum:
			return "Sum";
		case MapMode::Asym:
			return "Asym";
		case MapMode::Baseline:
			return "Baseline";
		case MapMode::RMS:
			return "RMS";
		}

		throw cet::exception("BADCONFIG")
		    << "CaloDigiDQM: invalid MapMode in modeSuffix.";
	}

	// Centralize titles and display options for disk maps.
	void setDiskMapTitles(mu2e::THMu2eCaloDisk* h, int disk, MapMode mode)
	{
		if(!h)
			return;

		const char* ztitle = "Value [ADC]";
		const char* main   = "SiPM value";

		switch(mode)
		{
		case MapMode::Amp:
			main   = "Mean SiPM amplitude (peak - baseline)";
			ztitle = "Mean Amplitude [ADC]";
			break;
		case MapMode::Sum:
			main   = "Mean Crystal sum (L+R) amplitude";
			ztitle = "Mean L+R [ADC]";
			break;
		case MapMode::Asym:
			main   = "Mean Crystal Asymmetry (L-R)/(L+R)";
			ztitle = "Mean Asymmetry";
			h->SetMinimum(-1.0);
			h->SetMaximum(1.0);
			break;
		case MapMode::Baseline:
			main   = "Mean SiPM baseline";
			ztitle = "Mean Baseline [ADC]";
			break;
		case MapMode::RMS:
			main   = "Mean SiPM baseline RMS";
			ztitle = "Mean RMS [ADC]";
			break;
		}

		h->SetTitle(Form("Disk %d - %s", disk, main));
		h->GetZaxis()->SetTitle(ztitle);
		h->SetOption("COLZ L");
		h->SetStats(0);
	}

	// -----------------------
	// Geometry / encoding
	// -----------------------
	static constexpr int kNDisks           = 2;
	static constexpr int kBoardsPerDisk    = 80;
	static constexpr int kChannelsPerBoard = 20;
	static constexpr int kChannelsPerDisk  = kBoardsPerDisk * kChannelsPerBoard;
	static constexpr int kUnmappedRawId    = 9999;

	static constexpr int kLaserBoardID  = 160;
	static constexpr int kLaserChannels = kChannelsPerBoard;

	static constexpr int kTotalBoards       = kNDisks * kBoardsPerDisk;    // 160
	static constexpr int kTotalChannels     = kNDisks * kChannelsPerDisk;  // 3200
	static constexpr int kSparseStride      = 100;
	static constexpr int kMaxEncodedBoardID = kLaserBoardID;  // include board 160

	static int boardMinForDisk(int disk) { return disk * kBoardsPerDisk; }

	static bool validRegularChannel(int disk, int boardID, int chanID)
	{
		if(disk < 0 || disk >= kNDisks)
			return false;
		if(chanID < 0 || chanID >= kChannelsPerBoard)
			return false;

		const int bmin = boardMinForDisk(disk);
		const int bmax = bmin + kBoardsPerDisk;
		return boardID >= bmin && boardID < bmax;
	}

	static int encodeChannel(int disk, int boardID, int chanID)
	{
		const int bmin = boardMinForDisk(disk);
		return (boardID - bmin) * kChannelsPerBoard + chanID;
	}

	static int encodeSparse(int boardID, int chanID)
	{
		return boardID * kSparseStride + chanID;
	}

	static int encodeDense(int boardID, int chanID)
	{
		return boardID * kChannelsPerBoard + chanID;
	}

	struct EncodedAxisConfig
	{
		int    nBins;
		double xMin;
		double xMax;
	};

	static EncodedAxisConfig axisSparseGlobal()
	{
		return EncodedAxisConfig{
		    kMaxEncodedBoardID * kSparseStride + kChannelsPerBoard,
		    0.0,
		    double(kMaxEncodedBoardID * kSparseStride + kChannelsPerBoard)};
	}

	static EncodedAxisConfig axisDenseGlobal()
	{
		return EncodedAxisConfig{(kMaxEncodedBoardID + 1) * kChannelsPerBoard,
		                         0.0,
		                         double((kMaxEncodedBoardID + 1) * kChannelsPerBoard)};
	}

	static int boardIndex(int disk, int boardID)
	{
		const int local = boardID - boardMinForDisk(disk);  // 0 to 79
		return disk * kBoardsPerDisk + local;               // 0 to 159
	}

	static int channelIndex(int disk, int boardID, int chanID)
	{
		return disk * kChannelsPerDisk +
		       encodeChannel(disk, boardID, chanID);  // 0 to 3199
	}

	static int diskFromCidx(int cidx) { return cidx / kChannelsPerDisk; }
	static int encodedFromCidx(int cidx) { return cidx % kChannelsPerDisk; }
	static int boardLocalFromEncoded(int enc) { return enc / kChannelsPerBoard; }
	static int chanFromEncoded(int enc) { return enc % kChannelsPerBoard; }

	static int boardIdFromDiskAndLocal(int disk, int blocal)
	{
		return boardMinForDisk(disk) + blocal;
	}

	// -----------------------
	// Skip / issue instrumentation
	// -----------------------
	enum class SkipReason : int
	{
		BadSipmId = 0,
		UnmappedRawId,
		OutOfRangeSipmId,
		RawIdNegative,
		DiskOutOfRange,
		PeakPosOutOfRange,
		NonFiniteBaselineOrRms,
		TinyDenomAsym,
		PairDiskMismatch,
		Count
	};

	enum class ShapeClass : int
	{
		Good = 0,
		Saturated,
		EdgePeak,
		LongTail,
		Undershoot,
		Noisy,
		Negative,
		Count
	};

	// Per-channel quality metrics use the convention:
	//   1.0 = good / passing
	//   0.0 = bad / failing
	// Intermediate values are allowed for averaged profile bins.
	enum class QualityMetric : int
	{
		Seen = 0,
		AmpPositive,
		RMSOK,
		SaturationOK,
		SNRGood,
		PeakTimeOK,
		PairOK,
		WaveformHealth,
		Count
	};

	static const char* qualityMetricLabel(QualityMetric m)
	{
		switch(m)
		{
		case QualityMetric::Seen:
			return "Seen";
		case QualityMetric::AmpPositive:
			return "AmpPositive";
		case QualityMetric::RMSOK:
			return "RMSOK";
		case QualityMetric::SaturationOK:
			return "SaturationOK";
		case QualityMetric::SNRGood:
			return "SNRGood";
		case QualityMetric::PeakTimeOK:
			return "PeakTimeOK";
		case QualityMetric::PairOK:
			return "PairOK";
		case QualityMetric::WaveformHealth:
			return "WaveformHealth";
		case QualityMetric::Count:
			break;
		}
		return "Unknown";
	}

	static const char* shapeClassLabel(ShapeClass c)
	{
		switch(c)
		{
		case ShapeClass::Good:
			return "Good";
		case ShapeClass::Saturated:
			return "Saturated";
		case ShapeClass::EdgePeak:
			return "EdgePeak";
		case ShapeClass::LongTail:
			return "LongTail";
		case ShapeClass::Undershoot:
			return "Undershoot";
		case ShapeClass::Noisy:
			return "Noisy";
		case ShapeClass::Negative:
			return "Negative";
		case ShapeClass::Count:
			break;
		}
		return "Unknown";
	}

	enum class IssueType : int
	{
		Sat = 0,
		EdgePeak,
		HighRMS,
		LowSNR,
		NegAmp,
		BadShape,
		PairMiss,
		PairDiskMismatch,
		Count
	};

	static const char* issueLabel(IssueType t)
	{
		switch(t)
		{
		case IssueType::Sat:
			return "Sat";
		case IssueType::EdgePeak:
			return "EdgePeak";
		case IssueType::HighRMS:
			return "HighRMS";
		case IssueType::LowSNR:
			return "LowSNR";
		case IssueType::NegAmp:
			return "NegAmp";
		case IssueType::BadShape:
			return "BadShape";
		case IssueType::PairMiss:
			return "PairMiss";
		case IssueType::PairDiskMismatch:
			return "PairDiskMismatch";
		case IssueType::Count:
			break;
		}
		return "Unknown";
	}

	static const char* skipLabel(SkipReason r)
	{
		switch(r)
		{
		case SkipReason::BadSipmId:
			return "BadSipmId";
		case SkipReason::UnmappedRawId:
			return "UnmappedRawId";
		case SkipReason::OutOfRangeSipmId:
			return "OutOfRangeSipmId";
		case SkipReason::RawIdNegative:
			return "RawIdNegative";
		case SkipReason::DiskOutOfRange:
			return "DiskOutOfRange";
		case SkipReason::PeakPosOutOfRange:
			return "PeakPosOutOfRange";
		case SkipReason::NonFiniteBaselineOrRms:
			return "NonFiniteBaselineOrRms";
		case SkipReason::TinyDenomAsym:
			return "TinyDenomAsym";
		case SkipReason::PairDiskMismatch:
			return "PairDiskMismatch";
		case SkipReason::Count:
			break;
		}
		return "Unknown";
	}

	std::array<uint64_t, (int)SkipReason::Count> skipCounts_{};
	TH1I*                                        h_skip_reason_{nullptr};

	inline void recordSkip(SkipReason r)
	{
		skipCounts_[(int)r]++;
		if(h_skip_reason_)
			h_skip_reason_->Fill((int)r);
	}

	// -----------------------
	// Constants / thresholds
	// -----------------------
	static constexpr int kWaveformNBins       = 64;
	static constexpr int kWaveformSizeHistMax = 200;

	static constexpr int      kEventBlockSize   = 100;
	static constexpr int      kEventTrendNBins  = 4000;
	static constexpr uint64_t kHitsAverageBlock = 1000;

	static constexpr int kExpectedPeakTick = 30;
	static constexpr int kSaturationAdc    = 4090;
	static constexpr int kBaselineSamples  = 5;

	static constexpr double kMaxGoodRms        = 20.0;
	static constexpr double kMinGoodSnr        = 5.0;
	static constexpr double kMaxGoodShapeScore = 1.0;
	static constexpr int    kMinShapeSamples   = 5;
	static constexpr double kMinDenomForAsym   = 5.0;

	static constexpr double kPairMissHealth   = 0.75;
	static constexpr double kLongTailFraction = 0.50;
	static constexpr double kUndershootSigma  = 5.0;

	static constexpr int    kEvtDigisHistMax = 1000;
	static constexpr double kAmpHistMin      = -200.0;
	static constexpr double kAmpHistMax      = 2000.0;

	static constexpr int kBoardTrendMerge = 10;
	static constexpr int kBoardTrendNBins = 400;

	static int boardTrendBin(int eventBlock)
	{
		return std::min(eventBlock / kBoardTrendMerge, kBoardTrendNBins - 1);
	}

	// Badness is a bounded heuristic score, not a probability.
	// Multiple issues can accumulate and are clamped to 1.0.
	static constexpr double kBadnessSaturation = 0.30;
	static constexpr double kBadnessEdgePeak   = 0.15;
	static constexpr double kBadnessHighRms    = 0.25;
	static constexpr double kBadnessLowSnr     = 0.20;
	static constexpr double kBadnessNegAmp     = 0.20;
	static constexpr double kBadnessBadShape   = 0.20;

	// -----------------------
	// Structured per-digi and per-event state
	// -----------------------
	struct WaveformSizeStats
	{
		uint32_t first{0}, last{0}, min{0}, max{0};
		uint32_t nSeen{0}, nMismatchToFirst{0}, nTransitions{0};
		uint32_t nTruncated{0}, nPadded{0};
	};

	struct DigiFeatures
	{
		int wfSize{0};
		int peakpos{-1};
		int nSat{0};

		float baseline{0.0f};
		float rms{0.0f};
		float ampRaw{0.0f};

		double amp{0.0};
		double timeResid{0.0};
		double shapeScore{0.0};

		ShapeClass shapeClass{ShapeClass::Good};
	};

	struct DigiAddress
	{
		int  sipmId{-1};
		int  rawId{-1};
		int  boardID{-1};
		int  chanID{-1};
		int  disk{-1};
		int  cidx{-1};
		int  encodedSparse{-1};
		int  encodedDense{-1};
		bool isLaser{false};
	};

	struct EventStats
	{
		int nDigis{0};

		int nSatWaveforms{0};
		int nEdgePeaks{0};
		int nHighRms{0};
		int nLowSnr{0};
		int nNegativeAmp{0};
		int nUnpairedSipms{0};
		int nPairDiskMismatches{0};

		double sumHealth{0.0};
		int    nHealthSamples{0};

		double sumRegularAmp{0.0};
		int    nRegularAmp{0};

		double sumLaserAmp{0.0};
		int    nLaserAmp{0};

		double meanLaserAmp() const
		{
			return nLaserAmp > 0 ? sumLaserAmp / (double)nLaserAmp : 0.0;
		}
	};

	struct SipmFeat
	{
		double amp{0.0};
		double baseline{0.0};
		double rms{0.0};
		double ampRaw{0.0};

		int    peakpos{-1};
		double timeResid{0.0};
		double snr{0.0};

		int rawId{-1};
		int disk{-1};
		int board{-1};
		int chan{-1};
		int cidx{-1};

		// Representative baseline-subtracted waveform used only for live waveform display.
		int                               wfN{0};
		std::array<float, kWaveformNBins> wfSub{};
	};

	// -----------------------
	// Configuration / constructor helpers
	// -----------------------
	void validateConfig() const;
	void loadReferenceFile();
	void createRootDirectories();
	void configureDiskMapModes(std::vector<std::string> const& rawStream);
	void allocateBuffers();
	void bookDiskMaps();
	void createHistoSender();
	void precomputeStreamPaths();
	void bookGlobalHistograms();

	// -----------------------
	// Event-processing helpers
	// -----------------------
	void beginEvent();
	void fillEventLevelCounters(EventStats const& stats,
	                            int               eventBlock,
	                            double            eventNumber);
	void accumulateHitsAverage(double eventNumber, int nHits);
	void flushHitsAveragePoint();

	void processDigi(CaloDigi const&   digi,
	                 CaloDAQMap const& calodaqconds,
	                 EventStats&       stats,
	                 int               eventBlock);

	bool decodeAddress(CaloDigi const&   digi,
	                   CaloDAQMap const& calodaqconds,
	                   DigiAddress&      addr);

	void processLaserDigi(CaloDigi const&    digi,
	                      DigiAddress const& addr,
	                      EventStats&        stats,
	                      int                eventBlock);

	void processRegularDigi(CaloDigi const&    digi,
	                        DigiAddress const& addr,
	                        EventStats&        stats,
	                        int                eventBlock);
	void processPairs(EventStats& stats, int eventBlock);

	// Live waveform representatives use the highest-amplitude digi per regular SiPM
	// and per laser channel. Pair/asymmetry calculations use all usable same-event
	// regular SiPM candidates and select the best time-matched even/odd pair separately.
	void queueRepWaveformsForStreaming();
	void queueRepLaserWaveformsForStreaming();

	bool pairCandidateSeen(int sid) const
	{
		return (sid >= 0 && sid < kMaxSipmIdForMaps_ &&
		        pairCandidateStamp_[(size_t)sid] == pairStamp_ &&
		        !pairCandidates_[(size_t)sid].empty());
	}

	void addPairCandidate(int sid, SipmFeat const& f);

	bool selectBestTimeMatchedPair(int       evenId,
	                               int       oddId,
	                               SipmFeat& bestEven,
	                               SipmFeat& bestOdd) const;

	double fillLaserNormalization(EventStats& stats, int eventBlock);
	void   fillDqmSummary(EventStats const& stats, int eventBlock, double meanLaserAmp);
	void   streamIfScheduled();

	void clearSummarySendQueues();
	void clearWaveformSendQueues();

	void fillIssue(int boardID, IssueType issue);
	void fillHealth(int boardID, double healthScore, EventStats& stats, int eventBlock);

	// Convert waveform-level issues into a bounded badness score.
	// The complementary value, 1 - badness, is stored as WaveformHealth.
	void fillWaveformHealth(int                 boardID,
	                        int                 chanID,
	                        DigiFeatures const& feat,
	                        EventStats&         stats,
	                        int                 eventBlock);

	static void styleQualityMatrix(TProfile2D* h);

	void fillQualityMetric(
	    int boardID, int chanID, QualityMetric metric, double value, int eventBlock);

	void fillBoardIssueMetric(int boardID, int chanID, IssueType issue);

	void updateNormalizedOccHistograms();

	// -----------------------
	// Feature extraction helpers
	// -----------------------
	template<class WaveformT>
	bool extractFeatures(WaveformT const& waveform,
	                     int              peakpos,
	                     int              boardID,
	                     int              chanID,
	                     int              eventBlock,
	                     DigiFeatures&    out);

	template<class WaveformT>
	static bool computeBaselineRms(WaveformT const& waveform,
	                               int              wfSize,
	                               float&           baseline,
	                               float&           rms)
	{
		if(wfSize <= 0)
			return false;

		const int nBase = std::min<int>(kBaselineSamples, wfSize);

		float sum   = 0.0f;
		float sumsq = 0.0f;

		for(int i = 0; i < nBase; ++i)
		{
			const float x = waveform[(size_t)i];
			sum += x;
			sumsq += x * x;
		}

		baseline            = sum / (float)nBase;
		const float mean_sq = sumsq / (float)nBase;
		rms = (mean_sq > baseline * baseline) ? std::sqrt(mean_sq - baseline * baseline)
		                                      : 0.0f;

		return std::isfinite(baseline) && std::isfinite(rms);
	}

	template<class WaveformT>
	double computePulseShapeScore(WaveformT const& waveform,
	                              int              wfSize,
	                              float            baseline,
	                              double           amp) const;

	template<class WaveformT>
	ShapeClass classifyWaveform(WaveformT const& waveform,
	                            int              wfSize,
	                            float            baseline,
	                            int              peakpos,
	                            double           rms,
	                            double           amp,
	                            int              nSat,
	                            double           shapeScore) const;

	template<class WaveformT>
	void fillGlobalWaveformDensity(WaveformT const& waveform, int wfSize)
	{
		if(!h_global_waveform_density_)
			return;

		const int n = std::min<int>(wfSize, h_global_waveform_density_->GetNbinsX());

		for(int i = 0; i < n; ++i)
		{
			const double y = (double)waveform[(size_t)i];
			h_global_waveform_density_->Fill(i, y);
		}

		waveformDensityUpdated_ = true;
	}

	// -----------------------
	// Pairing / representative digi helpers
	// -----------------------
	bool featSeen(int sid) const
	{
		return (sid >= 0 && sid < kMaxSipmIdForMaps_ &&
		        featStamp_[(size_t)sid] == pairStamp_);
	}

	void markFeat(int sid, SipmFeat const& f)
	{
		if(sid < 0 || sid >= kMaxSipmIdForMaps_)
			return;
		feat_[(size_t)sid]      = f;
		featStamp_[(size_t)sid] = pairStamp_;
	}

	bool paired(int crystalId) const
	{
		if(crystalId < 0 || (size_t)crystalId >= pairedStamp_.size())
			return true;
		return pairedStamp_[(size_t)crystalId] == pairStamp_;
	}

	void markPaired(int crystalId)
	{
		if(crystalId < 0 || (size_t)crystalId >= pairedStamp_.size())
			return;
		pairedStamp_[(size_t)crystalId] = pairStamp_;
	}

	void incMultiplicity(int sid)
	{
		if(sid < 0 || sid >= kMaxSipmIdForMaps_)
			return;

		const size_t idx = (size_t)sid;

		if(sipmMultiplicityStamp_[idx] != pairStamp_)
		{
			sipmMultiplicityStamp_[idx] = pairStamp_;
			sipmMultiplicity_[idx]      = 0u;
		}

		++sipmMultiplicity_[idx];
	}

	int getMultiplicity(int sid) const
	{
		if(sid < 0 || sid >= kMaxSipmIdForMaps_)
			return 0;

		const size_t idx = (size_t)sid;

		if(sipmMultiplicityStamp_[idx] != pairStamp_)
			return 0;

		return (int)sipmMultiplicity_[idx];
	}

	static bool betterRep(SipmFeat const& cand, SipmFeat const& cur)
	{
		if(cand.amp != cur.amp)
			return cand.amp > cur.amp;
		if(cand.ampRaw != cur.ampRaw)
			return cand.ampRaw > cur.ampRaw;
		return cand.cidx < cur.cidx;
	}

	template<class WaveformT>
	static void packRepWaveform(SipmFeat&        dst,
	                            WaveformT const& waveform,
	                            float            baseline,
	                            int              wfSize)
	{
		const int nLive = std::min<int>(wfSize, kWaveformNBins);

		dst.wfN = nLive;

		for(int i = 0; i < nLive; ++i)
			dst.wfSub[(size_t)i] = (float)waveform[(size_t)i] - baseline;

		for(int i = nLive; i < kWaveformNBins; ++i)
			dst.wfSub[(size_t)i] = 0.0f;
	}

	// -----------------------
	// Disk-map running mean
	// -----------------------
	static constexpr int kMaxSipmIdForMaps_    = 10000;  // Safety cap for vector sizing
	static constexpr int kMaxCrystalIdForMaps_ = (kMaxSipmIdForMaps_ + 1) / 2;

	static bool validIndexedSipmId(int sid)
	{
		return sid >= 0 && sid < kMaxSipmIdForMaps_;
	}

	bool     warnedOutOfRangeSipmId_{false};
	uint64_t nOutOfRangeSipmId_{0};

	void recordOutOfRangeSipmId(int sipmId)
	{
		recordSkip(SkipReason::OutOfRangeSipmId);
		++nOutOfRangeSipmId_;

		if(!warnedOutOfRangeSipmId_)
		{
			warnedOutOfRangeSipmId_ = true;
			mf::LogWarning("CaloDigiDQM")
			    << "Out-of-range sipmId=" << sipmId << " (cap=" << kMaxSipmIdForMaps_
			    << "). SiPM-indexed pairing, multiplicity, and disk-map accumulation "
			    << "will skip these digis.";
		}
	}

	void accDisk(MapMode m, int disk, int sipmId, double val)
	{
		if(disk < 0 || disk >= kNDisks)
			return;

		if(!validIndexedSipmId(sipmId))
		{
			recordOutOfRangeSipmId(sipmId);
			return;
		}

		const size_t midx = modeIndex(m);
		auto&        sumv = diskSum_[midx][disk];
		auto&        cntv = diskCnt_[midx][disk];

		sumv[(size_t)sipmId] += val;
		cntv[(size_t)sipmId] += 1u;
	}

	void refreshDiskMap(MapMode m)
	{
		const size_t midx = modeIndex(m);

		for(int disk = 0; disk < kNDisks; ++disk)
		{
			auto* h = (disk == 0) ? disk0Maps_[midx] : disk1Maps_[midx];
			if(!h)
				continue;

			h->Reset("ICESM");
			setDiskMapTitles(h, disk, m);

			auto& sumv = diskSum_[midx][disk];
			auto& cntv = diskCnt_[midx][disk];

			const size_t n = std::min(sumv.size(), cntv.size());
			for(size_t sipm = 0; sipm < n; ++sipm)
			{
				if(cntv[sipm] == 0u)
					continue;
				h->FillOffline((int)sipm, sumv[sipm] / (double)cntv[sipm]);
			}
		}
	}

	void refreshDiskMaps()
	{
		for(auto m : kAllModes)
			refreshDiskMap(m);
	}

	// -----------------------
	// Waveform cache / stats helpers
	// -----------------------
	inline void flushCachedWaveformToHist(TH1F* h, int cidx) const
	{
		if(!h)
			return;
		if(cidx < 0 || cidx >= kTotalChannels)
			return;
		if(!lastWfValid_[(size_t)cidx])
			return;

		for(int i = 0; i < kWaveformNBins; ++i)
			h->SetBinContent(i + 1, (double)lastWf_[(size_t)cidx][(size_t)i]);
	}

	void flushAllLiveWaveforms()
	{
		for(int cidx : activeRegularChannels_)
		{
			if(liveWf_[(size_t)cidx])
				flushCachedWaveformToHist(liveWf_[(size_t)cidx], cidx);
		}
	}

	void flushUpdatedLiveWaveforms()
	{
		for(int cidx : updatedRegularChannels_)
		{
			if(liveWaveformUpdated_[(size_t)cidx] && liveWf_[(size_t)cidx])
				flushCachedWaveformToHist(liveWf_[(size_t)cidx], cidx);
		}
	}

	inline void flushLaserCachedWaveformToHist(TH1F* h, int chanID) const
	{
		if(!h)
			return;
		if(chanID < 0 || chanID >= kLaserChannels)
			return;
		if(!laserLastWfValid_[(size_t)chanID])
			return;

		for(int i = 0; i < kWaveformNBins; ++i)
			h->SetBinContent(i + 1, (double)laserLastWf_[(size_t)chanID][(size_t)i]);
	}

	void flushAllLaserLiveWaveforms()
	{
		for(int chan : activeLaserChannels_)
		{
			if(laserLiveWf_[(size_t)chan])
				flushLaserCachedWaveformToHist(laserLiveWf_[(size_t)chan], chan);
		}
	}

	void flushUpdatedLaserLiveWaveforms()
	{
		for(int chan : updatedLaserChannels_)
		{
			if(laserLiveWaveformUpdated_[(size_t)chan] && laserLiveWf_[(size_t)chan])
				flushLaserCachedWaveformToHist(laserLiveWf_[(size_t)chan], chan);
		}
	}

	inline void updateWaveformStats(WaveformSizeStats& st, int wfSize)
	{
		const uint32_t sz = (uint32_t)wfSize;

		if(st.nSeen == 0)
			st.first = st.last = st.min = st.max = sz;
		else
		{
			if(sz != st.first)
				st.nMismatchToFirst++;
			if(sz != st.last)
				st.nTransitions++;
			if(sz < st.min)
				st.min = sz;
			if(sz > st.max)
				st.max = sz;
			st.last = sz;
		}

		st.nSeen++;
		if(sz > (uint32_t)kWaveformNBins)
			st.nTruncated++;
		else if(sz < (uint32_t)kWaveformNBins)
			st.nPadded++;
	}

	bool modeEnabled(MapMode m) const { return enabledModes_[modeIndex(m)]; }
	bool streamModeEnabled(MapMode m) const { return streamEnabledModes_[modeIndex(m)]; }

	// -----------------------
	// Booking helpers
	// -----------------------

	TH1F* ensureChannelDistBooked(
	    std::vector<TH1F*>&                                             storage,
	    std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards>& dirs,
	    int                                                             disk,
	    int                                                             boardID,
	    int                                                             chanID,
	    const char*                                                     suffix,
	    const char*                                                     titleBase,
	    int                                                             bins,
	    double                                                          xmin,
	    double                                                          xmax,
	    const char*                                                     xTitle);

	void ensureBaselineDistBooked(int disk, int boardID, int chanID);
	void ensureRmsDistBooked(int disk, int boardID, int chanID);
	void ensureMaxDistBooked(int disk, int boardID, int chanID);
	void ensureAsymDistBooked(int disk, int boardID, int chanID);

	static void addHist(std::vector<TH1*>& group, TH1* h)
	{
		if(h)
			group.push_back(h);
	}

	static void addGraph(std::vector<TGraph*>& group, TGraph* g)
	{
		if(g)
			group.push_back(g);
	}

	static void updateNormalizedHist(TH1* src, TH1* dst)
	{
		if(!src || !dst)
			return;

		dst->Reset("ICESM");
		dst->Add(src);

		const double integral = dst->Integral();
		if(integral > 0.0)
			dst->Scale(1.0 / integral);
	}

	static bool isLaserBoard(int boardID) { return boardID == kLaserBoardID; }

	void ensureLaserBoardBooked();
	void ensureLaserBaselineDistBooked(int chanID);
	void ensureLaserRmsDistBooked(int chanID);
	void ensureLaserMaxDistBooked(int chanID);
	void ensureLaserLiveWaveformBooked(int chanID, int rawId, int sipmId);

	template<class WaveformT>
	void ensureLaserFirstHitBooked(int              chanID,
	                               int              rawId,
	                               int              sipmId,
	                               WaveformT const& waveform,
	                               int              wfSize,
	                               float            baseline);

	void ensureBoardBooked(int disk, int boardID);
	void ensureLiveWaveformBooked(
	    int disk, int boardID, int chanID, int rawId, int sipmId);

	// Create the expected empty ROOT folder/histogram structure from CaloDAQMap.
	void prebookExpectedStructureFromCaloDAQMap(CaloDAQMap const& calodaqconds);

	template<class WaveformT>
	void ensureFirstHitBooked(int              disk,
	                          int              boardID,
	                          int              chanID,
	                          int              rawId,
	                          int              sipmId,
	                          WaveformT const& waveform,
	                          int              wfSize,
	                          float            baseline);

	// -----------------------
	// Data members
	// -----------------------
	std::vector<MapMode> streamModes_;

	art::InputTag caloDigiTag_;

	int         freqDQM_;
	int         freqWaveforms_;
	std::string address_;
	int         port_;
	std::string moduleTag_;
	bool        sendHists_;

	// True after the expected ROOT structure is prebooked from CaloDAQMap.
	bool prebookedExpectedStructure_{false};

	bool        waveformDensityUpdated_{false};
	std::string streamWaveformDensityPath_;

	std::unique_ptr<ots::HistoSender> histSender_;
	uint64_t                          eventCounter_{0};
	int                               histConsecutiveSendErrors_{0};
	uint64_t                          histTotalSendErrors_{0};
	static constexpr int              kMaxSendErrors_ = 10;

	bool enableDiskMaps_;

	// Reference histograms are optional.
	bool        useReferenceFile_;
	std::string referenceFile_;

	int nFillDisk0_{0}, nFillDisk1_{0}, nFillLaser_{0}, nFillMiss_{0};

	uint64_t nLaserAmpAccepted_{0};
	uint64_t nDetLaserRatioAccepted_{0};

	TH1F*     ref_h_occ_dense_{nullptr};
	TProfile* ref_h_base_dense_{nullptr};
	TProfile* ref_h_rms_dense_{nullptr};
	TProfile* ref_h_max_dense_{nullptr};
	TH1F*     ref_h_asym_{nullptr};

	TH1F*     ref_D0_B027_Occupancy_{nullptr};
	TProfile* ref_D0_B027_Baseline_{nullptr};
	TProfile* ref_D0_B027_RMS_{nullptr};
	TProfile* ref_D0_B027_Max_{nullptr};
	TH1F*     ref_D0_B027_C00_Waveform_{nullptr};

	struct BoardHists
	{
		TH1F*     occ{nullptr};
		TH1F*     occNorm{nullptr};
		TProfile* base{nullptr};
		TProfile* rms{nullptr};
		TProfile* max{nullptr};

		// Compact board-level diagnostics.
		TProfile2D* quality{nullptr};      // channel vs metric
		TH2I*       issue{nullptr};        // channel vs issue type
		TProfile2D* healthTrend{nullptr};  // coarse event block vs channel
	};

	// Additional DQM diagnostics.
	TH1I*     h_evt_digis_{nullptr};
	TProfile* h_digis_block_{nullptr};

	uint64_t nEvtDigisOverflow_{0};
	uint64_t nWaveformSizeOverflow_{0};
	uint64_t nAmpOverflow_{0};

	TH1F*     h_sat_samples_{nullptr};
	TProfile* h_sat_frac_board_{nullptr};

	TH2F*     h_amp_rms_{nullptr};
	TProfile* h_snr_board_{nullptr};

	TProfile* h_base_block_{nullptr};
	TProfile* h_rms_block_{nullptr};
	TProfile* h_amp_block_{nullptr};
	TProfile* h_laser_block_{nullptr};
	TProfile* h_amp_laser_block_{nullptr};

	TH2F* h_lr_corr_{nullptr};
	TH2F* h_sum_asym_{nullptr};

	TProfile* h_pair_ok_board_{nullptr};
	TH1I*     h_unpaired_evt_{nullptr};

	// Unlike feat_, pairCandidates_ stores all usable regular digis for each SiPM
	// in the current event. This is used for time-matched L/R pairing and asymmetry.
	std::vector<std::vector<SipmFeat>> pairCandidates_;
	std::vector<int>                   pairCandidateStamp_;
	std::vector<int>                   pairCandidateSipmIds_;

	TProfile2D* h_badness_board_channel_{nullptr};

	std::array<TProfile2D*, kNDisks> h_disk_quality_matrix_{};

	TProfile2D* h_time_resid_board_channel_{nullptr};
	TProfile*   h_time_resid_board_{nullptr};
	TH1F*       h_time_resid_dist_{nullptr};

	TH2I* h_shape_class_board_{nullptr};
	TH2I* h_shape_class_channel_{nullptr};

	// Topology DQM diagnostics.
	TH1I*     h_pair_raw_delta_{nullptr};
	TH2I*     h_pair_board_delta_{nullptr};
	TProfile* h_pair_miss_frac_block_{nullptr};

	TH1F*     h_pulse_shape_score_{nullptr};
	TProfile* h_pulse_shape_score_board_{nullptr};

	TProfile2D* h_amp_laser_board_channel_{nullptr};
	TProfile*   h_health_board_{nullptr};
	TProfile*   h_health_block_{nullptr};
	TH2I*       h_issue_board_{nullptr};

	TProfile* h_asym_board_{nullptr};
	TH2F*     h_peak_amp_{nullptr};

	// DQM summary panel.
	TH1F*     h_dqm_summary_{nullptr};
	TH1F*     h_dqm_issue_counts_{nullptr};
	TH1F*     h_dqm_run_counters_{nullptr};
	TProfile* h_dqm_summary_block_{nullptr};

	TH1F*   h_occ_board_{nullptr};
	TH1F*   h_occ_board_norm_{nullptr};
	TGraph* g_nhits_ewt_{nullptr};

	double   nhitsBlockSum_{0.0};
	double   eventNumberBlockSum_{0.0};
	uint64_t nhitsBlockCount_{0};

	TH2F* h_asym_chanid_{nullptr};

	std::array<BoardHists, kTotalBoards>                           boardH_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardHistDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardChanDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardBaselineDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardRmsDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardMaxDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardAsymDir_{};

	std::unique_ptr<art::TFileDirectory> laserDir_;
	std::unique_ptr<art::TFileDirectory> laserBoardHistDir_;
	std::unique_ptr<art::TFileDirectory> laserBoardChanDir_;
	std::unique_ptr<art::TFileDirectory> laserBaselineDir_;
	std::unique_ptr<art::TFileDirectory> laserRmsDir_;
	std::unique_ptr<art::TFileDirectory> laserMaxDir_;

	BoardHists laserBoardH_{};

	std::array<TH1F*, kLaserChannels>   laserLiveWf_{};
	std::array<TH1F*, kLaserChannels>   laserOneHitWf_{};
	std::array<uint8_t, kLaserChannels> laserOneHitSeen_{};
	std::array<uint8_t, kLaserChannels> laserOneHitSent_{};
	std::array<uint8_t, kLaserChannels> laserLiveWaveformUpdated_{};
	// Laser channels that have actually appeared in the input data.
	std::array<uint8_t, kLaserChannels> laserChannelSeen_{};

	std::array<WaveformSizeStats, kLaserChannels> laserWfStats_{};

	std::array<TH1F*, kLaserChannels> laserBaselineDist_{};
	std::array<TH1F*, kLaserChannels> laserRmsDist_{};
	std::array<TH1F*, kLaserChannels> laserMaxDist_{};

	std::array<std::array<float, kWaveformNBins>, kLaserChannels> laserLastWf_{};
	std::array<uint8_t, kLaserChannels>                           laserLastWfValid_{};

	std::array<SipmFeat, kLaserChannels> laserRep_{};
	std::array<uint8_t, kLaserChannels>  laserRepSeen_{};

	// Per-channel waveforms.
	std::vector<TH1F*>             liveWf_;
	std::vector<TH1F*>             oneHitWf_;
	std::vector<uint8_t>           oneHitSeen_;
	std::vector<uint8_t>           oneHitSent_;
	std::vector<uint8_t>           liveWaveformUpdated_;
	std::vector<WaveformSizeStats> wfStats_;
	std::vector<TH1F*>             h_baseline_dist_;
	std::vector<TH1F*>             h_rms_dist_;
	std::vector<TH1F*>             h_max_dist_;
	std::vector<TH1F*>             h_asym_dist_;

	// Regular channels that have actually appeared in the input data.
	std::vector<uint8_t> channelSeen_;

	// Channels/objects that appeared or changed during processing.
	std::vector<int> activeRegularChannels_;
	std::vector<int> activeLaserChannels_;
	std::vector<int> updatedBoards_;
	std::vector<int> updatedRegularChannels_;
	std::vector<int> updatedLaserChannels_;

	size_t pendingRegularFirstHits_{0};
	size_t pendingLaserFirstHits_{0};

	std::vector<uint8_t>                boardQueuedForSend_;
	std::vector<uint8_t>                regularQueuedForSend_;
	std::array<uint8_t, kLaserChannels> laserQueuedForSend_{};

	bool laserBoardUpdated_{false};

	// TFileService folders.
	std::unique_ptr<art::TFileDirectory> disk0Dir_;
	std::unique_ptr<art::TFileDirectory> disk1Dir_;
	std::unique_ptr<art::TFileDirectory> globalDir_;

	// Disk maps per mode.
	static constexpr size_t kNMapModes = 5;

	static constexpr size_t modeIndex(MapMode m) { return static_cast<size_t>(m); }

	static constexpr std::array<MapMode, kNMapModes> kAllModes{
	    MapMode::Amp, MapMode::Sum, MapMode::Asym, MapMode::Baseline, MapMode::RMS};

	// Precomputed streaming paths.
	std::string streamGlobalPath_;
	std::string streamDqmSummaryPath_;
	std::string streamLaserBoardPath_;
	std::string streamShifterPath_;

	std::array<std::string, kNMapModes>     streamDiskMapPath_{};
	std::array<std::string, kTotalBoards>   streamBoardPath_{};
	std::vector<std::string>                streamLivePath_;
	std::vector<std::string>                streamOneHitPath_;
	std::array<std::string, kLaserChannels> streamLaserLivePath_{};
	std::array<std::string, kLaserChannels> streamLaserOneHitPath_{};

	std::array<bool, kNMapModes> enabledModes_{};
	std::array<bool, kNMapModes> streamEnabledModes_{};

	std::array<mu2e::THMu2eCaloDisk*, kNMapModes> disk0Maps_{};
	std::array<mu2e::THMu2eCaloDisk*, kNMapModes> disk1Maps_{};

	std::array<std::array<std::vector<double>, kNDisks>, kNMapModes>   diskSum_;
	std::array<std::array<std::vector<uint32_t>, kNDisks>, kNMapModes> diskCnt_;

	// Global summaries.
	TH1F*     h_asymmetry_{nullptr};
	TProfile* h_amp_sparse_{nullptr};
	TProfile* h_amp_dense_{nullptr};
	TH1F*     h_occupancy_sparse_{nullptr};
	TH1F*     h_occupancy_sparse_norm_{nullptr};

	TH1F* h_occupancy_dense_{nullptr};
	TH1F* h_occupancy_dense_norm_{nullptr};
	TH1I* h_pair_multiplicity_{nullptr};

	TProfile* h_baseline_sparse_{nullptr};
	TProfile* h_baseline_dense_{nullptr};

	TProfile* h_rms_sparse_{nullptr};
	TProfile* h_rms_dense_{nullptr};

	TProfile* h_maxval_sparse_{nullptr};
	TProfile* h_maxval_dense_{nullptr};

	TH1F* h_amp_dist_{nullptr};

	TH2I* h_global_board_vs_channel_{nullptr};
	TH2F* h_global_waveform_density_{nullptr};
	TH2F* h_global_board_vs_channel_norm_{nullptr};

	TH1F* h_waveform_size_{nullptr};

	mu2e::ProditionsHandle<mu2e::CaloDAQMap> calodaqconds_h_;

	// Stamp-based validity avoids per-event clearing of large vectors.
	int                   pairStamp_{0};
	std::vector<SipmFeat> feat_;       // Indexed by sipmId.
	std::vector<int>      featStamp_;  // featStamp[sipmId] equals pairStamp when valid.
	std::vector<int>
	    pairedStamp_;  // pairedStamp[crystalId] equals pairStamp when paired.

	// Per-event usable digi multiplicity by SiPM.
	// This is intentionally not cleared every event.
	// A value is valid only when sipmMultiplicityStamp_[sid] == pairStamp_.
	// incMultiplicity() lazily resets the counter for the current event,
	// and getMultiplicity() returns 0 for stale entries.
	std::vector<uint16_t> sipmMultiplicity_;
	std::vector<int>      sipmMultiplicityStamp_;

	// Reused per-event containers.
	std::vector<int> repSipmIds_;
	std::vector<int> repLaserChannels_;

	// Reused streaming bookkeeping for one send call.
	std::vector<int> regularOneHitSentThisCall_;
	std::vector<int> laserOneHitSentThisCall_;
	std::vector<int> regularLiveSentThisCall_;
	std::vector<int> laserLiveSentThisCall_;

	// Waveform caching.
	std::vector<std::array<float, kWaveformNBins>> lastWf_;
	std::vector<uint8_t>                           lastWfValid_;
};

// ===========================
// Constructor and constructor helpers
// ===========================
CaloDigiDQM::CaloDigiDQM(const art::EDAnalyzer::Table<Config>& config)
    : art::EDAnalyzer{config}
    , caloDigiTag_{config().caloDigiModuleLabel()}
    , freqDQM_(config().freqDQM())
    , freqWaveforms_(config().freqWaveforms())
    , address_(config().address())
    , port_(config().port())
    , moduleTag_(config().moduleTag())
    , sendHists_(config().sendHists())
    , enableDiskMaps_(config().enableDiskMaps())
    , useReferenceFile_(config().useReferenceFile())
    , referenceFile_(config().referenceFile())
{
	validateConfig();

	if(useReferenceFile_)
		loadReferenceFile();

	createRootDirectories();
	configureDiskMapModes(config().diskCombines());
	allocateBuffers();
	bookDiskMaps();
	createHistoSender();
	precomputeStreamPaths();
	bookGlobalHistograms();
}

void CaloDigiDQM::validateConfig() const
{
	if(freqDQM_ < 0)
	{
		throw cet::exception("BADCONFIG") << "CaloDigiDQM: freqDQM must be >= 0.";
	}

	if(freqWaveforms_ < 0)
	{
		throw cet::exception("BADCONFIG") << "CaloDigiDQM: freqWaveforms must be >= 0.";
	}

	if(sendHists_)
	{
		if(address_.empty())
		{
			throw cet::exception("BADCONFIG")
			    << "CaloDigiDQM: address cannot be empty when sendHists=true.";
		}

		if(port_ <= 0)
		{
			throw cet::exception("BADCONFIG")
			    << "CaloDigiDQM: port must be positive when sendHists=true.";
		}

		if(moduleTag_.empty())
		{
			throw cet::exception("BADCONFIG")
			    << "CaloDigiDQM: moduleTag cannot be empty when sendHists=true.";
		}
	}
}

void CaloDigiDQM::loadReferenceFile()
{
	TFile f(referenceFile_.c_str(), "READ");
	if(f.IsZombie())
	{
		mf::LogWarning("CaloDigiDQM")
		    << "Reference file not available, continuing without references: "
		    << referenceFile_;
		return;
	}

	if(auto* h = dynamic_cast<TH1F*>(f.Get("ref_h_occ_dense")))
	{
		ref_h_occ_dense_ = dynamic_cast<TH1F*>(h->Clone("ref_h_occ_dense_mem"));
		ref_h_occ_dense_->SetDirectory(nullptr);
	}

	if(auto* h = dynamic_cast<TProfile*>(f.Get("ref_h_base_dense")))
	{
		ref_h_base_dense_ = dynamic_cast<TProfile*>(h->Clone("ref_h_base_dense_mem"));
		ref_h_base_dense_->SetDirectory(nullptr);
	}

	if(auto* h = dynamic_cast<TProfile*>(f.Get("ref_h_rms_dense")))
	{
		ref_h_rms_dense_ = dynamic_cast<TProfile*>(h->Clone("ref_h_rms_dense_mem"));
		ref_h_rms_dense_->SetDirectory(nullptr);
	}

	if(auto* h = dynamic_cast<TProfile*>(f.Get("ref_h_max_dense")))
	{
		ref_h_max_dense_ = dynamic_cast<TProfile*>(h->Clone("ref_h_max_dense_mem"));
		ref_h_max_dense_->SetDirectory(nullptr);
	}

	if(auto* h = dynamic_cast<TH1F*>(f.Get("ref_h_asym")))
	{
		ref_h_asym_ = dynamic_cast<TH1F*>(h->Clone("ref_h_asym_mem"));
		ref_h_asym_->SetDirectory(nullptr);
	}

	if(auto* h = dynamic_cast<TH1F*>(f.Get("ref_D0_B027_Occupancy")))
	{
		ref_D0_B027_Occupancy_ =
		    dynamic_cast<TH1F*>(h->Clone("ref_D0_B027_Occupancy_mem"));
		ref_D0_B027_Occupancy_->SetDirectory(nullptr);
	}

	if(auto* h = dynamic_cast<TProfile*>(f.Get("ref_D0_B027_Baseline")))
	{
		ref_D0_B027_Baseline_ =
		    dynamic_cast<TProfile*>(h->Clone("ref_D0_B027_Baseline_mem"));
		ref_D0_B027_Baseline_->SetDirectory(nullptr);
	}

	if(auto* h = dynamic_cast<TProfile*>(f.Get("ref_D0_B027_RMS")))
	{
		ref_D0_B027_RMS_ = dynamic_cast<TProfile*>(h->Clone("ref_D0_B027_RMS_mem"));
		ref_D0_B027_RMS_->SetDirectory(nullptr);
	}

	if(auto* h = dynamic_cast<TProfile*>(f.Get("ref_D0_B027_Max")))
	{
		ref_D0_B027_Max_ = dynamic_cast<TProfile*>(h->Clone("ref_D0_B027_Max_mem"));
		ref_D0_B027_Max_->SetDirectory(nullptr);
	}

	if(auto* h = dynamic_cast<TH1F*>(f.Get("ref_D0_B027_C00_Waveform")))
	{
		ref_D0_B027_C00_Waveform_ =
		    dynamic_cast<TH1F*>(h->Clone("ref_D0_B027_C00_Waveform_mem"));
		ref_D0_B027_C00_Waveform_->SetDirectory(nullptr);
	}
}

void CaloDigiDQM::createRootDirectories()
{
	art::ServiceHandle<art::TFileService> tfs;
	disk0Dir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk0"));
	disk1Dir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk1"));
	globalDir_ = std::make_unique<art::TFileDirectory>(tfs->mkdir("Global_Histograms"));
	laserDir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Laser"));
}

void CaloDigiDQM::configureDiskMapModes(std::vector<std::string> const& rawStream)
{
	streamEnabledModes_.fill(false);
	enabledModes_.fill(false);

	for(auto const& s : rawStream)
	{
		const auto   m    = parseMode(s);
		const size_t midx = modeIndex(m);
		if(!streamEnabledModes_[midx])
		{
			streamEnabledModes_[midx] = true;
			streamModes_.push_back(m);
		}
	}

	if(streamModes_.empty())
	{
		streamModes_.push_back(MapMode::Asym);
		streamEnabledModes_[modeIndex(MapMode::Asym)] = true;
	}

	// Save all disk-map modes to ROOT. diskCombines controls streaming only.
	for(auto m : kAllModes)
		enabledModes_[modeIndex(m)] = true;
}

void CaloDigiDQM::allocateBuffers()
{
	liveWf_.assign(kTotalChannels, nullptr);
	oneHitWf_.assign(kTotalChannels, nullptr);
	oneHitSeen_.assign(kTotalChannels, 0u);
	oneHitSent_.assign(kTotalChannels, 0u);
	liveWaveformUpdated_.assign(kTotalChannels, 0u);
	laserOneHitSent_.fill(0u);
	laserLiveWaveformUpdated_.fill(0u);
	wfStats_.assign(kTotalChannels, WaveformSizeStats{});
	h_baseline_dist_.assign(kTotalChannels, nullptr);
	h_rms_dist_.assign(kTotalChannels, nullptr);
	h_max_dist_.assign(kTotalChannels, nullptr);
	h_asym_dist_.assign(kTotalChannels, nullptr);
	channelSeen_.assign(kTotalChannels, 0u);

	activeRegularChannels_.reserve(kTotalChannels);
	activeLaserChannels_.reserve(kLaserChannels);
	updatedBoards_.reserve(kTotalBoards);
	updatedRegularChannels_.reserve(kTotalChannels);
	updatedLaserChannels_.reserve(kLaserChannels);

	repSipmIds_.reserve(kMaxSipmIdForMaps_);
	repLaserChannels_.reserve(kLaserChannels);

	regularOneHitSentThisCall_.reserve(kTotalChannels);
	laserOneHitSentThisCall_.reserve(kLaserChannels);
	regularLiveSentThisCall_.reserve(kTotalChannels);
	laserLiveSentThisCall_.reserve(kLaserChannels);

	boardQueuedForSend_.assign(kTotalBoards, 0u);
	regularQueuedForSend_.assign(kTotalChannels, 0u);
	laserQueuedForSend_.fill(0u);

	lastWf_.assign(kTotalChannels, std::array<float, kWaveformNBins>{});
	lastWfValid_.assign(kTotalChannels, 0u);

	feat_.assign((size_t)kMaxSipmIdForMaps_, SipmFeat{});
	featStamp_.assign((size_t)kMaxSipmIdForMaps_, 0);
	pairedStamp_.assign((size_t)kMaxCrystalIdForMaps_, 0);
	pairCandidates_.resize((size_t)kMaxSipmIdForMaps_);
	pairCandidateStamp_.assign((size_t)kMaxSipmIdForMaps_, 0);
	pairCandidateSipmIds_.reserve(kMaxSipmIdForMaps_);
	sipmMultiplicity_.assign((size_t)kMaxSipmIdForMaps_, 0u);
	sipmMultiplicityStamp_.assign((size_t)kMaxSipmIdForMaps_, 0);

	for(auto m : kAllModes)
	{
		const size_t midx = modeIndex(m);
		for(int d = 0; d < kNDisks; ++d)
		{
			diskSum_[midx][d].assign((size_t)kMaxSipmIdForMaps_, 0.0);
			diskCnt_[midx][d].assign((size_t)kMaxSipmIdForMaps_, 0u);
		}
	}
}

void CaloDigiDQM::bookDiskMaps()
{
	for(auto m : kAllModes)
	{
		const size_t midx = modeIndex(m);
		const char*  suf  = modeSuffix(m);

		std::string key0   = Form("disk0_%s", suf);
		std::string key1   = Form("disk1_%s", suf);
		std::string title0 = Form("Disk 0 - %s", suf);
		std::string title1 = Form("Disk 1 - %s", suf);

		auto* d0 = globalDir_->makeAndRegister<mu2e::THMu2eCaloDisk>(
		    key0.c_str(), title0.c_str(), key0.c_str(), title0.c_str(), 0);

		auto* d1 = globalDir_->makeAndRegister<mu2e::THMu2eCaloDisk>(
		    key1.c_str(), title1.c_str(), key1.c_str(), title1.c_str(), 1);

		setDiskMapTitles(d0, 0, m);
		setDiskMapTitles(d1, 1, m);

		disk0Maps_[midx] = d0;
		disk1Maps_[midx] = d1;
	}
}

void CaloDigiDQM::createHistoSender()
{
	if(sendHists_)
		histSender_ = std::make_unique<ots::HistoSender>(address_, port_);
}

void CaloDigiDQM::precomputeStreamPaths()
{
	streamGlobalPath_          = moduleTag_ + "/Global:replace";
	streamDqmSummaryPath_      = moduleTag_ + "/DQM_Summary:replace";
	streamShifterPath_         = moduleTag_ + "/Shifter:replace";
	streamLaserBoardPath_      = moduleTag_ + "/Laser/Board160:replace";
	streamWaveformDensityPath_ = moduleTag_ + "/Waveforms/GlobalWaveformDensity:replace";

	streamLivePath_.assign(kTotalChannels, "");
	streamOneHitPath_.assign(kTotalChannels, "");

	for(int bidx = 0; bidx < kTotalBoards; ++bidx)
	{
		const int disk    = bidx / kBoardsPerDisk;
		const int blocal  = bidx % kBoardsPerDisk;
		const int boardID = boardIdFromDiskAndLocal(disk, blocal);

		streamBoardPath_[(size_t)bidx] =
		    Form("%s/Disk%d/Board%03d:replace", moduleTag_.c_str(), disk, boardID);
	}

	for(int cidx = 0; cidx < kTotalChannels; ++cidx)
	{
		const int disk    = diskFromCidx(cidx);
		const int enc     = encodedFromCidx(cidx);
		const int blocal  = boardLocalFromEncoded(enc);
		const int chan    = chanFromEncoded(enc);
		const int boardID = boardIdFromDiskAndLocal(disk, blocal);

		streamLivePath_[(size_t)cidx] =
		    Form("%s/Waveforms/Disk%d/Board%03d/Channel%02d:replace",
		         moduleTag_.c_str(),
		         disk,
		         boardID,
		         chan);

		streamOneHitPath_[(size_t)cidx] =
		    Form("%s/OneHitWaveforms/Disk%d/Board%03d/Channel%02d:replace",
		         moduleTag_.c_str(),
		         disk,
		         boardID,
		         chan);
	}

	for(int chan = 0; chan < kLaserChannels; ++chan)
	{
		streamLaserLivePath_[(size_t)chan] = Form(
		    "%s/Laser/Waveforms/Board160/Channel%02d:replace", moduleTag_.c_str(), chan);

		streamLaserOneHitPath_[(size_t)chan] =
		    Form("%s/Laser/OneHitWaveforms/Board160/Channel%02d:replace",
		         moduleTag_.c_str(),
		         chan);
	}

	for(auto m : kAllModes)
	{
		const size_t midx        = modeIndex(m);
		streamDiskMapPath_[midx] = moduleTag_ + "/DiskMaps/" + modeSuffix(m) + ":replace";
	}
}

void CaloDigiDQM::bookGlobalHistograms()
{
	h_skip_reason_ = globalDir_->make<TH1I>("h_skip_reason",
	                                        "CaloDigiDQM skip reasons",
	                                        (int)SkipReason::Count,
	                                        0,
	                                        (int)SkipReason::Count);
	for(int i = 0; i < (int)SkipReason::Count; ++i)
		h_skip_reason_->GetXaxis()->SetBinLabel(i + 1, skipLabel((SkipReason)i));

	for(int disk = 0; disk < kNDisks; ++disk)
	{
		art::TFileDirectory& dir = (disk == 0) ? *disk0Dir_ : *disk1Dir_;

		TString name = Form("D%d_BoardQualityMatrix", disk);
		TString title =
		    Form("Disk %d - Board Quality Matrix;Local board ID;Metric", disk);

		h_disk_quality_matrix_[(size_t)disk] =
		    dir.make<TProfile2D>(name.Data(),
		                         title.Data(),
		                         kBoardsPerDisk,
		                         0,
		                         kBoardsPerDisk,
		                         (int)QualityMetric::Count,
		                         0,
		                         (int)QualityMetric::Count);

		styleQualityMatrix(h_disk_quality_matrix_[(size_t)disk]);
	}

	h_global_board_vs_channel_ =
	    globalDir_->make<TH2I>("h_board_vs_channel",
	                           "Board vs Channel Occupancy (All Disks + Laser)",
	                           161,
	                           0,
	                           161,
	                           20,
	                           0,
	                           20);
	h_global_board_vs_channel_->GetXaxis()->SetTitle("Board ID");
	h_global_board_vs_channel_->GetYaxis()->SetTitle("Channel ID");

	h_global_board_vs_channel_norm_ = globalDir_->make<TH2F>(
	    "h_board_vs_channel_norm",
	    "Normalized Board vs Channel Occupancy (All Disks + Laser)",
	    161,
	    0,
	    161,
	    20,
	    0,
	    20);
	h_global_board_vs_channel_norm_->GetXaxis()->SetTitle("Board ID");
	h_global_board_vs_channel_norm_->GetYaxis()->SetTitle("Channel ID");
	h_global_board_vs_channel_norm_->GetZaxis()->SetTitle("Fraction of hits");
	h_global_board_vs_channel_norm_->SetStats(0);
	h_global_board_vs_channel_norm_->SetOption("COLZ");

	h_global_waveform_density_ =
	    globalDir_->make<TH2F>("h_waveform_density",
	                           "Waveform Density (all valid digis)",
	                           150,
	                           0,
	                           150,
	                           400,
	                           2000,
	                           4095);
	h_global_waveform_density_->GetXaxis()->SetTitle("Tick");
	h_global_waveform_density_->GetYaxis()->SetTitle("ADC Value");

	h_evt_digis_ = globalDir_->make<TH1I>(
	    "h_evt_digis", "CaloDigis per Event", kEvtDigisHistMax, 0, kEvtDigisHistMax);
	h_evt_digis_->GetXaxis()->SetTitle("CaloDigis/event");
	h_evt_digis_->GetYaxis()->SetTitle("Events");

	h_digis_block_ = globalDir_->make<TProfile>("h_digis_block",
	                                            "CaloDigis vs Event Block",
	                                            kEventTrendNBins,
	                                            0,
	                                            kEventTrendNBins);
	h_digis_block_->GetXaxis()->SetTitle(
	    Form("Event block (%d events/block)", kEventBlockSize));
	h_digis_block_->GetYaxis()->SetTitle("Mean CaloDigis/event");

	h_sat_samples_ = globalDir_->make<TH1F>("h_sat_samples",
	                                        "Saturated Samples per Waveform",
	                                        kWaveformSizeHistMax + 1,
	                                        0,
	                                        kWaveformSizeHistMax + 1);
	h_sat_samples_->GetXaxis()->SetTitle(Form("Samples with ADC >= %d", kSaturationAdc));
	h_sat_samples_->GetYaxis()->SetTitle("Waveforms");
	h_sat_samples_->SetStats(0);
	h_sat_samples_->SetFillColor(kOrange + 1);
	h_sat_samples_->SetLineColor(kBlack);
	h_sat_samples_->SetLineWidth(2);
	h_sat_samples_->SetOption("HIST");

	h_sat_frac_board_ = globalDir_->make<TProfile>(
	    "h_sat_ok_board", "Saturation OK Fraction vs Board", 161, 0, 161);
	h_sat_frac_board_->GetXaxis()->SetTitle("Board ID");
	h_sat_frac_board_->GetYaxis()->SetTitle(
	    "Fraction without saturation [1=good, 0=bad]");
	styleStatusProfile(h_sat_frac_board_);

	h_amp_rms_ = globalDir_->make<TH2F>(
	    "h_amp_rms", "Amplitude vs RMS", 200, 0, 50, 300, kAmpHistMin, kAmpHistMax);
	h_amp_rms_->GetXaxis()->SetTitle("RMS [ADC]");
	h_amp_rms_->GetYaxis()->SetTitle("Amplitude [ADC]");

	h_snr_board_ = globalDir_->make<TProfile>("h_snr_board", "SNR vs Board", 161, 0, 161);
	h_snr_board_->GetXaxis()->SetTitle("Board ID");
	h_snr_board_->GetYaxis()->SetTitle("Mean amplitude/RMS");

	h_base_block_ = globalDir_->make<TProfile>(
	    "h_base_block", "Baseline vs Event Block", kEventTrendNBins, 0, kEventTrendNBins);
	h_base_block_->GetXaxis()->SetTitle(
	    Form("Event block (%d events/block)", kEventBlockSize));
	h_base_block_->GetYaxis()->SetTitle("Mean baseline [ADC]");

	h_rms_block_ = globalDir_->make<TProfile>(
	    "h_rms_block", "RMS vs Event Block", kEventTrendNBins, 0, kEventTrendNBins);
	h_rms_block_->GetXaxis()->SetTitle(
	    Form("Event block (%d events/block)", kEventBlockSize));
	h_rms_block_->GetYaxis()->SetTitle("Mean RMS [ADC]");

	h_amp_block_ = globalDir_->make<TProfile>(
	    "h_amp_block", "Amplitude vs Event Block", kEventTrendNBins, 0, kEventTrendNBins);
	h_amp_block_->GetXaxis()->SetTitle(
	    Form("Event block (%d events/block)", kEventBlockSize));
	h_amp_block_->GetYaxis()->SetTitle("Mean amplitude [ADC]");

	h_laser_block_ = globalDir_->make<TProfile>("h_laser_block",
	                                            "Laser Amplitude vs Event Block",
	                                            kEventTrendNBins,
	                                            0,
	                                            kEventTrendNBins);
	h_laser_block_->GetXaxis()->SetTitle(
	    Form("Event block (%d events/block)", kEventBlockSize));
	h_laser_block_->GetYaxis()->SetTitle("Mean laser amplitude [ADC]");

	h_amp_laser_block_ =
	    globalDir_->make<TProfile>("h_amp_laser_block",
	                               "Detector/Laser Amplitude vs Event Block",
	                               kEventTrendNBins,
	                               0,
	                               kEventTrendNBins);
	h_amp_laser_block_->GetXaxis()->SetTitle(
	    Form("Event block (%d events/block)", kEventBlockSize));
	h_amp_laser_block_->GetYaxis()->SetTitle("Mean detector amp / mean laser amp");

	h_lr_corr_ = globalDir_->make<TH2F>(
	    "h_lr_corr", "Left vs Right Amplitude", 300, -100, 3000, 300, -100, 3000);
	h_lr_corr_->GetXaxis()->SetTitle("Left amplitude [ADC]");
	h_lr_corr_->GetYaxis()->SetTitle("Right amplitude [ADC]");

	h_sum_asym_ = globalDir_->make<TH2F>(
	    "h_sum_asym", "Sum Amplitude vs Asymmetry", 300, 0, 5000, 100, -1.0, 1.0);
	h_sum_asym_->GetXaxis()->SetTitle("L + R amplitude [ADC]");
	h_sum_asym_->GetYaxis()->SetTitle("(L - R)/(L + R)");

	h_pair_ok_board_ = globalDir_->make<TProfile>(
	    "h_pair_ok_board", "Valid Same-Disk Pair Completeness vs Board", 161, 0, 161);
	h_pair_ok_board_->GetXaxis()->SetTitle("Board ID");
	h_pair_ok_board_->GetYaxis()->SetTitle("Fraction with valid same-disk partner");
	styleStatusProfile(h_pair_ok_board_);

	h_unpaired_evt_ =
	    globalDir_->make<TH1I>("h_unpaired_evt", "Unpaired SiPMs per Event", 200, 0, 200);
	h_unpaired_evt_->GetXaxis()->SetTitle("Unpaired SiPMs/event");
	h_unpaired_evt_->GetYaxis()->SetTitle("Events");
	h_unpaired_evt_->SetStats(0);
	h_unpaired_evt_->SetFillColor(kOrange + 1);
	h_unpaired_evt_->SetLineColor(kBlack);
	h_unpaired_evt_->SetLineWidth(2);
	h_unpaired_evt_->SetOption("HIST");

	h_pair_raw_delta_ = globalDir_->make<TH1I>(
	    "h_pair_raw_delta",
	    "Partner rawId difference;rawId(odd SiPM) - rawId(even SiPM);Pairs",
	    81,
	    -40,
	    41);
	h_pair_raw_delta_->SetStats(0);
	h_pair_raw_delta_->SetFillColor(kBlue - 9);
	h_pair_raw_delta_->SetLineColor(kBlack);
	h_pair_raw_delta_->SetLineWidth(2);

	h_pair_board_delta_ = globalDir_->make<TH2I>(
	    "h_pair_board_delta",
	    "Partner Board Delta;Even-side board ID;Odd board - even board",
	    161,
	    0,
	    161,
	    11,
	    -5,
	    6);
	h_pair_board_delta_->SetStats(0);
	h_pair_board_delta_->SetOption("COLZ");

	h_pair_miss_frac_block_ =
	    globalDir_->make<TProfile>("h_pair_miss_frac_block",
	                               "PairMiss Fraction vs Event Block",
	                               kEventTrendNBins,
	                               0,
	                               kEventTrendNBins);
	h_pair_miss_frac_block_->GetXaxis()->SetTitle(
	    Form("Event block (%d events/block)", kEventBlockSize));
	h_pair_miss_frac_block_->GetYaxis()->SetTitle("PairMiss fraction");
	styleStatusProfile(h_pair_miss_frac_block_);

	h_pulse_shape_score_ =
	    globalDir_->make<TH1F>("h_pulse_shape_score",
	                           "Pulse Shape Roughness Score;Shape score;Waveforms",
	                           200,
	                           0,
	                           5);
	h_pulse_shape_score_->SetStats(0);
	h_pulse_shape_score_->SetFillColor(kAzure - 9);
	h_pulse_shape_score_->SetLineColor(kBlack);
	h_pulse_shape_score_->SetLineWidth(2);

	h_pulse_shape_score_board_ = globalDir_->make<TProfile>(
	    "h_pulse_shape_score_board", "Pulse Shape Roughness vs Board", 161, 0, 161);
	h_pulse_shape_score_board_->GetXaxis()->SetTitle("Board ID");
	h_pulse_shape_score_board_->GetYaxis()->SetTitle("Mean pulse-shape score");
	h_pulse_shape_score_board_->SetMarkerStyle(20);
	h_pulse_shape_score_board_->SetMarkerSize(0.7);
	h_pulse_shape_score_board_->SetLineWidth(3);

	h_health_board_ =
	    globalDir_->make<TProfile>("h_health_board", "Board Health Score", 161, 0, 161);
	h_health_board_->GetXaxis()->SetTitle("Board ID");
	h_health_board_->GetYaxis()->SetTitle("Mean health score [1=good, 0=bad]");
	styleStatusProfile(h_health_board_);

	h_health_block_ = globalDir_->make<TProfile>("h_health_block",
	                                             "Health Score vs Event Block",
	                                             kEventTrendNBins,
	                                             0,
	                                             kEventTrendNBins);
	h_health_block_->GetXaxis()->SetTitle(
	    Form("Event block (%d events/block)", kEventBlockSize));
	h_health_block_->GetYaxis()->SetTitle("Mean health score [1=good, 0=bad]");
	styleStatusProfile(h_health_block_);

	h_issue_board_ = globalDir_->make<TH2I>("h_issue_board",
	                                        "Issue Type vs Board",
	                                        161,
	                                        0,
	                                        161,
	                                        (int)IssueType::Count,
	                                        0,
	                                        (int)IssueType::Count);
	h_issue_board_->GetXaxis()->SetTitle("Board ID");
	h_issue_board_->GetYaxis()->SetTitle("Issue type");
	styleIssueMap(h_issue_board_);

	h_peak_amp_ = globalDir_->make<TH2F>("h_peak_amp",
	                                     "Peak Tick vs Amplitude",
	                                     kWaveformSizeHistMax,
	                                     0,
	                                     kWaveformSizeHistMax,
	                                     300,
	                                     kAmpHistMin,
	                                     kAmpHistMax);
	h_peak_amp_->GetXaxis()->SetTitle("Peak tick");
	h_peak_amp_->GetYaxis()->SetTitle("Amplitude [ADC]");

	h_asym_board_ = globalDir_->make<TProfile>(
	    "h_asym_board", "Mean L/R Asymmetry vs Board", 161, 0, 161);
	h_asym_board_->GetXaxis()->SetTitle("Board ID");
	h_asym_board_->GetYaxis()->SetTitle("Mean (L - R)/(L + R)");

	h_amp_laser_board_channel_ = globalDir_->make<TProfile2D>(
	    "h_amp_laser_board_channel",
	    "Mean Detector Channel Amplitude / Laser Amplitude;Board ID;Channel ID",
	    161,
	    0,
	    161,
	    20,
	    0,
	    20);
	h_amp_laser_board_channel_->GetZaxis()->SetTitle("Mean amp / laser amp");
	h_amp_laser_board_channel_->SetStats(0);
	h_amp_laser_board_channel_->SetOption("COLZ");

	h_badness_board_channel_ =
	    globalDir_->make<TProfile2D>("h_badness_board_channel",
	                                 "Mean Channel Badness Score;Board ID;Channel ID",
	                                 161,
	                                 0,
	                                 161,
	                                 20,
	                                 0,
	                                 20);
	h_badness_board_channel_->GetZaxis()->SetTitle("Mean badness [0=good, 1=bad]");
	h_badness_board_channel_->SetStats(0);
	h_badness_board_channel_->SetOption("COLZ");

	h_time_resid_board_channel_ =
	    globalDir_->make<TProfile2D>("h_time_resid_board_channel",
	                                 "Mean Peak-Time Residual;Board ID;Channel ID",
	                                 161,
	                                 0,
	                                 161,
	                                 20,
	                                 0,
	                                 20);
	h_time_resid_board_channel_->GetZaxis()->SetTitle("Mean peak tick residual");
	h_time_resid_board_channel_->SetStats(0);
	h_time_resid_board_channel_->SetOption("COLZ");

	h_time_resid_board_ = globalDir_->make<TProfile>(
	    "h_time_resid_board", "Mean Peak-Time Residual vs Board", 161, 0, 161);
	h_time_resid_board_->GetXaxis()->SetTitle("Board ID");
	h_time_resid_board_->GetYaxis()->SetTitle("Mean peak tick residual");
	h_time_resid_board_->SetMarkerStyle(20);
	h_time_resid_board_->SetMarkerSize(0.7);
	h_time_resid_board_->SetLineWidth(3);

	h_time_resid_dist_ = globalDir_->make<TH1F>(
	    "h_time_resid_dist",
	    "Peak-Time Residual Distribution;peak tick - expected peak tick;Waveforms",
	    101,
	    -50.5,
	    50.5);
	h_time_resid_dist_->SetStats(0);
	h_time_resid_dist_->SetLineColor(kBlue + 1);
	h_time_resid_dist_->SetLineWidth(2);

	h_shape_class_board_ =
	    globalDir_->make<TH2I>("h_shape_class_board",
	                           "Waveform Shape Class vs Board;Board ID;Shape class",
	                           161,
	                           0,
	                           161,
	                           (int)ShapeClass::Count,
	                           0,
	                           (int)ShapeClass::Count);
	h_shape_class_board_->SetStats(0);
	h_shape_class_board_->SetOption("COLZ");
	h_shape_class_board_->GetZaxis()->SetTitle("Count");
	for(int i = 0; i < (int)ShapeClass::Count; ++i)
		h_shape_class_board_->GetYaxis()->SetBinLabel(i + 1,
		                                              shapeClassLabel((ShapeClass)i));

	h_shape_class_channel_ =
	    globalDir_->make<TH2I>("h_shape_class_channel",
	                           "Waveform Shape Class vs Encoded Channel;Encoded Channel "
	                           "(boardID*20 + chanID);Shape class",
	                           axisDenseGlobal().nBins,
	                           axisDenseGlobal().xMin,
	                           axisDenseGlobal().xMax,
	                           (int)ShapeClass::Count,
	                           0,
	                           (int)ShapeClass::Count);
	h_shape_class_channel_->SetStats(0);
	h_shape_class_channel_->SetOption("COLZ");
	h_shape_class_channel_->GetZaxis()->SetTitle("Count");
	for(int i = 0; i < (int)ShapeClass::Count; ++i)
		h_shape_class_channel_->GetYaxis()->SetBinLabel(i + 1,
		                                                shapeClassLabel((ShapeClass)i));

	for(int i = 0; i < (int)IssueType::Count; ++i)
		h_issue_board_->GetYaxis()->SetBinLabel(i + 1, issueLabel((IssueType)i));

	h_dqm_summary_ = globalDir_->make<TH1F>(
	    "h_dqm_summary", "DQM Summary Status;Check;Status [1=good, 0=bad]", 9, 0, 9);

	const char* dqmSummaryLabels[] = {"HasEvents",
	                                  "HasDigis",
	                                  "PairComplete",
	                                  "SaturationOK",
	                                  "RMS_OK",
	                                  "LaserSeen",
	                                  "DetLaserRatio",
	                                  "HealthOK",
	                                  "Overall"};

	for(int i = 0; i < 9; ++i)
		h_dqm_summary_->GetXaxis()->SetBinLabel(i + 1, dqmSummaryLabels[i]);
	styleStatusBar(h_dqm_summary_);

	h_dqm_issue_counts_ = globalDir_->make<TH1F>("h_dqm_issue_counts",
	                                             "DQM Issue Counts;Issue type;Count",
	                                             (int)IssueType::Count,
	                                             0,
	                                             (int)IssueType::Count);
	styleIssueCounts(h_dqm_issue_counts_);
	for(int i = 0; i < (int)IssueType::Count; ++i)
		h_dqm_issue_counts_->GetXaxis()->SetBinLabel(i + 1, issueLabel((IssueType)i));
	h_dqm_issue_counts_->SetStats(0);

	h_dqm_run_counters_ = globalDir_->make<TH1F>(
	    "h_dqm_run_counters", "DQM Run Counters;Counter;Value", 11, 0, 11);
	styleIssueCounts(h_dqm_run_counters_);

	const char* dqmCounterLabels[] = {"Events",
	                                  "DigisThisEvent",
	                                  "MappedDisk0Digis",
	                                  "MappedDisk1Digis",
	                                  "MappedLaserDigis",
	                                  "SkippedTotal",
	                                  "UnpairedThisEvent",
	                                  "TotalSendErrors",
	                                  "EvtDigisOverflow",
	                                  "WaveformSizeOverflow",
	                                  "AmpOverflow"};

	for(int i = 0; i < 11; ++i)
		h_dqm_run_counters_->GetXaxis()->SetBinLabel(i + 1, dqmCounterLabels[i]);

	h_dqm_run_counters_->SetStats(0);

	h_dqm_summary_block_ = globalDir_->make<TProfile>("h_dqm_summary_block",
	                                                  "Overall DQM Status vs Event Block",
	                                                  kEventTrendNBins,
	                                                  0,
	                                                  kEventTrendNBins);
	h_dqm_summary_block_->GetXaxis()->SetTitle(
	    Form("Event block (%d events/block)", kEventBlockSize));
	h_dqm_summary_block_->GetYaxis()->SetTitle("Mean DQM status [1=good, 0=bad]");
	styleStatusProfile(h_dqm_summary_block_);

	h_amp_dist_ = globalDir_->make<TH1F>(
	    "h_amp_dist", "Amplitude Distribution", 400, kAmpHistMin, kAmpHistMax);
	h_amp_dist_->GetXaxis()->SetTitle("Amplitude [ADC]");
	h_amp_dist_->GetYaxis()->SetTitle("Count");

	const int sizeMax = std::max(10, kWaveformSizeHistMax);
	h_waveform_size_  = globalDir_->make<TH1F>(
        "h_waveform_size", "Waveform size distribution", sizeMax, 0, sizeMax);
	h_waveform_size_->GetXaxis()->SetTitle("waveform.size() [samples]");
	h_waveform_size_->GetYaxis()->SetTitle("Count");

	h_pair_multiplicity_ = globalDir_->make<TH1I>(
	    "h_pair_multiplicity",
	    "Paired crystal: max usable digi multiplicity per side per event",
	    20,
	    0,
	    20);
	h_pair_multiplicity_->GetXaxis()->SetTitle("max (usable digis in left/right SiPM)");
	h_pair_multiplicity_->GetYaxis()->SetTitle("Count");

	auto axisSparse = axisSparseGlobal();
	auto axisDense  = axisDenseGlobal();

	h_occupancy_sparse_ =
	    globalDir_->make<TH1F>("h_occ_sparse",
	                           "Occupancy (Sparse Encoding, All Disks + Laser)",
	                           axisSparse.nBins,
	                           axisSparse.xMin,
	                           axisSparse.xMax);
	h_occupancy_sparse_->GetXaxis()->SetTitle("Encoded Channel (boardID*100 + chanID)");
	h_occupancy_sparse_->GetYaxis()->SetTitle("Hit Count");

	h_occupancy_sparse_norm_ = globalDir_->make<TH1F>(
	    "h_occ_sparse_norm",
	    "Normalized Occupancy (Sparse Encoding, All Disks + Laser)",
	    axisSparse.nBins,
	    axisSparse.xMin,
	    axisSparse.xMax);
	h_occupancy_sparse_norm_->GetXaxis()->SetTitle(
	    "Encoded Channel (boardID*100 + chanID)");
	h_occupancy_sparse_norm_->GetYaxis()->SetTitle("Fraction of hits");
	h_occupancy_sparse_norm_->SetStats(0);
	h_occupancy_sparse_norm_->SetLineWidth(2);

	h_occupancy_dense_ =
	    globalDir_->make<TH1F>("h_occ_dense",
	                           "Occupancy (Dense Encoding, All Disks + Laser)",
	                           axisDense.nBins,
	                           axisDense.xMin,
	                           axisDense.xMax);
	h_occupancy_dense_->GetXaxis()->SetTitle("Encoded Channel (boardID*20 + chanID)");
	h_occupancy_dense_->GetYaxis()->SetTitle("Hit Count");

	h_occupancy_dense_norm_ =
	    globalDir_->make<TH1F>("h_occ_dense_norm",
	                           "Normalized Occupancy (Dense Encoding, All Disks + Laser)",
	                           axisDense.nBins,
	                           axisDense.xMin,
	                           axisDense.xMax);
	h_occupancy_dense_norm_->GetXaxis()->SetTitle(
	    "Encoded Channel (boardID*20 + chanID)");
	h_occupancy_dense_norm_->GetYaxis()->SetTitle("Fraction of hits");
	h_occupancy_dense_norm_->SetStats(0);
	h_occupancy_dense_norm_->SetLineWidth(2);

	auto makeGlobalProfile = [&](const char*       name,
	                             const char*       title,
	                             EncodedAxisConfig ax,
	                             const char*       xTitle,
	                             const char*       yTitle) -> TProfile* {
		auto* h = globalDir_->make<TProfile>(name, title, ax.nBins, ax.xMin, ax.xMax);
		h->SetMarkerStyle(20);
		h->GetXaxis()->SetTitle(xTitle);
		h->GetYaxis()->SetTitle(yTitle);
		return h;
	};

	h_occ_board_ = globalDir_->make<TH1F>(
	    "h_occ_board", "Occupancy vs Board ID;Board ID;Hit Count", 161, 0, 161);
	h_occ_board_->SetStats(0);
	h_occ_board_->SetLineWidth(2);

	h_occ_board_norm_ = globalDir_->make<TH1F>(
	    "h_occ_board_norm",
	    "Normalized Occupancy vs Board ID;Board ID;Fraction of hits",
	    161,
	    0,
	    161);
	h_occ_board_norm_->SetStats(0);
	h_occ_board_norm_->SetLineWidth(2);

	g_nhits_ewt_ = globalDir_->makeAndRegister<TGraph>(
	    "g_nhits_ewt",
	    "Average Number of CaloDigis vs art Event Number;art event number;Mean "
	    "CaloDigis/event",
	    0);

	g_nhits_ewt_->SetMarkerStyle(20);
	g_nhits_ewt_->SetMarkerSize(0.7);
	g_nhits_ewt_->SetLineColor(kBlue + 1);
	g_nhits_ewt_->SetMarkerColor(kBlue + 1);
	g_nhits_ewt_->SetDrawOption("P");

	h_asym_chanid_ =
	    globalDir_->make<TH2F>("h_asym_chanid",
	                           "Asymmetry vs Encoded Channel ID;Encoded Channel "
	                           "(boardID*100 + chanID);(L - R)/(L + R)",
	                           axisSparse.nBins,
	                           axisSparse.xMin,
	                           axisSparse.xMax,
	                           100,
	                           -1.0,
	                           1.0);
	h_asym_chanid_->SetStats(0);
	h_asym_chanid_->SetOption("COLZ");
	h_asym_chanid_->GetZaxis()->SetTitle("Count");

	h_baseline_sparse_ =
	    makeGlobalProfile("h_base_sparse",
	                      "Baseline (Sparse Encoding, All Disks + Laser)",
	                      axisSparse,
	                      "Encoded Channel (boardID*100 + chanID)",
	                      "Mean Baseline [ADC]");

	h_baseline_dense_ = makeGlobalProfile("h_base_dense",
	                                      "Baseline (Dense Encoding, All Disks + Laser)",
	                                      axisDense,
	                                      "Encoded Channel (boardID*20 + chanID)",
	                                      "Mean Baseline [ADC]");

	h_rms_sparse_ = makeGlobalProfile("h_rms_sparse",
	                                  "RMS (Sparse Encoding, All Disks + Laser)",
	                                  axisSparse,
	                                  "Encoded Channel (boardID*100 + chanID)",
	                                  "Mean RMS [ADC]");

	h_rms_dense_ = makeGlobalProfile("h_rms_dense",
	                                 "RMS (Dense Encoding, All Disks + Laser)",
	                                 axisDense,
	                                 "Encoded Channel (boardID*20 + chanID)",
	                                 "Mean RMS [ADC]");

	h_amp_sparse_ = makeGlobalProfile("h_amp_sparse",
	                                  "Amplitude (Sparse Encoding, All Disks + Laser)",
	                                  axisSparse,
	                                  "Encoded Channel (boardID*100 + chanID)",
	                                  "Mean Amplitude [ADC]");

	h_amp_dense_ = makeGlobalProfile("h_amp_dense",
	                                 "Amplitude (Dense Encoding, All Disks + Laser)",
	                                 axisDense,
	                                 "Encoded Channel (boardID*20 + chanID)",
	                                 "Mean Amplitude [ADC]");

	h_maxval_sparse_ = makeGlobalProfile("h_max_sparse",
	                                     "Max ADC (Sparse Encoding, All Disks + Laser)",
	                                     axisSparse,
	                                     "Encoded Channel (boardID*100 + chanID)",
	                                     "Mean Peak ADC [ADC]");

	h_maxval_dense_ = makeGlobalProfile("h_max_dense",
	                                    "Max ADC (Dense Encoding, All Disks + Laser)",
	                                    axisDense,
	                                    "Encoded Channel (boardID*20 + chanID)",
	                                    "Mean Peak ADC [ADC]");

	h_asymmetry_ =
	    globalDir_->make<TH1F>("h_asym", "Left-Right Asymmetry", 100, -1.0, 1.0);
	h_asymmetry_->GetXaxis()->SetTitle("(L - R)/(L + R)");
	h_asymmetry_->GetYaxis()->SetTitle("Frequency");
}

void CaloDigiDQM::styleQualityMatrix(TProfile2D* h)
{
	if(!h)
		return;

	h->SetStats(0);
	h->SetOption("COLZ");
	h->GetXaxis()->SetTitleOffset(1.15);
	h->GetYaxis()->SetTitleOffset(1.25);
	h->GetZaxis()->SetTitle("Mean score [1=good, 0=bad]");
	h->GetZaxis()->SetTitleOffset(1.20);

	for(int i = 0; i < (int)QualityMetric::Count; ++i)
		h->GetYaxis()->SetBinLabel(i + 1, qualityMetricLabel((QualityMetric)i));
}

void CaloDigiDQM::fillQualityMetric(
    int boardID, int chanID, QualityMetric metric, double value, int eventBlock)
{
	if(chanID < 0 || chanID >= kChannelsPerBoard)
		return;

	const int disk = boardID / kBoardsPerDisk;
	if(disk < 0 || disk >= kNDisks)
		return;

	const int localBoard = boardID - boardMinForDisk(disk);
	if(localBoard < 0 || localBoard >= kBoardsPerDisk)
		return;

	const double v = std::max(0.0, std::min(1.0, value));

	if(h_disk_quality_matrix_[(size_t)disk])
		h_disk_quality_matrix_[(size_t)disk]->Fill(localBoard, (int)metric, v);

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return;

	auto& bh = boardH_[(size_t)bidx];

	if(bh.quality)
		bh.quality->Fill(chanID, (int)metric, v);

	if(metric == QualityMetric::WaveformHealth && bh.healthTrend)
		bh.healthTrend->Fill(boardTrendBin(eventBlock), chanID, v);
}

void CaloDigiDQM::fillBoardIssueMetric(int boardID, int chanID, IssueType issue)
{
	if(chanID < 0 || chanID >= kChannelsPerBoard)
		return;

	const int disk = boardID / kBoardsPerDisk;
	if(disk < 0 || disk >= kNDisks)
		return;

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return;

	auto& bh = boardH_[(size_t)bidx];

	if(bh.issue)
		bh.issue->Fill(chanID, (int)issue);
}

// ===========================
// Idempotent booking / prebooking helpers
// ===========================
//
// Used both for CaloDAQMap-based prebooking and runtime fallback.
// Booking state is separate from whether a channel has appeared in data.

TH1F* CaloDigiDQM::ensureChannelDistBooked(
    std::vector<TH1F*>&                                             storage,
    std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards>& dirs,
    int                                                             disk,
    int                                                             boardID,
    int                                                             chanID,
    const char*                                                     suffix,
    const char*                                                     titleBase,
    int                                                             bins,
    double                                                          xmin,
    double                                                          xmax,
    const char*                                                     xTitle)
{
	if(!validRegularChannel(disk, boardID, chanID))
		return nullptr;

	const int cidx = channelIndex(disk, boardID, chanID);
	if(cidx < 0 || (size_t)cidx >= storage.size())
		return nullptr;

	if(storage[(size_t)cidx])
		return storage[(size_t)cidx];

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return nullptr;

	if(!dirs[(size_t)bidx])
		return nullptr;

	TString hname  = Form("D%d_B%03d_C%02d_%s", disk, boardID, chanID, suffix);
	TString htitle = Form("%s D%d B%03d C%02d", titleBase, disk, boardID, chanID);

	storage[(size_t)cidx] = bookDist(
	    *dirs[(size_t)bidx], hname.Data(), htitle.Data(), bins, xmin, xmax, xTitle);

	return storage[(size_t)cidx];
}

void CaloDigiDQM::ensureBaselineDistBooked(int disk, int boardID, int chanID)
{
	ensureChannelDistBooked(h_baseline_dist_,
	                        boardBaselineDir_,
	                        disk,
	                        boardID,
	                        chanID,
	                        "BaselineDist",
	                        "Baseline Distribution",
	                        2000,
	                        1000,
	                        3000,
	                        "Baseline [ADC]");
}

void CaloDigiDQM::ensureRmsDistBooked(int disk, int boardID, int chanID)
{
	ensureChannelDistBooked(h_rms_dist_,
	                        boardRmsDir_,
	                        disk,
	                        boardID,
	                        chanID,
	                        "RMSDist",
	                        "RMS Distribution",
	                        30,
	                        0,
	                        30,
	                        "RMS [ADC]");
}

void CaloDigiDQM::ensureMaxDistBooked(int disk, int boardID, int chanID)
{
	ensureChannelDistBooked(h_max_dist_,
	                        boardMaxDir_,
	                        disk,
	                        boardID,
	                        chanID,
	                        "MaxDist",
	                        "Max ADC Distribution",
	                        300,
	                        0,
	                        4500,
	                        "Peak ADC");
}

void CaloDigiDQM::ensureAsymDistBooked(int disk, int boardID, int chanID)
{
	ensureChannelDistBooked(h_asym_dist_,
	                        boardAsymDir_,
	                        disk,
	                        boardID,
	                        chanID,
	                        "AsymDist",
	                        "Asymmetry Distribution",
	                        100,
	                        -1.0,
	                        1.0,
	                        "(L-R)/(L+R)");
}

void CaloDigiDQM::ensureLaserBoardBooked()
{
	if(!laserBoardHistDir_ || !laserBoardChanDir_)
	{
		art::TFileDirectory boardDir =
		    laserDir_->mkdir(Form("Board_%03d", kLaserBoardID));

		laserBoardHistDir_ =
		    std::make_unique<art::TFileDirectory>(boardDir.mkdir("Histograms"));
		laserBoardChanDir_ =
		    std::make_unique<art::TFileDirectory>(boardDir.mkdir("Channels"));
		laserBaselineDir_ =
		    std::make_unique<art::TFileDirectory>(laserBoardChanDir_->mkdir("Baseline"));
		laserRmsDir_ =
		    std::make_unique<art::TFileDirectory>(laserBoardChanDir_->mkdir("RMS"));
		laserMaxDir_ =
		    std::make_unique<art::TFileDirectory>(laserBoardChanDir_->mkdir("Max"));
	}

	if(!laserBoardH_.occ)
	{
		art::TFileDirectory& histosDir = *laserBoardHistDir_;

		laserBoardH_.occ = histosDir.make<TH1F>("B160_Occupancy",
		                                        "Occupancy for Laser Board 160",
		                                        kLaserChannels,
		                                        0,
		                                        kLaserChannels);
		laserBoardH_.occ->GetXaxis()->SetTitle("Channel ID");
		laserBoardH_.occ->GetYaxis()->SetTitle("Count");

		laserBoardH_.occNorm =
		    histosDir.make<TH1F>("B160_OccupancyNorm",
		                         "Normalized Occupancy for Laser Board 160",
		                         kLaserChannels,
		                         0,
		                         kLaserChannels);
		laserBoardH_.occNorm->GetXaxis()->SetTitle("Channel ID");
		laserBoardH_.occNorm->GetYaxis()->SetTitle("Fraction of laser hits");
		laserBoardH_.occNorm->SetStats(0);
		laserBoardH_.occNorm->SetLineWidth(2);

		laserBoardH_.base = histosDir.make<TProfile>("B160_Baseline",
		                                             "Baseline for Laser Board 160",
		                                             kLaserChannels,
		                                             0,
		                                             kLaserChannels);

		laserBoardH_.rms = histosDir.make<TProfile>(
		    "B160_RMS", "RMS for Laser Board 160", kLaserChannels, 0, kLaserChannels);

		laserBoardH_.max = histosDir.make<TProfile>(
		    "B160_Max", "Max for Laser Board 160", kLaserChannels, 0, kLaserChannels);

		for(auto* p : {laserBoardH_.base, laserBoardH_.rms, laserBoardH_.max})
		{
			p->GetXaxis()->SetTitle("Channel ID");
			p->SetMarkerStyle(20);
		}

		laserBoardH_.base->GetYaxis()->SetTitle("Mean Baseline [ADC]");
		laserBoardH_.rms->GetYaxis()->SetTitle("Mean RMS [ADC]");
		laserBoardH_.max->GetYaxis()->SetTitle("Mean Peak ADC [ADC]");
	}
}

void CaloDigiDQM::ensureLaserBaselineDistBooked(int chanID)
{
	if(chanID < 0 || chanID >= kLaserChannels || laserBaselineDist_[(size_t)chanID] ||
	   !laserBaselineDir_)
		return;

	TString hname  = Form("B160_C%02d_BaselineDist", chanID);
	TString htitle = Form("Baseline Distribution B160 C%02d", chanID);

	laserBaselineDist_[(size_t)chanID] = bookDist(*laserBaselineDir_,
	                                              hname.Data(),
	                                              htitle.Data(),
	                                              2000,
	                                              1000,
	                                              3000,
	                                              "Baseline [ADC]");
}

void CaloDigiDQM::ensureLaserRmsDistBooked(int chanID)
{
	if(chanID < 0 || chanID >= kLaserChannels || laserRmsDist_[(size_t)chanID] ||
	   !laserRmsDir_)
		return;

	TString hname  = Form("B160_C%02d_RMSDist", chanID);
	TString htitle = Form("RMS Distribution B160 C%02d", chanID);

	laserRmsDist_[(size_t)chanID] =
	    bookDist(*laserRmsDir_, hname.Data(), htitle.Data(), 30, 0, 30, "RMS [ADC]");
}

void CaloDigiDQM::ensureLaserMaxDistBooked(int chanID)
{
	if(chanID < 0 || chanID >= kLaserChannels || laserMaxDist_[(size_t)chanID] ||
	   !laserMaxDir_)
		return;

	TString hname  = Form("B160_C%02d_MaxDist", chanID);
	TString htitle = Form("Max ADC Distribution B160 C%02d", chanID);

	laserMaxDist_[(size_t)chanID] =
	    bookDist(*laserMaxDir_, hname.Data(), htitle.Data(), 300, 0, 4500, "Peak ADC");
}

void CaloDigiDQM::ensureLaserLiveWaveformBooked(int chanID, int rawId, int sipmId)
{
	if(chanID < 0 || chanID >= kLaserChannels)
		return;
	if(laserLiveWf_[(size_t)chanID])
		return;
	if(!laserBoardHistDir_)
		return;

	art::TFileDirectory& histosDir = *laserBoardHistDir_;

	TString cname  = Form("B160_C%02d_Waveform", chanID);
	TString ctitle = Form("Live Waveform for %s",
	                      channelLabel(kLaserBoardID, chanID, rawId, sipmId).Data());

	laserLiveWf_[(size_t)chanID] = histosDir.make<TH1F>(
	    cname.Data(), ctitle.Data(), kWaveformNBins, 0, kWaveformNBins);
	laserLiveWf_[(size_t)chanID]->GetYaxis()->SetTitle("ADC - Baseline");
	laserLiveWf_[(size_t)chanID]->GetXaxis()->SetTitle("Tick");
}

template<class WaveformT>
void CaloDigiDQM::ensureLaserFirstHitBooked(int              chanID,
                                            int              rawId,
                                            int              sipmId,
                                            WaveformT const& waveform,
                                            int              wfSize,
                                            float            baseline)
{
	if(chanID < 0 || chanID >= kLaserChannels)
		return;
	if(laserOneHitSeen_[(size_t)chanID])
		return;
	if(!laserBoardChanDir_)
		return;

	art::TFileDirectory& chanDir = *laserBoardChanDir_;

	TString cname  = Form("B160_C%02d_FirstHit", chanID);
	TString ctitle = Form("First-Hit Waveform for %s",
	                      channelLabel(kLaserBoardID, chanID, rawId, sipmId).Data());

	TH1F* onehitHist = chanDir.make<TH1F>(
	    cname.Data(), ctitle.Data(), kWaveformNBins, 0, kWaveformNBins);
	onehitHist->GetYaxis()->SetTitle("ADC - Baseline");
	onehitHist->GetXaxis()->SetTitle("Tick");

	const int nb = onehitHist->GetNbinsX();
	const int n  = std::min<int>(wfSize, nb);
	for(int i = 0; i < n; ++i)
		onehitHist->SetBinContent(i + 1, (double)waveform[(size_t)i] - (double)baseline);
	for(int i = n; i < nb; ++i)
		onehitHist->SetBinContent(i + 1, 0.0);

	laserOneHitWf_[(size_t)chanID]   = onehitHist;
	laserOneHitSeen_[(size_t)chanID] = 1u;
	++pendingLaserFirstHits_;
}

void CaloDigiDQM::ensureBoardBooked(int disk, int boardID)
{
	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return;

	if(!boardHistDir_[(size_t)bidx] || !boardChanDir_[(size_t)bidx])
	{
		art::TFileDirectory boardDir =
		    (disk == 0 ? *disk0Dir_ : *disk1Dir_).mkdir(Form("Board_%03d", boardID));

		boardHistDir_[(size_t)bidx] =
		    std::make_unique<art::TFileDirectory>(boardDir.mkdir("Histograms"));
		boardChanDir_[(size_t)bidx] =
		    std::make_unique<art::TFileDirectory>(boardDir.mkdir("Channels"));

		boardBaselineDir_[(size_t)bidx] = std::make_unique<art::TFileDirectory>(
		    boardChanDir_[(size_t)bidx]->mkdir("Baseline"));

		boardRmsDir_[(size_t)bidx] = std::make_unique<art::TFileDirectory>(
		    boardChanDir_[(size_t)bidx]->mkdir("RMS"));

		boardMaxDir_[(size_t)bidx] = std::make_unique<art::TFileDirectory>(
		    boardChanDir_[(size_t)bidx]->mkdir("Max"));

		boardAsymDir_[(size_t)bidx] = std::make_unique<art::TFileDirectory>(
		    boardChanDir_[(size_t)bidx]->mkdir("Asym"));
	}

	auto& bh = boardH_[(size_t)bidx];
	if(!bh.occ)
	{
		art::TFileDirectory& histosDir = *boardHistDir_[(size_t)bidx];

		TString occName  = Form("D%d_B%03d_Occupancy", disk, boardID);
		TString occTitle = Form("Occupancy for D%d B%03d", disk, boardID);

		bh.occ = histosDir.make<TH1F>(
		    occName.Data(), occTitle.Data(), kChannelsPerBoard, 0, kChannelsPerBoard);

		bh.occ->GetXaxis()->SetTitle("Channel ID");
		bh.occ->GetYaxis()->SetTitle("Hit Count");

		TString occNormName  = Form("D%d_B%03d_OccupancyNorm", disk, boardID);
		TString occNormTitle = Form("Normalized Occupancy for D%d B%03d", disk, boardID);

		bh.occNorm = histosDir.make<TH1F>(occNormName.Data(),
		                                  occNormTitle.Data(),
		                                  kChannelsPerBoard,
		                                  0,
		                                  kChannelsPerBoard);

		bh.occNorm->GetXaxis()->SetTitle("Channel ID");
		bh.occNorm->GetYaxis()->SetTitle("Fraction of board hits");
		bh.occNorm->SetStats(0);
		bh.occNorm->SetLineWidth(2);

		TString baseName  = Form("D%d_B%03d_Baseline", disk, boardID);
		TString baseTitle = Form("Baseline for D%d B%03d", disk, boardID);

		bh.base = histosDir.make<TProfile>(
		    baseName.Data(), baseTitle.Data(), kChannelsPerBoard, 0, kChannelsPerBoard);

		TString rmsName  = Form("D%d_B%03d_RMS", disk, boardID);
		TString rmsTitle = Form("RMS for D%d B%03d", disk, boardID);

		bh.rms = histosDir.make<TProfile>(
		    rmsName.Data(), rmsTitle.Data(), kChannelsPerBoard, 0, kChannelsPerBoard);

		TString maxName  = Form("D%d_B%03d_Max", disk, boardID);
		TString maxTitle = Form("Max for D%d B%03d", disk, boardID);

		bh.max = histosDir.make<TProfile>(
		    maxName.Data(), maxTitle.Data(), kChannelsPerBoard, 0, kChannelsPerBoard);

		TString qualityName = Form("D%d_B%03d_ChannelQualityMatrix", disk, boardID);
		TString qualityTitle =
		    Form("D%d B%03d - Channel Quality Matrix;Channel ID;Metric", disk, boardID);

		bh.quality = histosDir.make<TProfile2D>(qualityName.Data(),
		                                        qualityTitle.Data(),
		                                        kChannelsPerBoard,
		                                        0,
		                                        kChannelsPerBoard,
		                                        (int)QualityMetric::Count,
		                                        0,
		                                        (int)QualityMetric::Count);

		styleQualityMatrix(bh.quality);

		TString issueName = Form("D%d_B%03d_ChannelIssueMap", disk, boardID);
		TString issueTitle =
		    Form("D%d B%03d - Channel Issue Map;Channel ID;Issue type", disk, boardID);

		bh.issue = histosDir.make<TH2I>(issueName.Data(),
		                                issueTitle.Data(),
		                                kChannelsPerBoard,
		                                0,
		                                kChannelsPerBoard,
		                                (int)IssueType::Count,
		                                0,
		                                (int)IssueType::Count);

		styleIssueMap(bh.issue);

		for(int i = 0; i < (int)IssueType::Count; ++i)
			bh.issue->GetYaxis()->SetBinLabel(i + 1, issueLabel((IssueType)i));

		TString trendName = Form("D%d_B%03d_ChannelHealthTrend", disk, boardID);
		TString trendTitle =
		    Form("D%d B%03d - Channel Health Trend;Coarse event block;Channel ID",
		         disk,
		         boardID);

		bh.healthTrend = histosDir.make<TProfile2D>(trendName.Data(),
		                                            trendTitle.Data(),
		                                            kBoardTrendNBins,
		                                            0,
		                                            kBoardTrendNBins,
		                                            kChannelsPerBoard,
		                                            0,
		                                            kChannelsPerBoard);

		bh.healthTrend->SetStats(0);
		bh.healthTrend->SetOption("COLZ");
		bh.healthTrend->GetZaxis()->SetTitle("Mean health [1=good, 0=bad]");

		for(auto* p : {bh.base, bh.rms, bh.max})
		{
			p->GetXaxis()->SetTitle("Channel ID");
			p->SetMarkerStyle(20);
		}

		bh.base->GetYaxis()->SetTitle("Mean Baseline [ADC]");
		bh.rms->GetYaxis()->SetTitle("Mean RMS [ADC]");
		bh.max->GetYaxis()->SetTitle("Mean Peak ADC [ADC]");
	}
}

void CaloDigiDQM::ensureLiveWaveformBooked(
    int disk, int boardID, int chanID, int rawId, int sipmId)
{
	if(!validRegularChannel(disk, boardID, chanID))
		return;

	const int cidx = channelIndex(disk, boardID, chanID);
	if(liveWf_[(size_t)cidx])
		return;

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards || !boardHistDir_[(size_t)bidx])
		return;

	art::TFileDirectory& histosDir = *boardHistDir_[(size_t)bidx];

	TString cname = Form("D%d_B%03d_C%02d_Waveform", disk, boardID, chanID);
	TString ctitle =
	    Form("Live Waveform for %s", channelLabel(boardID, chanID, rawId, sipmId).Data());

	liveWf_[(size_t)cidx] = histosDir.make<TH1F>(
	    cname.Data(), ctitle.Data(), kWaveformNBins, 0, kWaveformNBins);
	liveWf_[(size_t)cidx]->GetYaxis()->SetTitle("ADC - Baseline");
	liveWf_[(size_t)cidx]->GetXaxis()->SetTitle("Tick");
}

template<class WaveformT>
void CaloDigiDQM::ensureFirstHitBooked(int              disk,
                                       int              boardID,
                                       int              chanID,
                                       int              rawId,
                                       int              sipmId,
                                       WaveformT const& waveform,
                                       int              wfSize,
                                       float            baseline)
{
	if(!validRegularChannel(disk, boardID, chanID))
		return;

	const int cidx = channelIndex(disk, boardID, chanID);
	if(oneHitSeen_[(size_t)cidx])
		return;

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards || !boardChanDir_[(size_t)bidx])
		return;

	art::TFileDirectory& chanDir = *boardChanDir_[(size_t)bidx];

	TString cname  = Form("D%d_B%03d_C%02d_FirstHit", disk, boardID, chanID);
	TString ctitle = Form("First-Hit Waveform for %s",
	                      channelLabel(boardID, chanID, rawId, sipmId).Data());

	TH1F* onehitHist = chanDir.make<TH1F>(
	    cname.Data(), ctitle.Data(), kWaveformNBins, 0, kWaveformNBins);
	onehitHist->GetYaxis()->SetTitle("ADC - Baseline");
	onehitHist->GetXaxis()->SetTitle("Tick");

	const int nb = onehitHist->GetNbinsX();
	const int n  = std::min<int>(wfSize, nb);
	for(int i = 0; i < n; ++i)
		onehitHist->SetBinContent(i + 1, (double)waveform[(size_t)i] - (double)baseline);
	for(int i = n; i < nb; ++i)
		onehitHist->SetBinContent(i + 1, 0.0);

	oneHitWf_[(size_t)cidx]   = onehitHist;
	oneHitSeen_[(size_t)cidx] = 1u;
	++pendingRegularFirstHits_;
}

// ===========================
// Feature extraction helpers
// ===========================
template<class WaveformT>
double CaloDigiDQM::computePulseShapeScore(WaveformT const& waveform,
                                           int              wfSize,
                                           float            baseline,
                                           double           amp) const
{
	if(wfSize < kMinShapeSamples)
		return 0.0;

	if(amp <= 0.0)
		return 0.0;

	const int n = std::min<int>(wfSize, kWaveformNBins);

	double roughness = 0.0;

	for(int i = 1; i + 1 < n; ++i)
	{
		const double y0 = (double)waveform[(size_t)(i - 1)] - baseline;
		const double y1 = (double)waveform[(size_t)i] - baseline;
		const double y2 = (double)waveform[(size_t)(i + 1)] - baseline;

		const double secondDiff = y2 - 2.0 * y1 + y0;
		roughness += secondDiff * secondDiff;
	}

	return roughness / std::max(1.0, amp * amp);
}

template<class WaveformT>
CaloDigiDQM::ShapeClass CaloDigiDQM::classifyWaveform(WaveformT const& waveform,
                                                      int              wfSize,
                                                      float            baseline,
                                                      int              peakpos,
                                                      double           rms,
                                                      double           amp,
                                                      int              nSat,
                                                      double           shapeScore) const
{
	if(nSat > 0)
		return ShapeClass::Saturated;

	if(peakpos <= 1 || peakpos >= wfSize - 2)
		return ShapeClass::EdgePeak;

	if(amp < 0.0)
		return ShapeClass::Negative;

	if(rms > kMaxGoodRms)
		return ShapeClass::Noisy;

	if(shapeScore > kMaxGoodShapeScore)
		return ShapeClass::Noisy;

	const int n = std::min<int>(wfSize, kWaveformNBins);

	if(n > peakpos + 8)
	{
		const double peak = (double)waveform[(size_t)peakpos] - baseline;
		const double tail = (double)waveform[(size_t)(peakpos + 8)] - baseline;

		if(peak > 0.0 && tail / peak > kLongTailFraction)
			return ShapeClass::LongTail;
	}

	if(n > peakpos + 3)
	{
		const double after = (double)waveform[(size_t)(peakpos + 3)] - baseline;

		if(after < -kUndershootSigma * std::max(1.0, (double)rms))
			return ShapeClass::Undershoot;
	}

	return ShapeClass::Good;
}

template<class WaveformT>
bool CaloDigiDQM::extractFeatures(WaveformT const& waveform,
                                  int              peakpos,
                                  int              boardID,
                                  int              chanID,
                                  int              eventBlock,
                                  DigiFeatures&    out)
{
	out        = DigiFeatures{};
	out.wfSize = (int)waveform.size();

	if(out.wfSize > 0)
	{
		const int nCheck = out.wfSize;

		for(int i = 0; i < nCheck; ++i)
		{
			if(waveform[(size_t)i] >= kSaturationAdc)
				++out.nSat;
		}

		if(h_sat_samples_)
			h_sat_samples_->Fill(out.nSat);

		if(h_sat_frac_board_)
			h_sat_frac_board_->Fill(boardID, out.nSat == 0 ? 1.0 : 0.0);
	}

	if(out.wfSize == 0 || peakpos < 0 || peakpos >= out.wfSize)
	{
		recordSkip(SkipReason::PeakPosOutOfRange);
		return false;
	}

	out.peakpos   = peakpos;
	out.timeResid = (double)peakpos - (double)kExpectedPeakTick;

	if(h_time_resid_board_channel_)
		h_time_resid_board_channel_->Fill(boardID, chanID, out.timeResid);
	if(h_time_resid_board_)
		h_time_resid_board_->Fill(boardID, out.timeResid);
	if(h_time_resid_dist_)
		h_time_resid_dist_->Fill(out.timeResid);

	if(!computeBaselineRms(waveform, out.wfSize, out.baseline, out.rms))
	{
		recordSkip(SkipReason::NonFiniteBaselineOrRms);
		return false;
	}

	if(h_base_block_)
		h_base_block_->Fill(eventBlock, out.baseline);
	if(h_rms_block_)
		h_rms_block_->Fill(eventBlock, out.rms);

	out.ampRaw     = waveform[(size_t)peakpos];
	out.amp        = (double)out.ampRaw - (double)out.baseline;
	out.shapeScore = computePulseShapeScore(waveform, out.wfSize, out.baseline, out.amp);
	out.shapeClass = classifyWaveform(waveform,
	                                  out.wfSize,
	                                  out.baseline,
	                                  out.peakpos,
	                                  out.rms,
	                                  out.amp,
	                                  out.nSat,
	                                  out.shapeScore);

	return true;
}

bool CaloDigiDQM::decodeAddress(CaloDigi const&   digi,
                                CaloDAQMap const& calodaqconds,
                                DigiAddress&      addr)
{
	addr.sipmId = digi.SiPMID();

	if(addr.sipmId < 0)
	{
		recordSkip(SkipReason::BadSipmId);
		return false;
	}

	addr.rawId = calodaqconds.rawId(mu2e::CaloSiPMId(addr.sipmId)).id();

	if(addr.rawId == kUnmappedRawId)
	{
		recordSkip(SkipReason::UnmappedRawId);
		return false;
	}

	if(addr.rawId < 0)
	{
		recordSkip(SkipReason::RawIdNegative);
		return false;
	}

	addr.boardID = addr.rawId / kChannelsPerBoard;
	addr.chanID  = addr.rawId % kChannelsPerBoard;
	addr.isLaser = isLaserBoard(addr.boardID);

	addr.encodedSparse = encodeSparse(addr.boardID, addr.chanID);
	addr.encodedDense  = encodeDense(addr.boardID, addr.chanID);

	if(addr.isLaser)
		return true;

	addr.disk = addr.boardID / kBoardsPerDisk;

	if(!validRegularChannel(addr.disk, addr.boardID, addr.chanID))
	{
		recordSkip(SkipReason::DiskOutOfRange);
		++nFillMiss_;
		return false;
	}

	addr.cidx = channelIndex(addr.disk, addr.boardID, addr.chanID);
	return true;
}

void CaloDigiDQM::clearSummarySendQueues()
{
	for(int bidx : updatedBoards_)
	{
		if(bidx >= 0 && bidx < kTotalBoards)
			boardQueuedForSend_[(size_t)bidx] = 0u;
	}

	updatedBoards_.clear();
	laserBoardUpdated_ = false;
}

void CaloDigiDQM::clearWaveformSendQueues()
{
	for(int cidx : updatedRegularChannels_)
	{
		if(cidx >= 0 && cidx < kTotalChannels)
			regularQueuedForSend_[(size_t)cidx] = 0u;
	}

	updatedRegularChannels_.clear();

	for(int chan : updatedLaserChannels_)
	{
		if(chan >= 0 && chan < kLaserChannels)
			laserQueuedForSend_[(size_t)chan] = 0u;
	}

	updatedLaserChannels_.clear();
}

// ===========================
// Event processing
// ===========================
void CaloDigiDQM::beginEvent()
{
	// Stamp rollover protection, preserves correctness without clearing vectors each event.
	if(++pairStamp_ == std::numeric_limits<int>::max())
	{
		std::fill(featStamp_.begin(), featStamp_.end(), 0);
		std::fill(pairedStamp_.begin(), pairedStamp_.end(), 0);
		std::fill(sipmMultiplicityStamp_.begin(), sipmMultiplicityStamp_.end(), 0);
		std::fill(pairCandidateStamp_.begin(), pairCandidateStamp_.end(), 0);
		pairStamp_ = 1;
	}

	repSipmIds_.clear();
	repLaserChannels_.clear();
	laserRepSeen_.fill(0u);
	pairCandidateSipmIds_.clear();
}

void CaloDigiDQM::accumulateHitsAverage(double eventNumber, int nHits)
{
	nhitsBlockSum_ += (double)nHits;
	eventNumberBlockSum_ += eventNumber;
	++nhitsBlockCount_;

	if(nhitsBlockCount_ >= kHitsAverageBlock)
		flushHitsAveragePoint();
}

void CaloDigiDQM::flushHitsAveragePoint()
{
	if(!g_nhits_ewt_)
		return;

	if(nhitsBlockCount_ == 0)
		return;

	const double meanHits = nhitsBlockSum_ / (double)nhitsBlockCount_;

	const double meanEventNumber = eventNumberBlockSum_ / (double)nhitsBlockCount_;

	const int n = g_nhits_ewt_->GetN();
	g_nhits_ewt_->SetPoint(n, meanEventNumber, meanHits);

	nhitsBlockSum_       = 0.0;
	eventNumberBlockSum_ = 0.0;
	nhitsBlockCount_     = 0;
}

void CaloDigiDQM::fillEventLevelCounters(EventStats const& stats,
                                         int               eventBlock,
                                         double            eventNumber)
{
	if(h_evt_digis_)
		h_evt_digis_->Fill(stats.nDigis);

	if(h_digis_block_)
		h_digis_block_->Fill(eventBlock, stats.nDigis);

	accumulateHitsAverage(eventNumber, stats.nDigis);
}

void CaloDigiDQM::prebookExpectedStructureFromCaloDAQMap(CaloDAQMap const& calodaqconds)
{
	size_t nRegularChannels = 0;
	size_t nLaserChannels   = 0;
	size_t nUnmapped        = 0;
	size_t nInvalid         = 0;

	std::vector<uint8_t>                prebookedRegular((size_t)kTotalChannels, 0u);
	std::array<uint8_t, kLaserChannels> prebookedLaser{};
	prebookedLaser.fill(0u);

	for(int sipmId = 0; sipmId < kMaxSipmIdForMaps_; ++sipmId)
	{
		int rawId = kUnmappedRawId;

		try
		{
			rawId = calodaqconds.rawId(mu2e::CaloSiPMId(sipmId)).id();
		}
		catch(const std::exception&)
		{
			++nInvalid;
			continue;
		}
		catch(...)
		{
			++nInvalid;
			continue;
		}

		if(rawId == kUnmappedRawId)
		{
			++nUnmapped;
			continue;
		}

		if(rawId < 0)
		{
			++nInvalid;
			continue;
		}

		const int boardID = rawId / kChannelsPerBoard;
		const int chanID  = rawId % kChannelsPerBoard;

		if(chanID < 0 || chanID >= kChannelsPerBoard)
		{
			++nInvalid;
			continue;
		}

		if(isLaserBoard(boardID))
		{
			if(chanID < 0 || chanID >= kLaserChannels)
			{
				++nInvalid;
				continue;
			}

			ensureLaserBoardBooked();
			ensureLaserBaselineDistBooked(chanID);
			ensureLaserRmsDistBooked(chanID);
			ensureLaserMaxDistBooked(chanID);
			ensureLaserLiveWaveformBooked(chanID, rawId, sipmId);

			if(!prebookedLaser[(size_t)chanID])
			{
				prebookedLaser[(size_t)chanID] = 1u;
				++nLaserChannels;
			}

			continue;
		}

		const int disk = boardID / kBoardsPerDisk;

		if(!validRegularChannel(disk, boardID, chanID))
		{
			++nInvalid;
			continue;
		}

		const int cidx = channelIndex(disk, boardID, chanID);

		if(cidx < 0 || cidx >= kTotalChannels)
		{
			++nInvalid;
			continue;
		}

		ensureBoardBooked(disk, boardID);
		ensureBaselineDistBooked(disk, boardID, chanID);
		ensureRmsDistBooked(disk, boardID, chanID);
		ensureMaxDistBooked(disk, boardID, chanID);
		ensureAsymDistBooked(disk, boardID, chanID);
		ensureLiveWaveformBooked(disk, boardID, chanID, rawId, sipmId);

		if(!prebookedRegular[(size_t)cidx])
		{
			prebookedRegular[(size_t)cidx] = 1u;
			++nRegularChannels;
		}
	}

	mf::LogInfo("CaloDigiDQM")
	    << "Prebooked expected CaloDigiDQM ROOT structure from CaloDAQMap:"
	    << " regularChannels=" << nRegularChannels << " laserChannels=" << nLaserChannels
	    << " unmappedSipmIds=" << nUnmapped << " invalidEntries=" << nInvalid;
}

void CaloDigiDQM::analyze(art::Event const& event)
{
	beginEvent();

	const auto& caloDigis    = *event.getValidHandle<CaloDigiCollection>(caloDigiTag_);
	const auto& calodaqconds = calodaqconds_h_.get(event.id());

	if(!prebookedExpectedStructure_)
	{
		prebookExpectedStructureFromCaloDAQMap(calodaqconds);
		prebookedExpectedStructure_ = true;
	}

	const int eventBlock = (eventCounter_ / kEventBlockSize >= (uint64_t)kEventTrendNBins)
	                           ? kEventTrendNBins - 1
	                           : static_cast<int>(eventCounter_ / kEventBlockSize);

	EventStats stats{};
	stats.nDigis = (int)caloDigis.size();
	if(stats.nDigis >= kEvtDigisHistMax)
		++nEvtDigisOverflow_;

	const double eventNumber = static_cast<double>(event.id().event());

	fillEventLevelCounters(stats, eventBlock, eventNumber);

	for(const auto& digi : caloDigis)
		processDigi(digi, calodaqconds, stats, eventBlock);

	processPairs(stats, eventBlock);
	queueRepWaveformsForStreaming();
	queueRepLaserWaveformsForStreaming();

	const double meanLaserAmp = fillLaserNormalization(stats, eventBlock);
	fillDqmSummary(stats, eventBlock, meanLaserAmp);

	++eventCounter_;

	streamIfScheduled();
}

// Build normalized occupancy views for streaming.
// Raw occupancy histograms remain filled and saved to the ROOT file.
void CaloDigiDQM::updateNormalizedOccHistograms()
{
	updateNormalizedHist(h_occ_board_, h_occ_board_norm_);
	updateNormalizedHist(h_occupancy_sparse_, h_occupancy_sparse_norm_);
	updateNormalizedHist(h_occupancy_dense_, h_occupancy_dense_norm_);
	updateNormalizedHist(h_global_board_vs_channel_, h_global_board_vs_channel_norm_);

	for(auto& bh : boardH_)
		updateNormalizedHist(bh.occ, bh.occNorm);

	updateNormalizedHist(laserBoardH_.occ, laserBoardH_.occNorm);
}

void CaloDigiDQM::processDigi(CaloDigi const&   digi,
                              CaloDAQMap const& calodaqconds,
                              EventStats&       stats,
                              int               eventBlock)
{
	const auto& waveform = digi.waveform();

	if((int)waveform.size() >= kWaveformSizeHistMax)
		++nWaveformSizeOverflow_;

	if(h_waveform_size_)
		h_waveform_size_->Fill((int)waveform.size());

	DigiAddress addr;
	if(!decodeAddress(digi, calodaqconds, addr))
		return;

	if(addr.isLaser)
	{
		processLaserDigi(digi, addr, stats, eventBlock);
		return;
	}

	processRegularDigi(digi, addr, stats, eventBlock);
}

void CaloDigiDQM::processLaserDigi(CaloDigi const&    digi,
                                   DigiAddress const& addr,
                                   EventStats&        stats,
                                   int                eventBlock)
{
	const auto& waveform = digi.waveform();
	const int   wfSize   = (int)waveform.size();

	const int sipmId  = addr.sipmId;
	const int rawId   = addr.rawId;
	const int boardID = addr.boardID;
	const int chanID  = addr.chanID;

	const int encodedSparse = addr.encodedSparse;
	const int encodedDense  = addr.encodedDense;

	++nFillLaser_;
	ensureLaserBoardBooked();
	laserBoardUpdated_ = true;

	// First time this laser channel appears in data; ensure histograms exist.
	if(!laserChannelSeen_[(size_t)chanID])
	{
		ensureLaserBaselineDistBooked(chanID);
		ensureLaserRmsDistBooked(chanID);
		ensureLaserMaxDistBooked(chanID);
		ensureLaserLiveWaveformBooked(chanID, rawId, sipmId);

		laserChannelSeen_[(size_t)chanID] = 1u;
		activeLaserChannels_.push_back(chanID);
	}

	if(h_occupancy_sparse_)
		h_occupancy_sparse_->Fill(encodedSparse);
	if(h_occupancy_dense_)
		h_occupancy_dense_->Fill(encodedDense);
	if(h_global_board_vs_channel_)
		h_global_board_vs_channel_->Fill(boardID, chanID);
	if(h_occ_board_)
		h_occ_board_->Fill(boardID);
	if(laserBoardH_.occ)
		laserBoardH_.occ->Fill(chanID);

	updateWaveformStats(laserWfStats_[(size_t)chanID], wfSize);

	DigiFeatures feat;
	if(!extractFeatures(waveform, digi.peakpos(), boardID, chanID, eventBlock, feat))
		return;

	if(laserBaselineDist_[(size_t)chanID])
		laserBaselineDist_[(size_t)chanID]->Fill(feat.baseline);
	if(laserRmsDist_[(size_t)chanID])
		laserRmsDist_[(size_t)chanID]->Fill(feat.rms);
	if(laserMaxDist_[(size_t)chanID])
		laserMaxDist_[(size_t)chanID]->Fill(feat.ampRaw);

	ensureLaserFirstHitBooked(chanID, rawId, sipmId, waveform, wfSize, feat.baseline);

	fillGlobalWaveformDensity(waveform, wfSize);

	if(laserBoardH_.base)
		laserBoardH_.base->Fill(chanID, feat.baseline);
	if(laserBoardH_.rms)
		laserBoardH_.rms->Fill(chanID, feat.rms);
	if(laserBoardH_.max)
		laserBoardH_.max->Fill(chanID, feat.ampRaw);

	if(h_amp_sparse_)
		h_amp_sparse_->Fill(encodedSparse, feat.amp);
	if(h_amp_dense_)
		h_amp_dense_->Fill(encodedDense, feat.amp);
	if(h_baseline_sparse_)
		h_baseline_sparse_->Fill(encodedSparse, feat.baseline);
	if(h_baseline_dense_)
		h_baseline_dense_->Fill(encodedDense, feat.baseline);
	if(h_rms_sparse_)
		h_rms_sparse_->Fill(encodedSparse, feat.rms);
	if(h_rms_dense_)
		h_rms_dense_->Fill(encodedDense, feat.rms);
	if(h_maxval_sparse_)
		h_maxval_sparse_->Fill(encodedSparse, feat.ampRaw);
	if(h_maxval_dense_)
		h_maxval_dense_->Fill(encodedDense, feat.ampRaw);

	if(h_pulse_shape_score_)
		h_pulse_shape_score_->Fill(feat.shapeScore);
	if(h_pulse_shape_score_board_)
		h_pulse_shape_score_board_->Fill(boardID, feat.shapeScore);

	fillWaveformHealth(boardID, chanID, feat, stats, eventBlock);

	if(h_amp_rms_)
		h_amp_rms_->Fill(feat.rms, feat.amp);
	if(h_snr_board_ && feat.rms > 0.0)
		h_snr_board_->Fill(boardID, feat.amp / feat.rms);
	if(h_laser_block_)
		h_laser_block_->Fill(eventBlock, feat.amp);
	if(h_peak_amp_)
		h_peak_amp_->Fill(feat.peakpos, feat.amp);
	if(h_amp_dist_)
		h_amp_dist_->Fill(feat.amp);

	if(feat.amp < kAmpHistMin || feat.amp >= kAmpHistMax)
		++nAmpOverflow_;

	if(feat.amp > 0.0)
	{
		stats.sumLaserAmp += feat.amp;
		++stats.nLaserAmp;
		++nLaserAmpAccepted_;
	}

	SipmFeat cand{};
	cand.amp       = feat.amp;
	cand.baseline  = feat.baseline;
	cand.rms       = feat.rms;
	cand.ampRaw    = feat.ampRaw;
	cand.peakpos   = feat.peakpos;
	cand.timeResid = feat.timeResid;
	cand.snr       = (feat.rms > 0.0) ? feat.amp / feat.rms : 0.0;
	cand.rawId     = rawId;
	cand.disk      = -1;
	cand.board     = kLaserBoardID;
	cand.chan      = chanID;
	cand.cidx      = -1;

	packRepWaveform(cand, waveform, feat.baseline, wfSize);

	if(!laserRepSeen_[(size_t)chanID])
	{
		laserRep_[(size_t)chanID]     = cand;
		laserRepSeen_[(size_t)chanID] = 1u;
		repLaserChannels_.push_back(chanID);
	}
	else if(betterRep(cand, laserRep_[(size_t)chanID]))
	{
		laserRep_[(size_t)chanID] = cand;
	}
}

void CaloDigiDQM::processRegularDigi(CaloDigi const&    digi,
                                     DigiAddress const& addr,
                                     EventStats&        stats,
                                     int                eventBlock)
{
	const auto& waveform = digi.waveform();
	const int   wfSize   = (int)waveform.size();

	const int sipmId  = addr.sipmId;
	const int rawId   = addr.rawId;
	const int boardID = addr.boardID;
	const int chanID  = addr.chanID;
	const int disk    = addr.disk;
	const int cidx    = addr.cidx;

	const int encodedSparse = addr.encodedSparse;
	const int encodedDense  = addr.encodedDense;

	ensureBoardBooked(disk, boardID);

	// First time this channel appears in data; ensure histograms exist.
	if(!channelSeen_[(size_t)cidx])
	{
		ensureBaselineDistBooked(disk, boardID, chanID);
		ensureRmsDistBooked(disk, boardID, chanID);
		ensureMaxDistBooked(disk, boardID, chanID);
		ensureLiveWaveformBooked(disk, boardID, chanID, rawId, sipmId);

		channelSeen_[(size_t)cidx] = 1u;
		activeRegularChannels_.push_back(cidx);
	}

	if(disk == 0)
		++nFillDisk0_;
	else
		++nFillDisk1_;

	if(h_global_board_vs_channel_)
		h_global_board_vs_channel_->Fill(boardID, chanID);
	if(h_occupancy_sparse_)
		h_occupancy_sparse_->Fill(encodedSparse);
	if(h_occ_board_)
		h_occ_board_->Fill(boardID);
	if(h_occupancy_dense_)
		h_occupancy_dense_->Fill(encodedDense);

	const int bidx = boardIndex(disk, boardID);
	if(bidx >= 0 && bidx < kTotalBoards)
	{
		auto& bh = boardH_[(size_t)bidx];

		if(bh.occ)
			bh.occ->Fill(chanID);

		if(!boardQueuedForSend_[(size_t)bidx])
		{
			updatedBoards_.push_back(bidx);
			boardQueuedForSend_[(size_t)bidx] = 1u;
		}
	}

	updateWaveformStats(wfStats_[(size_t)cidx], wfSize);

	fillQualityMetric(boardID, chanID, QualityMetric::Seen, 1.0, eventBlock);

	DigiFeatures feat;
	if(!extractFeatures(waveform, digi.peakpos(), boardID, chanID, eventBlock, feat))
		return;

	fillQualityMetric(boardID,
	                  chanID,
	                  QualityMetric::AmpPositive,
	                  feat.amp > 0.0 ? 1.0 : 0.0,
	                  eventBlock);

	fillQualityMetric(boardID,
	                  chanID,
	                  QualityMetric::RMSOK,
	                  feat.rms <= kMaxGoodRms ? 1.0 : 0.0,
	                  eventBlock);

	fillQualityMetric(boardID,
	                  chanID,
	                  QualityMetric::SaturationOK,
	                  feat.nSat == 0 ? 1.0 : 0.0,
	                  eventBlock);

	fillQualityMetric(boardID,
	                  chanID,
	                  QualityMetric::SNRGood,
	                  (feat.rms > 0.0 && feat.amp / feat.rms >= kMinGoodSnr) ? 1.0 : 0.0,
	                  eventBlock);

	fillQualityMetric(boardID,
	                  chanID,
	                  QualityMetric::PeakTimeOK,
	                  (feat.peakpos > 1 && feat.peakpos < feat.wfSize - 2) ? 1.0 : 0.0,
	                  eventBlock);

	const bool sipmIndexOK = validIndexedSipmId(sipmId);

	if(sipmIndexOK)
		incMultiplicity(sipmId);
	else
		recordOutOfRangeSipmId(sipmId);

	if(h_baseline_dist_[(size_t)cidx])
		h_baseline_dist_[(size_t)cidx]->Fill(feat.baseline);
	if(h_rms_dist_[(size_t)cidx])
		h_rms_dist_[(size_t)cidx]->Fill(feat.rms);
	if(h_max_dist_[(size_t)cidx])
		h_max_dist_[(size_t)cidx]->Fill(feat.ampRaw);

	ensureFirstHitBooked(
	    disk, boardID, chanID, rawId, sipmId, waveform, wfSize, feat.baseline);

	fillGlobalWaveformDensity(waveform, wfSize);

	if(h_amp_sparse_)
		h_amp_sparse_->Fill(encodedSparse, feat.amp);
	if(h_amp_dense_)
		h_amp_dense_->Fill(encodedDense, feat.amp);
	if(h_baseline_sparse_)
		h_baseline_sparse_->Fill(encodedSparse, feat.baseline);
	if(h_baseline_dense_)
		h_baseline_dense_->Fill(encodedDense, feat.baseline);
	if(h_rms_sparse_)
		h_rms_sparse_->Fill(encodedSparse, feat.rms);
	if(h_rms_dense_)
		h_rms_dense_->Fill(encodedDense, feat.rms);
	if(h_maxval_sparse_)
		h_maxval_sparse_->Fill(encodedSparse, feat.ampRaw);
	if(h_maxval_dense_)
		h_maxval_dense_->Fill(encodedDense, feat.ampRaw);

	if(bidx >= 0 && bidx < kTotalBoards)
	{
		auto& bh = boardH_[(size_t)bidx];

		if(bh.base)
			bh.base->Fill(chanID, feat.baseline);
		if(bh.rms)
			bh.rms->Fill(chanID, feat.rms);
		if(bh.max)
			bh.max->Fill(chanID, feat.ampRaw);
	}

	if(sipmIndexOK)
	{
		if(modeEnabled(MapMode::Amp))
			accDisk(MapMode::Amp, disk, sipmId, feat.amp);
		if(modeEnabled(MapMode::Baseline))
			accDisk(MapMode::Baseline, disk, sipmId, feat.baseline);
		if(modeEnabled(MapMode::RMS))
			accDisk(MapMode::RMS, disk, sipmId, feat.rms);
	}

	if(h_pulse_shape_score_)
		h_pulse_shape_score_->Fill(feat.shapeScore);
	if(h_pulse_shape_score_board_)
		h_pulse_shape_score_board_->Fill(boardID, feat.shapeScore);

	fillWaveformHealth(boardID, chanID, feat, stats, eventBlock);

	if(h_amp_rms_)
		h_amp_rms_->Fill(feat.rms, feat.amp);
	if(h_snr_board_ && feat.rms > 0.0)
		h_snr_board_->Fill(boardID, feat.amp / feat.rms);
	if(h_amp_block_)
		h_amp_block_->Fill(eventBlock, feat.amp);
	if(h_peak_amp_)
		h_peak_amp_->Fill(feat.peakpos, feat.amp);
	if(h_amp_dist_)
		h_amp_dist_->Fill(feat.amp);

	if(feat.amp < kAmpHistMin || feat.amp >= kAmpHistMax)
		++nAmpOverflow_;

	if(feat.amp > 0.0)
	{
		stats.sumRegularAmp += feat.amp;
		++stats.nRegularAmp;
	}

	SipmFeat cand{};
	cand.amp       = feat.amp;
	cand.baseline  = feat.baseline;
	cand.rms       = feat.rms;
	cand.ampRaw    = feat.ampRaw;
	cand.peakpos   = feat.peakpos;
	cand.timeResid = feat.timeResid;
	cand.snr       = (feat.rms > 0.0) ? feat.amp / feat.rms : 0.0;
	cand.rawId     = rawId;
	cand.disk      = disk;
	cand.board     = boardID;
	cand.chan      = chanID;
	cand.cidx      = cidx;

	packRepWaveform(cand, waveform, feat.baseline, wfSize);

	if(sipmIndexOK)
	{
		addPairCandidate(sipmId, cand);

		if(!featSeen(sipmId))
		{
			markFeat(sipmId, cand);
			repSipmIds_.push_back(sipmId);
		}
		else if(betterRep(cand, feat_[(size_t)sipmId]))
		{
			markFeat(sipmId, cand);
		}
	}
}

void CaloDigiDQM::fillIssue(int boardID, IssueType issue)
{
	if(h_issue_board_)
		h_issue_board_->Fill(boardID, (int)issue);

	if(h_dqm_issue_counts_)
		h_dqm_issue_counts_->Fill((int)issue);
}

void CaloDigiDQM::fillHealth(int         boardID,
                             double      healthScore,
                             EventStats& stats,
                             int         eventBlock)
{
	const double h = std::max(0.0, std::min(1.0, healthScore));

	if(h_health_board_)
		h_health_board_->Fill(boardID, h);

	if(h_health_block_)
		h_health_block_->Fill(eventBlock, h);

	stats.sumHealth += h;
	++stats.nHealthSamples;
}

void CaloDigiDQM::fillWaveformHealth(
    int boardID, int chanID, DigiFeatures const& feat, EventStats& stats, int eventBlock)
{
	double badness = 0.0;

	auto markIssue = [&](IssueType issue) {
		fillIssue(boardID, issue);
		fillBoardIssueMetric(boardID, chanID, issue);
	};

	if(feat.nSat > 0)
	{
		badness += kBadnessSaturation;
		++stats.nSatWaveforms;
		markIssue(IssueType::Sat);
	}

	if(feat.peakpos <= 1 || feat.peakpos >= feat.wfSize - 2)
	{
		badness += kBadnessEdgePeak;
		++stats.nEdgePeaks;
		markIssue(IssueType::EdgePeak);
	}

	if(feat.rms > kMaxGoodRms)
	{
		badness += kBadnessHighRms;
		++stats.nHighRms;
		markIssue(IssueType::HighRMS);
	}

	if(feat.rms > 0.0 && feat.amp / feat.rms < kMinGoodSnr)
	{
		badness += kBadnessLowSnr;
		++stats.nLowSnr;
		markIssue(IssueType::LowSNR);
	}

	if(feat.amp < 0.0)
	{
		badness += kBadnessNegAmp;
		++stats.nNegativeAmp;
		markIssue(IssueType::NegAmp);
	}

	if(feat.shapeScore > kMaxGoodShapeScore)
	{
		badness += kBadnessBadShape;
		markIssue(IssueType::BadShape);
	}

	badness = std::max(0.0, std::min(1.0, badness));

	fillQualityMetric(
	    boardID, chanID, QualityMetric::WaveformHealth, 1.0 - badness, eventBlock);

	if(h_badness_board_channel_)
		h_badness_board_channel_->Fill(boardID, chanID, badness);

	if(h_shape_class_board_)
		h_shape_class_board_->Fill(boardID, (int)feat.shapeClass);

	if(h_shape_class_channel_)
	{
		const int encodedDense = encodeDense(boardID, chanID);
		h_shape_class_channel_->Fill(encodedDense, (int)feat.shapeClass);
	}

	fillHealth(boardID, 1.0 - badness, stats, eventBlock);
}

void CaloDigiDQM::addPairCandidate(int sid, SipmFeat const& f)
{
	if(!validIndexedSipmId(sid))
		return;

	const size_t idx = (size_t)sid;

	if(pairCandidateStamp_[idx] != pairStamp_)
	{
		pairCandidateStamp_[idx] = pairStamp_;
		pairCandidates_[idx].clear();
		pairCandidateSipmIds_.push_back(sid);
	}

	pairCandidates_[idx].push_back(f);
}

bool CaloDigiDQM::selectBestTimeMatchedPair(int       evenId,
                                            int       oddId,
                                            SipmFeat& bestEven,
                                            SipmFeat& bestOdd) const
{
	if(!pairCandidateSeen(evenId) || !pairCandidateSeen(oddId))
		return false;

	const auto& evens = pairCandidates_[(size_t)evenId];
	const auto& odds  = pairCandidates_[(size_t)oddId];

	bool   haveBest         = false;
	bool   bestBothPositive = false;
	double bestDt           = std::numeric_limits<double>::infinity();
	double bestCenterResid  = std::numeric_limits<double>::infinity();
	double bestAmpSum       = -std::numeric_limits<double>::infinity();
	double bestSnrSum       = -std::numeric_limits<double>::infinity();

	for(auto const& e : evens)
	{
		if(e.peakpos < 0)
			continue;

		for(auto const& o : odds)
		{
			if(o.peakpos < 0)
				continue;

			const bool   bothPositive = (e.amp > 0.0 && o.amp > 0.0);
			const double dt           = std::abs((double)e.peakpos - (double)o.peakpos);
			const double centerResid  = std::abs(0.5 * (e.timeResid + o.timeResid));
			const double ampSum       = e.amp + o.amp;
			const double snrSum       = e.snr + o.snr;

			bool better = false;

			if(!haveBest)
			{
				better = true;
			}
			else if(bothPositive != bestBothPositive)
			{
				better = bothPositive;
			}
			else if(dt != bestDt)
			{
				better = dt < bestDt;
			}
			else if(centerResid != bestCenterResid)
			{
				better = centerResid < bestCenterResid;
			}
			else if(ampSum != bestAmpSum)
			{
				better = ampSum > bestAmpSum;
			}
			else if(snrSum != bestSnrSum)
			{
				better = snrSum > bestSnrSum;
			}

			if(better)
			{
				haveBest         = true;
				bestBothPositive = bothPositive;
				bestDt           = dt;
				bestCenterResid  = centerResid;
				bestAmpSum       = ampSum;
				bestSnrSum       = snrSum;
				bestEven         = e;
				bestOdd          = o;
			}
		}
	}

	return haveBest;
}

void CaloDigiDQM::processPairs(EventStats& stats, int eventBlock)
{
	// PairOK is 1 only when the adjacent even/odd regular SiPM partner exists
	// and the selected time-matched pair is on the same disk.
	for(int sipmId : pairCandidateSipmIds_)
	{
		if(!validIndexedSipmId(sipmId))
			continue;

		if(!featSeen(sipmId))
			continue;

		const auto& f = feat_[(size_t)sipmId];

		if(f.cidx < 0)
			continue;

		const int  partnerId  = (sipmId % 2 == 0) ? sipmId + 1 : sipmId - 1;
		const bool hasPartner = pairCandidateSeen(partnerId);

		if(!hasPartner)
		{
			fillQualityMetric(f.board, f.chan, QualityMetric::PairOK, 0.0, eventBlock);

			if(h_pair_ok_board_)
				h_pair_ok_board_->Fill(f.board, 0.0);

			++stats.nUnpairedSipms;
			fillIssue(f.board, IssueType::PairMiss);
			fillBoardIssueMetric(f.board, f.chan, IssueType::PairMiss);
			fillHealth(f.board, kPairMissHealth, stats, eventBlock);
		}
	}

	if(h_unpaired_evt_)
		h_unpaired_evt_->Fill(stats.nUnpairedSipms);

	if(h_pair_miss_frac_block_ && !pairCandidateSipmIds_.empty())
	{
		const double pairMissFrac =
		    (double)stats.nUnpairedSipms / (double)pairCandidateSipmIds_.size();
		h_pair_miss_frac_block_->Fill(eventBlock, pairMissFrac);
	}

	// Pairing convention:
	//   adjacent even/odd SiPM IDs belong to the same crystal:
	//     even SiPM ID = one side
	//     odd  SiPM ID = partner side
	//     crystalId    = sipmId / 2
	//
	// If multiple usable digis exist for either side, choose the even/odd pair
	// using time matching first. Positive amplitudes are preferred, then closest
	// peak time, then smallest center residual. Amplitude and SNR are tie-breakers.
	for(int sipmId : pairCandidateSipmIds_)
	{
		if(!validIndexedSipmId(sipmId))
			continue;

		const int crystalId = sipmId / 2;
		if(paired(crystalId))
			continue;

		const int evenId = 2 * crystalId;
		const int oddId  = evenId + 1;

		if(!(pairCandidateSeen(evenId) && pairCandidateSeen(oddId)))
			continue;

		SipmFeat fL;
		SipmFeat fR;

		if(!selectBestTimeMatchedPair(evenId, oddId, fL, fR))
			continue;

		// Sanity check: a physical L/R SiPM pair should not cross disks.
		if(fL.disk != fR.disk)
		{
			recordSkip(SkipReason::PairDiskMismatch);
			++stats.nPairDiskMismatches;
			markPaired(crystalId);

			fillIssue(fL.board, IssueType::PairDiskMismatch);
			fillIssue(fR.board, IssueType::PairDiskMismatch);

			fillBoardIssueMetric(fL.board, fL.chan, IssueType::PairDiskMismatch);
			fillBoardIssueMetric(fR.board, fR.chan, IssueType::PairDiskMismatch);

			fillQualityMetric(fL.board, fL.chan, QualityMetric::PairOK, 0.0, eventBlock);
			fillQualityMetric(fR.board, fR.chan, QualityMetric::PairOK, 0.0, eventBlock);

			if(h_pair_ok_board_)
			{
				h_pair_ok_board_->Fill(fL.board, 0.0);
				h_pair_ok_board_->Fill(fR.board, 0.0);
			}

			fillHealth(fL.board, kPairMissHealth, stats, eventBlock);
			fillHealth(fR.board, kPairMissHealth, stats, eventBlock);

			const uint64_t nMismatch = skipCounts_[(int)SkipReason::PairDiskMismatch];

			if(nMismatch <= 10)
			{
				mf::LogWarning("CaloDigiDQM")
				    << "Disk mismatch for paired crystal " << crystalId << " (SiPM "
				    << evenId << " rawId=" << fL.rawId << " board=" << fL.board
				    << " chan=" << fL.chan << " disk=" << fL.disk << ", SiPM " << oddId
				    << " rawId=" << fR.rawId << " board=" << fR.board
				    << " chan=" << fR.chan << " disk=" << fR.disk
				    << "). Skipping asymmetry for this pair.";
			}
			else if(nMismatch == 11)
			{
				mf::LogWarning("CaloDigiDQM")
				    << "Further PairDiskMismatch warnings suppressed. "
				    << "Total count will still be recorded in h_skip_reason and "
				       "endJob().";
			}

			continue;
		}

		fillQualityMetric(fL.board, fL.chan, QualityMetric::PairOK, 1.0, eventBlock);
		fillQualityMetric(fR.board, fR.chan, QualityMetric::PairOK, 1.0, eventBlock);

		if(h_pair_ok_board_)
		{
			h_pair_ok_board_->Fill(fL.board, 1.0);
			h_pair_ok_board_->Fill(fR.board, 1.0);
		}

		if(fL.rawId >= 0 && fR.rawId >= 0 && fL.rawId != kUnmappedRawId &&
		   fR.rawId != kUnmappedRawId)
		{
			if(h_pair_raw_delta_)
				h_pair_raw_delta_->Fill(fR.rawId - fL.rawId);

			if(h_pair_board_delta_)
				h_pair_board_delta_->Fill(fL.board, fR.board - fL.board);
		}

		const int multL = getMultiplicity(evenId);
		const int multR = getMultiplicity(oddId);

		if(h_pair_multiplicity_)
			h_pair_multiplicity_->Fill(std::max(multL, multR));

		markPaired(crystalId);

		const double L     = fL.amp;
		const double R     = fR.amp;
		const double denom = L + R;

		if(std::abs(denom) <= kMinDenomForAsym)
		{
			recordSkip(SkipReason::TinyDenomAsym);
			continue;
		}

		const double sumLR = denom;
		const double asym  = (L - R) / denom;

		if(h_lr_corr_)
			h_lr_corr_->Fill(L, R);

		if(h_sum_asym_)
			h_sum_asym_->Fill(sumLR, asym);

		ensureAsymDistBooked(fL.disk, fL.board, fL.chan);
		ensureAsymDistBooked(fR.disk, fR.board, fR.chan);

		const int cidxL = fL.cidx;
		const int cidxR = fR.cidx;

		if(cidxL >= 0 && cidxL < kTotalChannels && h_asym_dist_[(size_t)cidxL])
			h_asym_dist_[(size_t)cidxL]->Fill(asym);

		if(cidxR >= 0 && cidxR < kTotalChannels && h_asym_dist_[(size_t)cidxR])
			h_asym_dist_[(size_t)cidxR]->Fill(asym);

		if(h_asymmetry_)
			h_asymmetry_->Fill(asym);

		if(h_asym_chanid_)
		{
			h_asym_chanid_->Fill(encodeSparse(fL.board, fL.chan), asym);
			h_asym_chanid_->Fill(encodeSparse(fR.board, fR.chan), asym);
		}

		if(h_asym_board_)
		{
			h_asym_board_->Fill(fL.board, asym);
			h_asym_board_->Fill(fR.board, asym);
		}

		if(modeEnabled(MapMode::Sum))
		{
			accDisk(MapMode::Sum, fL.disk, evenId, sumLR);
			accDisk(MapMode::Sum, fR.disk, oddId, sumLR);
		}

		if(modeEnabled(MapMode::Asym))
		{
			accDisk(MapMode::Asym, fL.disk, evenId, asym);
			accDisk(MapMode::Asym, fR.disk, oddId, asym);
		}
	}
}

void CaloDigiDQM::queueRepWaveformsForStreaming()
{
	for(int sipmId : repSipmIds_)
	{
		if(!validIndexedSipmId(sipmId))
			continue;

		const auto& f = feat_[(size_t)sipmId];

		if(f.cidx < 0 || f.cidx >= kTotalChannels)
			continue;

		lastWf_[(size_t)f.cidx]      = f.wfSub;
		lastWfValid_[(size_t)f.cidx] = 1u;

		liveWaveformUpdated_[(size_t)f.cidx] = 1u;

		if(!regularQueuedForSend_[(size_t)f.cidx])
		{
			updatedRegularChannels_.push_back(f.cidx);
			regularQueuedForSend_[(size_t)f.cidx] = 1u;
		}
	}
}

void CaloDigiDQM::queueRepLaserWaveformsForStreaming()
{
	for(int chan : repLaserChannels_)
	{
		if(chan < 0 || chan >= kLaserChannels)
			continue;

		const auto& f = laserRep_[(size_t)chan];

		laserLastWf_[(size_t)chan]      = f.wfSub;
		laserLastWfValid_[(size_t)chan] = 1u;

		laserLiveWaveformUpdated_[(size_t)chan] = 1u;

		if(!laserQueuedForSend_[(size_t)chan])
		{
			updatedLaserChannels_.push_back(chan);
			laserQueuedForSend_[(size_t)chan] = 1u;
		}
	}
}

double CaloDigiDQM::fillLaserNormalization(EventStats& stats, int eventBlock)
{
	const double meanLaserAmp = stats.meanLaserAmp();

	if(stats.nRegularAmp > 0 && meanLaserAmp > 0.0)
	{
		const double meanRegularAmp = stats.sumRegularAmp / (double)stats.nRegularAmp;

		if(h_amp_laser_block_)
			h_amp_laser_block_->Fill(eventBlock, meanRegularAmp / meanLaserAmp);

		++nDetLaserRatioAccepted_;
	}

	if(meanLaserAmp > 0.0)
	{
		for(int sipmId : repSipmIds_)
		{
			if(!validIndexedSipmId(sipmId))
				continue;

			const auto& f = feat_[(size_t)sipmId];

			if(f.cidx < 0)
				continue;

			const double ratio = f.amp / meanLaserAmp;

			if(h_amp_laser_board_channel_)
				h_amp_laser_board_channel_->Fill(f.board, f.chan, ratio);
		}
	}

	return meanLaserAmp;
}

void CaloDigiDQM::fillDqmSummary(EventStats const& stats,
                                 int               eventBlock,
                                 double            meanLaserAmp)
{
	auto setDqmSummaryBin = [&](int bin, double value) {
		if(!h_dqm_summary_)
			return;

		const double v = std::max(0.0, std::min(1.0, value));
		h_dqm_summary_->SetBinContent(bin, v);
	};

	const double hasEventsStatus = 1.0;
	const double hasDigisStatus  = (stats.nDigis > 0) ? 1.0 : 0.0;

	// Pair misses are also included as a mild health penalty so they appear
	// in both pair-completeness and board-health summaries.

	double pairStatus = 1.0;
	if(!pairCandidateSipmIds_.empty())
	{
		const int badPairLikeSipms = stats.nUnpairedSipms + 2 * stats.nPairDiskMismatches;

		pairStatus = 1.0 - std::min(1.0,
		                            (double)badPairLikeSipms /
		                                (double)pairCandidateSipmIds_.size());
	}
	double saturationStatus = 1.0;
	double rmsStatus        = 1.0;

	if(stats.nHealthSamples > 0)
	{
		saturationStatus =
		    1.0 -
		    std::min(1.0, (double)stats.nSatWaveforms / (double)stats.nHealthSamples);

		rmsStatus =
		    1.0 - std::min(1.0, (double)stats.nHighRms / (double)stats.nHealthSamples);
	}

	const double laserStatus = (stats.nLaserAmp > 0) ? 1.0 : 0.0;
	const double detLaserStatus =
	    (stats.nRegularAmp > 0 && meanLaserAmp > 0.0) ? 1.0 : 0.0;
	const double healthStatus =
	    (stats.nHealthSamples > 0) ? stats.sumHealth / (double)stats.nHealthSamples : 1.0;

	const double overallStatus =
	    (hasEventsStatus + hasDigisStatus + pairStatus + saturationStatus + rmsStatus +
	     laserStatus + detLaserStatus + healthStatus) /
	    8.0;

	setDqmSummaryBin(1, hasEventsStatus);
	setDqmSummaryBin(2, hasDigisStatus);
	setDqmSummaryBin(3, pairStatus);
	setDqmSummaryBin(4, saturationStatus);
	setDqmSummaryBin(5, rmsStatus);
	setDqmSummaryBin(6, laserStatus);
	setDqmSummaryBin(7, detLaserStatus);
	setDqmSummaryBin(8, healthStatus);
	setDqmSummaryBin(9, overallStatus);

	if(h_dqm_summary_block_)
		h_dqm_summary_block_->Fill(eventBlock, overallStatus);

	if(h_dqm_run_counters_)
	{
		uint64_t totalSkips = 0;
		for(int i = 0; i < (int)SkipReason::Count; ++i)
			totalSkips += skipCounts_[(size_t)i];

		h_dqm_run_counters_->SetBinContent(1, eventCounter_ + 1);
		h_dqm_run_counters_->SetBinContent(2, stats.nDigis);
		h_dqm_run_counters_->SetBinContent(3, nFillDisk0_);
		h_dqm_run_counters_->SetBinContent(4, nFillDisk1_);
		h_dqm_run_counters_->SetBinContent(5, nFillLaser_);
		h_dqm_run_counters_->SetBinContent(6, (double)totalSkips);
		h_dqm_run_counters_->SetBinContent(7, stats.nUnpairedSipms);
		h_dqm_run_counters_->SetBinContent(8, (double)histTotalSendErrors_);
		h_dqm_run_counters_->SetBinContent(9, (double)nEvtDigisOverflow_);
		h_dqm_run_counters_->SetBinContent(10, (double)nWaveformSizeOverflow_);
		h_dqm_run_counters_->SetBinContent(11, (double)nAmpOverflow_);
	}
}

// ===========================
// Streaming
// ===========================
void CaloDigiDQM::streamIfScheduled()
{
	const bool doSummariesEvent = (freqDQM_ > 0) && (eventCounter_ % freqDQM_ == 0);
	bool doWaveforms = (freqWaveforms_ > 0) && (eventCounter_ % freqWaveforms_ == 0);

	const int  diskMapPeriod = (freqDQM_ > 0) ? (freqDQM_ + kDiskMapsExtraPeriod) : 0;
	const bool doDiskMaps =
	    enableDiskMaps_ && (diskMapPeriod > 0) && (eventCounter_ % diskMapPeriod == 0);

	if(doWaveforms)
	{
		const bool haveWaveformUpdates = !updatedRegularChannels_.empty() ||
		                                 !updatedLaserChannels_.empty() ||
		                                 waveformDensityUpdated_;

		const bool havePendingFirstHits =
		    (pendingRegularFirstHits_ > 0) || (pendingLaserFirstHits_ > 0);

		if(!haveWaveformUpdates && !havePendingFirstHits)
			doWaveforms = false;
	}

	if(!doSummariesEvent && !doWaveforms && !doDiskMaps)
		return;

	if(!sendHists_ || !histSender_)
		return;

	if(doDiskMaps)
	{
		for(auto m : streamModes_)
			refreshDiskMap(m);
	}

	if(doWaveforms)
	{
		flushUpdatedLiveWaveforms();
		flushUpdatedLaserLiveWaveforms();
	}

	std::map<std::string, std::vector<TH1*>>    hists_to_send;
	std::map<std::string, std::vector<TGraph*>> graphs_to_send;

	regularOneHitSentThisCall_.clear();
	laserOneHitSentThisCall_.clear();
	regularLiveSentThisCall_.clear();
	laserLiveSentThisCall_.clear();

	if(doSummariesEvent)
	{
		auto& g             = hists_to_send[streamGlobalPath_];
		auto& dqm           = hists_to_send[streamDqmSummaryPath_];
		auto& shifter       = hists_to_send[streamShifterPath_];
		auto& shifterGraphs = graphs_to_send[streamShifterPath_];

		updateNormalizedOccHistograms();

		addHist(shifter, h_occ_board_norm_);
		addGraph(shifterGraphs, g_nhits_ewt_);
		addHist(shifter, h_amp_sparse_);
		addHist(shifter, h_asym_chanid_);

		addHist(dqm, h_dqm_summary_);
		addHist(dqm, h_dqm_issue_counts_);
		addHist(dqm, h_dqm_run_counters_);
		addHist(dqm, h_dqm_summary_block_);
		addHist(dqm, h_health_board_);
		addHist(dqm, h_health_block_);
		addHist(dqm, h_issue_board_);
		addHist(dqm, h_pair_ok_board_);
		addHist(dqm, h_pair_raw_delta_);
		addHist(dqm, h_pair_board_delta_);
		addHist(dqm, h_pair_miss_frac_block_);
		addHist(dqm, h_pulse_shape_score_);
		addHist(dqm, h_pulse_shape_score_board_);
		addHist(dqm, h_amp_laser_board_channel_);
		addHist(dqm, h_amp_laser_block_);
		addHist(dqm, h_badness_board_channel_);
		addHist(dqm, h_time_resid_board_channel_);
		for(int disk = 0; disk < kNDisks; ++disk)
			addHist(dqm, h_disk_quality_matrix_[(size_t)disk]);

		addHist(dqm, h_time_resid_board_);
		addHist(dqm, h_time_resid_dist_);
		addHist(dqm, h_shape_class_board_);
		addHist(dqm, h_shape_class_channel_);

		addHist(g, h_occupancy_sparse_norm_);
		//addHist(g, ref_h_occ_dense_);
		addHist(g, h_occupancy_dense_norm_);
		addHist(g, h_baseline_sparse_);
		addHist(g, ref_h_base_dense_);
		addHist(g, h_baseline_dense_);
		addHist(g, h_rms_sparse_);
		addHist(g, ref_h_rms_dense_);
		addHist(g, h_rms_dense_);
		addHist(g, h_maxval_sparse_);
		addHist(g, ref_h_max_dense_);
		addHist(g, h_maxval_dense_);
		addHist(g, ref_h_asym_);
		addHist(g, h_asymmetry_);
		addHist(g, h_global_board_vs_channel_norm_);
		addHist(g, h_waveform_size_);
		addHist(g, h_pair_multiplicity_);
		addHist(g, h_skip_reason_);
		addHist(g, h_evt_digis_);
		addHist(g, h_digis_block_);
		addHist(g, h_peak_amp_);
		addHist(g, h_sat_samples_);
		addHist(g, h_sat_frac_board_);
		addHist(g, h_amp_rms_);
		addHist(g, h_snr_board_);
		addHist(g, h_base_block_);
		addHist(g, h_rms_block_);
		addHist(g, h_amp_block_);
		addHist(g, h_laser_block_);
		addHist(g, h_lr_corr_);
		addHist(g, h_sum_asym_);
		addHist(g, h_unpaired_evt_);
		addHist(g, h_asym_board_);

		if(laserBoardUpdated_)
		{
			auto& lg = hists_to_send[streamLaserBoardPath_];
			addHist(lg, laserBoardH_.occNorm);
			addHist(lg, laserBoardH_.base);
			addHist(lg, laserBoardH_.rms);
			addHist(lg, laserBoardH_.max);
		}

		for(int bidx : updatedBoards_)
		{
			const int disk    = bidx / kBoardsPerDisk;
			const int blocal  = bidx % kBoardsPerDisk;
			const int boardID = boardIdFromDiskAndLocal(disk, blocal);

			const std::string& groupPath  = streamBoardPath_[(size_t)bidx];
			auto&              boardGroup = hists_to_send[groupPath];

			if(disk == 0 && boardID == 27)
			{
				addHist(boardGroup, ref_D0_B027_Occupancy_);
				addHist(boardGroup, ref_D0_B027_Baseline_);
				addHist(boardGroup, ref_D0_B027_RMS_);
				addHist(boardGroup, ref_D0_B027_Max_);
			}

			const auto& bh = boardH_[(size_t)bidx];
			addHist(boardGroup, bh.occNorm);
			addHist(boardGroup, bh.base);
			addHist(boardGroup, bh.rms);
			addHist(boardGroup, bh.max);
			addHist(boardGroup, bh.quality);
			addHist(boardGroup, bh.issue);

			// Large object: saved to ROOT, not streamed by default.
			// addHist(boardGroup, bh.healthTrend);
		}
	}

	if(doDiskMaps)
	{
		for(auto m : streamModes_)
		{
			if(!streamModeEnabled(m))
				continue;

			const size_t       midx      = modeIndex(m);
			const std::string& groupPath = streamDiskMapPath_[midx];

			addHist(hists_to_send[groupPath], disk0Maps_[midx]);
			addHist(hists_to_send[groupPath], disk1Maps_[midx]);
		}
	}

	if(doWaveforms)
	{
		for(int chan : updatedLaserChannels_)
		{
			const std::string& livePath = streamLaserLivePath_[(size_t)chan];

			if(laserLiveWf_[(size_t)chan] && laserLiveWaveformUpdated_[(size_t)chan])
			{
				hists_to_send[livePath].push_back(laserLiveWf_[(size_t)chan]);
				laserLiveSentThisCall_.push_back(chan);
			}
		}

		for(int chan : activeLaserChannels_)
		{
			const std::string& oneHitPath = streamLaserOneHitPath_[(size_t)chan];

			if(laserOneHitWf_[(size_t)chan] && !laserOneHitSent_[(size_t)chan])
			{
				hists_to_send[oneHitPath].push_back(laserOneHitWf_[(size_t)chan]);
				laserOneHitSentThisCall_.push_back(chan);
			}
		}

		for(int cidx : updatedRegularChannels_)
		{
			const int disk    = diskFromCidx(cidx);
			const int enc     = encodedFromCidx(cidx);
			const int blocal  = boardLocalFromEncoded(enc);
			const int chan    = chanFromEncoded(enc);
			const int boardID = boardIdFromDiskAndLocal(disk, blocal);

			const std::string& livePath = streamLivePath_[(size_t)cidx];

			if(disk == 0 && boardID == 27 && chan == 0)
			{
				if(ref_D0_B027_C00_Waveform_)
					hists_to_send[livePath].push_back(ref_D0_B027_C00_Waveform_);
			}

			if(liveWf_[(size_t)cidx] && liveWaveformUpdated_[(size_t)cidx])
			{
				hists_to_send[livePath].push_back(liveWf_[(size_t)cidx]);
				regularLiveSentThisCall_.push_back(cidx);
			}
		}

		for(int cidx : activeRegularChannels_)
		{
			const std::string& oneHitPath = streamOneHitPath_[(size_t)cidx];

			if(oneHitWf_[(size_t)cidx] && !oneHitSent_[(size_t)cidx])
			{
				hists_to_send[oneHitPath].push_back(oneHitWf_[(size_t)cidx]);
				regularOneHitSentThisCall_.push_back(cidx);
			}
		}

		if(h_global_waveform_density_ && waveformDensityUpdated_)
			hists_to_send[streamWaveformDensityPath_].push_back(
			    h_global_waveform_density_);
	}

	if(hists_to_send.empty() && graphs_to_send.empty())
		return;

	try
	{
		if(!hists_to_send.empty())
			histSender_->sendHistograms(hists_to_send);

		if(!graphs_to_send.empty())
			histSender_->sendGraphs(graphs_to_send);
		histConsecutiveSendErrors_ = 0;

		if(doWaveforms && waveformDensityUpdated_)
		{
			auto it = hists_to_send.find(streamWaveformDensityPath_);
			if(it != hists_to_send.end() && !it->second.empty())
				waveformDensityUpdated_ = false;
		}

		for(int cidx : regularOneHitSentThisCall_)
		{
			if(!oneHitSent_[(size_t)cidx])
			{
				oneHitSent_[(size_t)cidx] = 1u;
				if(pendingRegularFirstHits_ > 0)
					--pendingRegularFirstHits_;
			}
		}

		for(int chan : laserOneHitSentThisCall_)
		{
			if(!laserOneHitSent_[(size_t)chan])
			{
				laserOneHitSent_[(size_t)chan] = 1u;
				if(pendingLaserFirstHits_ > 0)
					--pendingLaserFirstHits_;
			}
		}

		for(int cidx : regularLiveSentThisCall_)
			liveWaveformUpdated_[(size_t)cidx] = 0u;

		for(int chan : laserLiveSentThisCall_)
			laserLiveWaveformUpdated_[(size_t)chan] = 0u;

		if(doSummariesEvent)
			clearSummarySendQueues();

		if(doWaveforms)
			clearWaveformSendQueues();
	}
	catch(const std::exception& e)
	{
		++histConsecutiveSendErrors_;
		++histTotalSendErrors_;

		mf::LogError("CaloDigiDQM")
		    << "HistoSender::sendHistograms exception (consecutive="
		    << histConsecutiveSendErrors_ << ", total=" << histTotalSendErrors_
		    << "): " << e.what();

		// Keep queues after transient send failures to allow retry.
		// Queued flags prevent duplicate growth; queues are cleared if streaming is disabled.
		if(histConsecutiveSendErrors_ >= kMaxSendErrors_)
		{
			clearSummarySendQueues();
			clearWaveformSendQueues();
			sendHists_ = false;
			histSender_.reset();

			mf::LogWarning("CaloDigiDQM")
			    << "Histogram streaming disabled after " << histConsecutiveSendErrors_
			    << " consecutive send errors.";
		}
	}
	catch(...)
	{
		++histConsecutiveSendErrors_;
		++histTotalSendErrors_;

		mf::LogError("CaloDigiDQM") << "HistoSender::sendHistograms non-std exception "
		                            << "(consecutive=" << histConsecutiveSendErrors_
		                            << ", total=" << histTotalSendErrors_ << ").";

		// Keep queues after transient send failures to allow retry.
		// Queued flags prevent duplicate growth; queues are cleared if streaming is disabled.
		if(histConsecutiveSendErrors_ >= kMaxSendErrors_)
		{
			clearSummarySendQueues();
			clearWaveformSendQueues();
			sendHists_ = false;
			histSender_.reset();

			mf::LogWarning("CaloDigiDQM")
			    << "Histogram streaming disabled after " << histConsecutiveSendErrors_
			    << " consecutive send errors.";
		}
	}
}

// ===========================
// endJob()
// ===========================
void CaloDigiDQM::endJob()
{
	// Final flush ensures ROOT output captures last cached values.
	flushHitsAveragePoint();
	flushAllLiveWaveforms();
	flushAllLaserLiveWaveforms();
	refreshDiskMaps();
	updateNormalizedOccHistograms();

	std::ostringstream skips;
	skips << " skipCounts={";
	for(int i = 0; i < (int)SkipReason::Count; ++i)
	{
		if(i)
			skips << ", ";
		skips << skipLabel((SkipReason)i) << ":" << skipCounts_[(size_t)i];
	}
	skips << "}";

	mf::LogInfo("CaloDigiDQM") << "CaloDigiDQM summary:"
	                           << " events=" << eventCounter_ << " d0=" << nFillDisk0_
	                           << " d1=" << nFillDisk1_ << " laser=" << nFillLaser_
	                           << " miss=" << nFillMiss_
	                           << " consecutiveSendErr=" << histConsecutiveSendErrors_
	                           << " totalSendErr=" << histTotalSendErrors_
	                           << " outOfRangeSipmId=" << nOutOfRangeSipmId_
	                           << skips.str();

	struct Row
	{
		int               cidx;
		WaveformSizeStats st;
	};

	std::vector<Row> offenders;
	offenders.reserve((size_t)kTotalChannels);

	size_t seenCount = 0;
	for(int cidx : activeRegularChannels_)
	{
		++seenCount;
		const auto& st = wfStats_[(size_t)cidx];
		if(st.nSeen && st.min != st.max)
			offenders.push_back(Row{cidx, st});
	}

	std::sort(offenders.begin(), offenders.end(), [](const Row& a, const Row& b) {
		if(a.st.nTransitions != b.st.nTransitions)
			return a.st.nTransitions > b.st.nTransitions;

		const uint32_t ra = a.st.max - a.st.min;
		const uint32_t rb = b.st.max - b.st.min;
		if(ra != rb)
			return ra > rb;

		return a.st.nMismatchToFirst > b.st.nMismatchToFirst;
	});

	mf::LogInfo("CaloDigiDQM") << "Waveform-size summary:"
	                           << " channels_seen=" << seenCount
	                           << " variable=" << offenders.size()
	                           << " nbins=" << kWaveformNBins;

	const size_t top = std::min<size_t>(10, offenders.size());
	if(top)
	{
		std::ostringstream os;
		os << "Top " << top << " variable-size channels:\n";

		for(size_t i = 0; i < top; ++i)
		{
			const auto& r       = offenders[i];
			const int   disk    = diskFromCidx(r.cidx);
			const int   enc     = encodedFromCidx(r.cidx);
			const int   blocal  = boardLocalFromEncoded(enc);
			const int   chan    = chanFromEncoded(enc);
			const int   boardID = boardIdFromDiskAndLocal(disk, blocal);

			os << "  (D" << disk << " B" << boardID << " C" << chan << ")"
			   << " first=" << r.st.first << " min=" << r.st.min << " max=" << r.st.max
			   << " seen=" << r.st.nSeen << " trans=" << r.st.nTransitions
			   << " mismatch=" << r.st.nMismatchToFirst << " pad=" << r.st.nPadded
			   << " trunc=" << r.st.nTruncated << "\n";
		}

		mf::LogInfo("CaloDigiDQM") << os.str();
	}

	struct LaserRow
	{
		int               chan;
		WaveformSizeStats st;
	};

	std::vector<LaserRow> laserOffenders;
	laserOffenders.reserve((size_t)kLaserChannels);

	size_t laserSeenCount = 0;
	for(int chan : activeLaserChannels_)
	{
		++laserSeenCount;
		const auto& st = laserWfStats_[(size_t)chan];
		if(st.nSeen && st.min != st.max)
			laserOffenders.push_back(LaserRow{chan, st});
	}

	std::sort(laserOffenders.begin(),
	          laserOffenders.end(),
	          [](const LaserRow& a, const LaserRow& b) {
		          if(a.st.nTransitions != b.st.nTransitions)
			          return a.st.nTransitions > b.st.nTransitions;

		          const uint32_t ra = a.st.max - a.st.min;
		          const uint32_t rb = b.st.max - b.st.min;
		          if(ra != rb)
			          return ra > rb;

		          return a.st.nMismatchToFirst > b.st.nMismatchToFirst;
	          });

	mf::LogInfo("CaloDigiDQM") << "Laser waveform-size summary:"
	                           << " channels_seen=" << laserSeenCount
	                           << " variable=" << laserOffenders.size()
	                           << " nbins=" << kWaveformNBins;

	const size_t laserTop = std::min<size_t>(10, laserOffenders.size());
	if(laserTop)
	{
		std::ostringstream os;
		os << "Top " << laserTop << " variable-size laser channels:\n";

		for(size_t i = 0; i < laserTop; ++i)
		{
			const auto& r = laserOffenders[i];

			os << "  (Laser B160 C" << r.chan << ")"
			   << " first=" << r.st.first << " min=" << r.st.min << " max=" << r.st.max
			   << " seen=" << r.st.nSeen << " trans=" << r.st.nTransitions
			   << " mismatch=" << r.st.nMismatchToFirst << " pad=" << r.st.nPadded
			   << " trunc=" << r.st.nTruncated << "\n";
		}

		mf::LogInfo("CaloDigiDQM") << os.str();
	}
}

}  // namespace mu2e

DEFINE_ART_MODULE(mu2e::CaloDigiDQM);
