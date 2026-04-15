////////////////////////////////////////////////////////////////////////////////////
// CaloDigiDQM_module.cc
//
// Mu2e calorimeter DQM analyzer.
//
// Responsibilities:
//   - read CaloDigi objects
//   - map offline SiPM IDs to electronics IDs via CaloDAQMap
//   - fill ROOT histograms for global, disk, board, channel, and laser monitoring
//   - optionally stream selected histograms to otsdaq via HistoSender
//
// Maintainer notes:
//   - boardID is a global board index across both disks
//   - channelIndex(...) returns a compact global channel index
//   - disk maps are always saved to ROOT
//   - enableDiskMaps affects streaming only
//   - board 160 is treated as the dedicated laser board and is handled separately
//     from regular disk/board/channel monitoring
//   - reference histograms are optional; missing reference files or objects do not
//     stop the module
//   - live waveform streaming is update-driven to reduce network load
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
#include "TH1F.h"
#include "TH1I.h"
#include "TH2D.h"
#include "TH2I.h"
#include "TProfile.h"
#include "TString.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "cetlib_except/exception.h"

namespace mu2e
{
class CaloDigiDQM : public art::EDAnalyzer
{
  public:
	struct Config
	{
		// otsdaq destination and top-level namespace for streamed histograms
		fhicl::Atom<std::string> address{fhicl::Name("address")};
		fhicl::Atom<int>         port{fhicl::Name("port")};
		fhicl::Atom<std::string> moduleTag{fhicl::Name("moduleTag")};
		fhicl::Atom<bool>        sendHists{fhicl::Name("sendHists")};

		// Event cadence for streaming
		// freqDQM controls summary histograms
		// freqWaveforms controls live waveform streaming
		fhicl::Atom<int> freqDQM{fhicl::Name("freqDQM")};
		fhicl::Atom<int> freqWaveforms{
		    fhicl::Name("freqWaveforms")};  // 0 = disable waveform streaming

		// art input tag label for the CaloDigiCollection
		fhicl::Atom<std::string> caloDigiModuleLabel{fhicl::Name("caloDigiModuleLabel")};

		// Disk maps are always saved to the ROOT file
		// This flag controls disk-map streaming only
		fhicl::Atom<bool> enableDiskMaps{fhicl::Name("enableDiskMaps")};

		// Disk map streaming selection only, examples {"asym"} or {"asym","sum"}
		// Disk maps are streamed less frequently than summary histograms
		fhicl::Sequence<std::string> diskCombines{fhicl::Name("diskCombines")};
		// Optional reference ROOT file for comparison histograms
		fhicl::Atom<bool>        useReferenceFile{fhicl::Name("useReferenceFile")};
		fhicl::Atom<std::string> referenceFile{fhicl::Name("referenceFile")};
	};

	explicit CaloDigiDQM(const art::EDAnalyzer::Table<Config>& config);
	void analyze(art::Event const& event) override;
	void endJob() override;

  private:
	// -----------------------
	// Fixed waveform settings
	// -----------------------
	static constexpr int kWaveformNBins =
	    64;  // Number of bins used for live and first-hit waveform histograms.
	static constexpr int kWaveformSizeHistMax = 200;  // Cap for size histogram axis

	// -----------------------
	// Disk-map streaming cadence
	// -----------------------
	// Disk maps are streamed less frequently than summary histograms
	// This value is added on top of freqDQM to reduce refresh cost
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

	// Short suffix used for object names and streaming groups
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
		return "Amp";
	}

	// Centralize titles and display options for disk maps
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

	static constexpr int kTotalBoards   = kNDisks * kBoardsPerDisk;    // 160
	static constexpr int kTotalChannels = kNDisks * kChannelsPerDisk;  // 3200

	// First board ID for a given disk in the global board ID space
	static int boardMinForDisk(int disk) { return disk * kBoardsPerDisk; }

	// Contiguous channel index within disk, computed from boardID and chanID
	static int encodeChannel(int disk, int boardID, int chanID)
	{
		const int bmin = boardMinForDisk(disk);
		return (boardID - bmin) * kChannelsPerBoard + chanID;
	}

	static int encodeSparse(int boardID, int chanID) { return boardID * 100 + chanID; }

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
		return EncodedAxisConfig{15920, 0.0, 15920.0};
	}

	static EncodedAxisConfig axisDenseGlobal()
	{
		return EncodedAxisConfig{kTotalChannels, 0.0, (double)kTotalChannels};
	}

	// Flatten disk and board ID into a compact board index for arrays
	static int boardIndex(int disk, int boardID)
	{
		const int local = boardID - boardMinForDisk(disk);  // 0 to 79
		return disk * kBoardsPerDisk + local;               // 0 to 159
	}

	// Global channel index across both disks
	static int channelIndex(int disk, int boardID, int chanID)
	{
		return disk * kChannelsPerDisk +
		       encodeChannel(disk, boardID, chanID);  // 0 to 3199
	}

	// Decode helpers used for reporting and endJob summaries
	static int diskFromCidx(int cidx) { return cidx / kChannelsPerDisk; }
	static int encodedFromCidx(int cidx) { return cidx % kChannelsPerDisk; }
	static int boardLocalFromEncoded(int enc) { return enc / kChannelsPerBoard; }
	static int chanFromEncoded(int enc) { return enc % kChannelsPerBoard; }
	static int boardIdFromDiskAndLocal(int disk, int blocal)
	{
		return boardMinForDisk(disk) + blocal;
	}

	// -----------------------
	// Skip instrumentation
	// -----------------------
	enum class SkipReason : int
	{
		BadSipmId = 0,
		UnmappedRawId,
		RawIdNegative,
		DiskOutOfRange,
		PeakPosOutOfRange,
		NonFiniteBaselineOrRms,
		TinyDenomAsym,
		Count
	};

	// Cheap counters plus optional per event histogram for diagnostics
	std::array<uint64_t, (int)SkipReason::Count> skipCounts_{};

	TH1I* h_skip_reason_{nullptr};

	// Record a skip reason consistently in both counters and histogram
	inline void recordSkip(SkipReason r)
	{
		skipCounts_[(int)r]++;
		if(h_skip_reason_)
			h_skip_reason_->Fill((int)r);
	}

	// Text labels for skip histogram axis
	static const char* skipLabel(SkipReason r)
	{
		switch(r)
		{
		case SkipReason::BadSipmId:
			return "BadSipmId";
		case SkipReason::UnmappedRawId:
			return "UnmappedRawId";
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
		case SkipReason::Count:
			break;
		}
		return "Unknown";
	}

	// -----------------------
	// Disk-map running mean
	// -----------------------
	static constexpr int kMaxSipmIdForMaps_ = 10000;  // Safety cap for vector sizing
	bool                 warnedBadSipmId_{false};
	int                  nBadSipmId_{0};

	// Accumulate values into sum and count buffers for later mean computation
	void accDisk(MapMode m, int disk, int sipmId, double val)
	{
		if(disk < 0 || disk >= kNDisks)
			return;

		if(sipmId < 0 || sipmId >= kMaxSipmIdForMaps_)
		{
			++nBadSipmId_;
			if(!warnedBadSipmId_)
			{
				warnedBadSipmId_ = true;
				mf::LogWarning("CaloDigiDQM")
				    << "Out-of-range sipmId=" << sipmId << " (cap=" << kMaxSipmIdForMaps_
				    << "). Disk-map accumulation will skip these.";
			}
			return;
		}

		const size_t midx = modeIndex(m);
		auto&        sumv = diskSum_[midx][disk];
		auto&        cntv = diskCnt_[midx][disk];

		sumv[(size_t)sipmId] += val;
		cntv[(size_t)sipmId] += 1u;
	}

	// Materialize disk maps by resetting and refilling from mean buffers
	void refreshDiskMaps()
	{
		for(auto m : kAllModes)
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
	}

	// -----------------------
	// Waveform-size bookkeeping
	// -----------------------
	struct WaveformSizeStats
	{
		uint32_t first{0}, last{0}, min{0}, max{0};
		uint32_t nSeen{0}, nMismatchToFirst{0}, nTransitions{0};
		uint32_t nTruncated{0}, nPadded{0};
	};

	// -----------------------
	// Fast per event pairing
	// -----------------------
	struct SipmFeat
	{
		double                            amp{0.0};
		double                            baseline{0.0};
		double                            rms{0.0};
		double                            ampRaw{0.0};
		int                               disk{-1};
		int                               board{-1};
		int                               chan{-1};
		int                               cidx{-1};
		std::array<float, kWaveformNBins> wfSub{};
	};
	// Stamp based validity avoids per event clearing of large vectors
	int                   pairStamp_{0};
	std::vector<SipmFeat> feat_;       // Indexed by sipmId
	std::vector<int>      featStamp_;  // featStamp[sipmId] equals pairStamp when valid
	std::vector<int> pairedStamp_;  // pairedStamp[crystalId] equals pairStamp when paired
	std::vector<uint16_t>
	    sipmMultiplicity_;  // number of usable digis per sipmId in this event

	// -----------------------
	// Waveform caching
	// -----------------------
	std::vector<std::array<float, kWaveformNBins>> lastWf_;
	std::vector<uint8_t>                           lastWfValid_;

	// Copy cached samples into a live waveform histogram just before streaming
	inline void flushCachedWaveformToHist(TH1F* h, int cidx) const
	{
		if(!h)
			return;
		if(!lastWfValid_[(size_t)cidx])
			return;
		for(int i = 0; i < kWaveformNBins; ++i)
			h->SetBinContent(i + 1, (double)lastWf_[(size_t)cidx][(size_t)i]);
	}

	// Flush all booked live waveform hists from the cached arrays
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

	// -----------------------
	// Helpers
	// -----------------------
	bool modeEnabled(MapMode m) const { return enabledModes_[modeIndex(m)]; }

	bool streamModeEnabled(MapMode m) const { return streamEnabledModes_[modeIndex(m)]; }
	void ensureBaselineDistBooked(int disk, int boardID, int chanID);
	void ensureRmsDistBooked(int disk, int boardID, int chanID);
	void ensureMaxDistBooked(int disk, int boardID, int chanID);
	void ensureAsymDistBooked(int disk, int boardID, int chanID);

	// -----------------------
	// Data members
	// -----------------------

	// Stream modes for otsdaq, selected by diskCombines
	std::vector<MapMode> streamModes_;

	art::InputTag caloDigiTag_;

	int         freqDQM_;
	int         freqWaveforms_;
	std::string address_;
	int         port_;
	std::string moduleTag_;
	bool        sendHists_;

	std::unique_ptr<ots::HistoSender> histSender_;
	int                               eventCounter_{0};
	int                               histSendErrorCount_{0};
	static constexpr int              kMaxSendErrors_ = 10;

	// Streaming toggle for disk maps, saving is always enabled
	bool enableDiskMaps_;

	// -----------------------
	// Reference file
	// -----------------------
	// Reference histograms are optional
	// Missing file or missing objects should not stop the module
	bool        useReferenceFile_;
	std::string referenceFile_;

	// Simple counters for coverage reporting
	int nFillDisk0_{0}, nFillDisk1_{0}, nFillLaser_{0}, nFillMiss_{0};

	// -----------------------
	// Reference histograms
	// -----------------------
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

	// -----------------------
	// Board-level summaries
	// -----------------------
	struct BoardHists
	{
		TH1F*     occ{nullptr};
		TProfile* base{nullptr};
		TProfile* rms{nullptr};
		TProfile* max{nullptr};
	};

	static constexpr int kLaserBoardID  = 160;
	static constexpr int kLaserChannels = kChannelsPerBoard;

	static bool isLaserBoard(int boardID) { return boardID == kLaserBoardID; }

	void ensureLaserBoardBooked();
	void ensureLaserBaselineDistBooked(int chanID);
	void ensureLaserRmsDistBooked(int chanID);
	void ensureLaserMaxDistBooked(int chanID);
	void ensureLaserLiveWaveformBooked(int chanID, int rawId, int sipmId);
	void ensureLaserFirstHitBooked(int         chanID,
	                               int         rawId,
	                               int         sipmId,
	                               auto const& waveform,
	                               int         wfSize,
	                               float       baseline);

	static bool computeBaselineRms(auto const& waveform,
	                               int         wfSize,
	                               float&      baseline,
	                               float&      rms)
	{
		if(wfSize <= 0)
			return false;

		const int nBase = std::min<int>(5, wfSize);

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

	std::array<BoardHists, kTotalBoards>                           boardH_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardHistDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardChanDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardBaselineDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardRmsDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardMaxDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardAsymDir_{};
	std::unique_ptr<art::TFileDirectory>                           laserDir_;
	std::unique_ptr<art::TFileDirectory>                           laserBoardHistDir_;
	std::unique_ptr<art::TFileDirectory>                           laserBoardChanDir_;
	std::unique_ptr<art::TFileDirectory>                           laserBaselineDir_;
	std::unique_ptr<art::TFileDirectory>                           laserRmsDir_;
	std::unique_ptr<art::TFileDirectory>                           laserMaxDir_;

	BoardHists laserBoardH_{};

	std::array<TH1F*, kLaserChannels>   laserLiveWf_{};
	std::array<TH1F*, kLaserChannels>   laserOneHitWf_{};
	std::array<uint8_t, kLaserChannels> laserOneHitSeen_{};
	std::array<uint8_t, kLaserChannels> laserOneHitSent_{};
	std::array<uint8_t, kLaserChannels> laserLiveWaveformUpdated_{};
	std::array<uint8_t, kLaserChannels> laserChannelBooked_{};

	std::array<WaveformSizeStats, kLaserChannels> laserWfStats_{};

	std::array<TH1F*, kLaserChannels> laserBaselineDist_{};
	std::array<TH1F*, kLaserChannels> laserRmsDist_{};
	std::array<TH1F*, kLaserChannels> laserMaxDist_{};

	std::array<std::array<float, kWaveformNBins>, kLaserChannels> laserLastWf_{};
	std::array<uint8_t, kLaserChannels>                           laserLastWfValid_{};

	// -----------------------
	// Per-channel waveforms
	// -----------------------
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

	std::vector<uint8_t> channelBooked_;

	// Streaming bookkeeping
	//   active*           : objects that exist / were seen at least once
	//   updated*          : objects modified since the last successful send
	//   *QueuedForSend_   : de-duplication flags for updated* vectors
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

	// TFileService folders
	std::unique_ptr<art::TFileDirectory> disk0Dir_;
	std::unique_ptr<art::TFileDirectory> disk1Dir_;
	std::unique_ptr<art::TFileDirectory> globalDir_;

	// Disk maps per mode
	static constexpr size_t kNMapModes = 5;

	static constexpr size_t modeIndex(MapMode m) { return static_cast<size_t>(m); }

	static constexpr std::array<MapMode, kNMapModes> kAllModes{
	    MapMode::Amp, MapMode::Sum, MapMode::Asym, MapMode::Baseline, MapMode::RMS};

	std::array<bool, kNMapModes> enabledModes_{};
	std::array<bool, kNMapModes> streamEnabledModes_{};

	std::array<mu2e::THMu2eCaloDisk*, kNMapModes> disk0Maps_{};
	std::array<mu2e::THMu2eCaloDisk*, kNMapModes> disk1Maps_{};

	std::array<std::array<std::vector<double>, kNDisks>, kNMapModes>   diskSum_;
	std::array<std::array<std::vector<uint32_t>, kNDisks>, kNMapModes> diskCnt_;

	// Global summaries
	TH1F*     h_asymmetry{nullptr};
	TProfile* h_amp_sparse_{nullptr};
	TProfile* h_amp_dense_{nullptr};
	TH1F*     h_occupancy_sparse_{nullptr};
	TH1F*     h_occupancy_dense_{nullptr};
	TH1I*     h_pair_multiplicity_{nullptr};

	TProfile* h_baseline_sparse_{nullptr};
	TProfile* h_baseline_dense_{nullptr};

	TProfile* h_rms_sparse_{nullptr};
	TProfile* h_rms_dense_{nullptr};

	TProfile* h_maxval_sparse_{nullptr};
	TProfile* h_maxval_dense_{nullptr};

	TH1F* h_amp_dist_{nullptr};

	TH1F* h_global_board_dist_{nullptr};

	TH2I* h_global_board_vs_channel_{nullptr};
	TH2D* h_global_waveform_density_{nullptr};

	TH1F* h_waveform_size_{nullptr};

	// Electronics mapping from conditions
	mu2e::ProditionsHandle<mu2e::CaloDAQMap> _calodaqconds_h;

	// Booking helpers keep histogram creation lazy and bounded
	void ensureBoardBooked(int disk, int boardID);
	void ensureLiveWaveformBooked(
	    int disk, int boardID, int chanID, int rawId, int sipmId);
	void ensureFirstHitBooked(int         disk,
	                          int         boardID,
	                          int         chanID,
	                          int         rawId,
	                          int         sipmId,
	                          auto const& waveform,
	                          int         wfSize,
	                          float       baseline);
};

void CaloDigiDQM::ensureBaselineDistBooked(int disk, int boardID, int chanID)
{
	const int cidx = channelIndex(disk, boardID, chanID);
	if(cidx < 0 || cidx >= kTotalChannels)
		return;

	if(h_baseline_dist_[(size_t)cidx])
		return;

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return;

	if(!boardBaselineDir_[(size_t)bidx])
		return;

	art::TFileDirectory& dir = *boardBaselineDir_[(size_t)bidx];

	h_baseline_dist_[(size_t)cidx] = dir.make<TH1F>(
	    Form("D%d_B%03d_C%02d_BaselineDist", disk, boardID, chanID),
	    Form("Baseline Distribution D%d B%03d C%02d", disk, boardID, chanID),
	    2000,
	    1000,
	    3000);

	h_baseline_dist_[(size_t)cidx]->GetXaxis()->SetTitle("Baseline [ADC]");
	h_baseline_dist_[(size_t)cidx]->GetYaxis()->SetTitle("Count");
}
void CaloDigiDQM::ensureRmsDistBooked(int disk, int boardID, int chanID)
{
	const int cidx = channelIndex(disk, boardID, chanID);
	if(cidx < 0 || cidx >= kTotalChannels)
		return;
	if(h_rms_dist_[(size_t)cidx])
		return;

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return;
	if(!boardRmsDir_[(size_t)bidx])
		return;

	art::TFileDirectory& dir = *boardRmsDir_[(size_t)bidx];

	h_rms_dist_[(size_t)cidx] =
	    dir.make<TH1F>(Form("D%d_B%03d_C%02d_RMSDist", disk, boardID, chanID),
	                   Form("RMS Distribution D%d B%03d C%02d", disk, boardID, chanID),
	                   30,
	                   0,
	                   30);

	h_rms_dist_[(size_t)cidx]->GetXaxis()->SetTitle("RMS [ADC]");
	h_rms_dist_[(size_t)cidx]->GetYaxis()->SetTitle("Count");
}

void CaloDigiDQM::ensureMaxDistBooked(int disk, int boardID, int chanID)
{
	const int cidx = channelIndex(disk, boardID, chanID);
	if(cidx < 0 || cidx >= kTotalChannels)
		return;
	if(h_max_dist_[(size_t)cidx])
		return;

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return;
	if(!boardMaxDir_[(size_t)bidx])
		return;

	art::TFileDirectory& dir = *boardMaxDir_[(size_t)bidx];

	h_max_dist_[(size_t)cidx] = dir.make<TH1F>(
	    Form("D%d_B%03d_C%02d_MaxDist", disk, boardID, chanID),
	    Form("Max ADC Distribution D%d B%03d C%02d", disk, boardID, chanID),
	    300,
	    0,
	    4500);

	h_max_dist_[(size_t)cidx]->GetXaxis()->SetTitle("Peak ADC");
	h_max_dist_[(size_t)cidx]->GetYaxis()->SetTitle("Count");
}

void CaloDigiDQM::ensureAsymDistBooked(int disk, int boardID, int chanID)
{
	const int cidx = channelIndex(disk, boardID, chanID);
	if(cidx < 0 || cidx >= kTotalChannels)
		return;
	if(h_asym_dist_[(size_t)cidx])
		return;

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return;
	if(!boardAsymDir_[(size_t)bidx])
		return;

	art::TFileDirectory& dir = *boardAsymDir_[(size_t)bidx];

	h_asym_dist_[(size_t)cidx] = dir.make<TH1F>(
	    Form("D%d_B%03d_C%02d_AsymDist", disk, boardID, chanID),
	    Form("Asymmetry Distribution D%d B%03d C%02d", disk, boardID, chanID),
	    100,
	    -1.0,
	    1.0);

	h_asym_dist_[(size_t)cidx]->GetXaxis()->SetTitle("(L-R)/(L+R)");
	h_asym_dist_[(size_t)cidx]->GetYaxis()->SetTitle("Count");
}

static TString channelLabel(int boardID, int chanID, int rawId, int sipmId)
{
	// Compact per channel label for waveform hist titles
	return Form("B%03d C%02d (raw: %d, offline: %d)", boardID, chanID, rawId, sipmId);
}

void CaloDigiDQM::ensureLaserBoardBooked()
{
	if(!laserBoardHistDir_ || !laserBoardChanDir_)
	{
		art::TFileDirectory boardDir = laserDir_->mkdir("Board_160");

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
	if(chanID < 0 || chanID >= kLaserChannels)
		return;
	if(laserBaselineDist_[(size_t)chanID])
		return;
	if(!laserBaselineDir_)
		return;

	art::TFileDirectory& dir = *laserBaselineDir_;

	laserBaselineDist_[(size_t)chanID] =
	    dir.make<TH1F>(Form("B160_C%02d_BaselineDist", chanID),
	                   Form("Baseline Distribution B160 C%02d", chanID),
	                   2000,
	                   1000,
	                   3000);

	laserBaselineDist_[(size_t)chanID]->GetXaxis()->SetTitle("Baseline [ADC]");
	laserBaselineDist_[(size_t)chanID]->GetYaxis()->SetTitle("Count");
}

void CaloDigiDQM::ensureLaserRmsDistBooked(int chanID)
{
	if(chanID < 0 || chanID >= kLaserChannels)
		return;
	if(laserRmsDist_[(size_t)chanID])
		return;
	if(!laserRmsDir_)
		return;

	art::TFileDirectory& dir = *laserRmsDir_;

	laserRmsDist_[(size_t)chanID] =
	    dir.make<TH1F>(Form("B160_C%02d_RMSDist", chanID),
	                   Form("RMS Distribution B160 C%02d", chanID),
	                   30,
	                   0,
	                   30);

	laserRmsDist_[(size_t)chanID]->GetXaxis()->SetTitle("RMS [ADC]");
	laserRmsDist_[(size_t)chanID]->GetYaxis()->SetTitle("Count");
}

void CaloDigiDQM::ensureLaserMaxDistBooked(int chanID)
{
	if(chanID < 0 || chanID >= kLaserChannels)
		return;
	if(laserMaxDist_[(size_t)chanID])
		return;
	if(!laserMaxDir_)
		return;

	art::TFileDirectory& dir = *laserMaxDir_;

	laserMaxDist_[(size_t)chanID] =
	    dir.make<TH1F>(Form("B160_C%02d_MaxDist", chanID),
	                   Form("Max ADC Distribution B160 C%02d", chanID),
	                   300,
	                   0,
	                   4500);

	laserMaxDist_[(size_t)chanID]->GetXaxis()->SetTitle("Peak ADC");
	laserMaxDist_[(size_t)chanID]->GetYaxis()->SetTitle("Count");
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

	laserLiveWf_[(size_t)chanID] =
	    histosDir.make<TH1F>(cname, ctitle, kWaveformNBins, 0, kWaveformNBins);

	laserLiveWf_[(size_t)chanID]->GetYaxis()->SetTitle("ADC - Baseline");
	laserLiveWf_[(size_t)chanID]->GetXaxis()->SetTitle("Tick");
}

void CaloDigiDQM::ensureLaserFirstHitBooked(
    int chanID, int rawId, int sipmId, auto const& waveform, int wfSize, float baseline)
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

	TH1F* onehitHist =
	    chanDir.make<TH1F>(cname, ctitle, kWaveformNBins, 0, kWaveformNBins);

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

	if(h_global_waveform_density_)
	{
		const int nbx = h_global_waveform_density_->GetNbinsX();
		const int n2  = std::min<int>(wfSize, nbx);
		for(int i = 0; i < n2; ++i)
			h_global_waveform_density_->Fill(i, (double)waveform[(size_t)i]);
	}
}

void CaloDigiDQM::ensureBoardBooked(int disk, int boardID)
{
	// Lazily create per board directories and summary histograms
	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return;
	if(!boardHistDir_[(size_t)bidx] || !boardChanDir_[(size_t)bidx])
	{
		art::TFileDirectory boardDir = (disk == 0 ? *disk0Dir_ : *disk1Dir_)
		                                   .mkdir("Board_" + std::to_string(boardID));

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

		bh.occ = histosDir.make<TH1F>(Form("D%d_B%03d_Occupancy", disk, boardID),
		                              Form("Occupancy for D%d B%03d", disk, boardID),
		                              kChannelsPerBoard,
		                              0,
		                              kChannelsPerBoard);
		bh.occ->GetXaxis()->SetTitle("Channel ID");
		bh.occ->GetYaxis()->SetTitle("Count");

		bh.base = histosDir.make<TProfile>(Form("D%d_B%03d_Baseline", disk, boardID),
		                                   Form("Baseline for D%d B%03d", disk, boardID),
		                                   kChannelsPerBoard,
		                                   0,
		                                   kChannelsPerBoard);
		bh.rms  = histosDir.make<TProfile>(Form("D%d_B%03d_RMS", disk, boardID),
                                          Form("RMS for D%d B%03d", disk, boardID),
                                          kChannelsPerBoard,
                                          0,
                                          kChannelsPerBoard);
		bh.max  = histosDir.make<TProfile>(Form("D%d_B%03d_Max", disk, boardID),
                                          Form("Max for D%d B%03d", disk, boardID),
                                          kChannelsPerBoard,
                                          0,
                                          kChannelsPerBoard);

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
	// Book a per channel live waveform hist once, values are updated via cache flushing
	const int cidx = channelIndex(disk, boardID, chanID);
	if(cidx < 0 || cidx >= kTotalChannels)
		return;

	if(liveWf_[(size_t)cidx])
		return;

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return;
	if(!boardHistDir_[(size_t)bidx])
		return;

	art::TFileDirectory& histosDir = *boardHistDir_[(size_t)bidx];

	TString cname = Form("D%d_B%03d_C%02d_Waveform", disk, boardID, chanID);
	TString ctitle =
	    Form("Live Waveform for %s", channelLabel(boardID, chanID, rawId, sipmId).Data());

	liveWf_[(size_t)cidx] =
	    histosDir.make<TH1F>(cname, ctitle, kWaveformNBins, 0, kWaveformNBins);
	liveWf_[(size_t)cidx]->GetYaxis()->SetTitle("ADC - Baseline");
	liveWf_[(size_t)cidx]->GetXaxis()->SetTitle("Tick");
}

void CaloDigiDQM::ensureFirstHitBooked(int         disk,
                                       int         boardID,
                                       int         chanID,
                                       int         rawId,
                                       int         sipmId,
                                       auto const& waveform,
                                       int         wfSize,
                                       float       baseline)
{
	// Capture the first observed waveform per channel for offline inspection and density plots
	const int cidx = channelIndex(disk, boardID, chanID);
	if(cidx < 0 || cidx >= kTotalChannels)
		return;
	if(oneHitSeen_[(size_t)cidx])
		return;

	const int bidx = boardIndex(disk, boardID);
	if(bidx < 0 || bidx >= kTotalBoards)
		return;
	if(!boardChanDir_[(size_t)bidx])
		return;

	art::TFileDirectory& chanDir = *boardChanDir_[(size_t)bidx];

	TString cname  = Form("D%d_B%03d_C%02d_FirstHit", disk, boardID, chanID);
	TString ctitle = Form("First-Hit Waveform for %s",
	                      channelLabel(boardID, chanID, rawId, sipmId).Data());

	TH1F* onehitHist =
	    chanDir.make<TH1F>(cname, ctitle, kWaveformNBins, 0, kWaveformNBins);
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

	if(h_global_waveform_density_)
	{
		const int nbx = h_global_waveform_density_->GetNbinsX();
		const int n2  = std::min<int>(wfSize, nbx);
		for(int i = 0; i < n2; ++i)
			h_global_waveform_density_->Fill(i, (double)waveform[(size_t)i]);
	}
}

// ===========================
// Constructor
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
	// -----------------------
	// Reference file
	// -----------------------
	if(useReferenceFile_)
	{
		TFile f(referenceFile_.c_str(), "READ");
		if(f.IsZombie())
		{
			mf::LogWarning("CaloDigiDQM")
			    << "Reference file not available, continuing without references: "
			    << referenceFile_;
		}
		else
		{
			if(auto* h = dynamic_cast<TH1F*>(f.Get("ref_h_occ_dense")))
			{
				ref_h_occ_dense_ = dynamic_cast<TH1F*>(h->Clone("ref_h_occ_dense_mem"));
				ref_h_occ_dense_->SetDirectory(nullptr);
			}

			if(auto* h = dynamic_cast<TProfile*>(f.Get("ref_h_base_dense")))
			{
				ref_h_base_dense_ =
				    dynamic_cast<TProfile*>(h->Clone("ref_h_base_dense_mem"));
				ref_h_base_dense_->SetDirectory(nullptr);
			}

			if(auto* h = dynamic_cast<TProfile*>(f.Get("ref_h_rms_dense")))
			{
				ref_h_rms_dense_ =
				    dynamic_cast<TProfile*>(h->Clone("ref_h_rms_dense_mem"));
				ref_h_rms_dense_->SetDirectory(nullptr);
			}

			if(auto* h = dynamic_cast<TProfile*>(f.Get("ref_h_max_dense")))
			{
				ref_h_max_dense_ =
				    dynamic_cast<TProfile*>(h->Clone("ref_h_max_dense_mem"));
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
				ref_D0_B027_RMS_ =
				    dynamic_cast<TProfile*>(h->Clone("ref_D0_B027_RMS_mem"));
				ref_D0_B027_RMS_->SetDirectory(nullptr);
			}

			if(auto* h = dynamic_cast<TProfile*>(f.Get("ref_D0_B027_Max")))
			{
				ref_D0_B027_Max_ =
				    dynamic_cast<TProfile*>(h->Clone("ref_D0_B027_Max_mem"));
				ref_D0_B027_Max_->SetDirectory(nullptr);
			}

			if(auto* h = dynamic_cast<TH1F*>(f.Get("ref_D0_B027_C00_Waveform")))
			{
				ref_D0_B027_C00_Waveform_ =
				    dynamic_cast<TH1F*>(h->Clone("ref_D0_B027_C00_Waveform_mem"));
				ref_D0_B027_C00_Waveform_->SetDirectory(nullptr);
			}
		}
	}
	// -----------------------
	// ROOT directory layout
	// -----------------------
	art::ServiceHandle<art::TFileService> tfs;
	disk0Dir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk0"));
	disk1Dir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk1"));
	globalDir_ = std::make_unique<art::TFileDirectory>(tfs->mkdir("Global_Histograms"));
	laserDir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Laser"));
	// -----------------------
	// Stream selection controls otsdaq streaming only
	// -----------------------
	std::vector<std::string> rawStream = config().diskCombines();

	streamEnabledModes_.fill(false);
	enabledModes_.fill(false);

	for(auto& s : rawStream)
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

	// Save all modes to ROOT
	for(auto m : kAllModes)
		enabledModes_[modeIndex(m)] = true;
	// -----------------------
	// Allocate fixed size storage to avoid per event allocations
	// -----------------------
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
	channelBooked_.assign(kTotalChannels, 0u);

	activeRegularChannels_.reserve(kTotalChannels);
	activeLaserChannels_.reserve(kLaserChannels);
	updatedBoards_.reserve(kTotalBoards);
	updatedRegularChannels_.reserve(kTotalChannels);
	updatedLaserChannels_.reserve(kLaserChannels);

	boardQueuedForSend_.assign(kTotalBoards, 0u);
	regularQueuedForSend_.assign(kTotalChannels, 0u);
	laserQueuedForSend_.fill(0u);

	lastWf_.assign(kTotalChannels, std::array<float, kWaveformNBins>{});
	lastWfValid_.assign(kTotalChannels, 0u);

	feat_.assign((size_t)kMaxSipmIdForMaps_, SipmFeat{});
	featStamp_.assign((size_t)kMaxSipmIdForMaps_, 0);
	pairedStamp_.assign((size_t)(kMaxSipmIdForMaps_ / 2 + 2), 0);
	sipmMultiplicity_.assign((size_t)kMaxSipmIdForMaps_, 0u);

	for(auto m : kAllModes)
	{
		const size_t midx = modeIndex(m);
		for(int d = 0; d < kNDisks; ++d)
		{
			diskSum_[midx][d].assign((size_t)kMaxSipmIdForMaps_, 0.0);
			diskCnt_[midx][d].assign((size_t)kMaxSipmIdForMaps_, 0u);
		}
	}

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
	// Construct sender only when streaming is enabled
	if(sendHists_)
		histSender_ = std::make_unique<ots::HistoSender>(address_, port_);

	// -----------------------
	// Global diagnostic histograms
	// -----------------------
	h_skip_reason_ = globalDir_->make<TH1I>("h_skip_reason",
	                                        "CaloDigiDQM skip reasons",
	                                        (int)SkipReason::Count,
	                                        0,
	                                        (int)SkipReason::Count);
	for(int i = 0; i < (int)SkipReason::Count; ++i)
		h_skip_reason_->GetXaxis()->SetBinLabel(i + 1, skipLabel((SkipReason)i));

	h_global_board_vs_channel_ = globalDir_->make<TH2I>(
	    "h_board_vs_channel", "Board vs Channel Occupancy", 161, 0, 161, 20, 0, 20);
	h_global_board_vs_channel_->GetXaxis()->SetTitle("Board ID");
	h_global_board_vs_channel_->GetYaxis()->SetTitle("Channel ID");

	h_global_waveform_density_ =
	    globalDir_->make<TH2D>("h_waveform_density",
	                           "Waveform Density (first-hit per channel)",
	                           150,
	                           0,
	                           150,
	                           400,
	                           2000,
	                           4095);
	h_global_waveform_density_->GetXaxis()->SetTitle("Tick");
	h_global_waveform_density_->GetYaxis()->SetTitle("ADC Value");

	h_amp_dist_ =
	    globalDir_->make<TH1F>("h_amp_dist", "Amplitude Distribution", 400, -200, 2000);
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

	h_occupancy_sparse_ = globalDir_->make<TH1F>("h_occ_sparse",
	                                             "Occupancy (Sparse Encoding, All Disks)",
	                                             axisSparse.nBins,
	                                             axisSparse.xMin,
	                                             axisSparse.xMax);
	h_occupancy_sparse_->GetXaxis()->SetTitle("Encoded Channel (boardID*100 + chanID)");
	h_occupancy_sparse_->GetYaxis()->SetTitle("Hit Count");

	h_occupancy_dense_ = globalDir_->make<TH1F>("h_occ_dense",
	                                            "Occupancy (Dense Encoding, All Disks)",
	                                            axisDense.nBins,
	                                            axisDense.xMin,
	                                            axisDense.xMax);
	h_occupancy_dense_->GetXaxis()->SetTitle("Encoded Channel (boardID*20 + chanID)");
	h_occupancy_dense_->GetYaxis()->SetTitle("Hit Count");

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

	h_baseline_sparse_ = makeGlobalProfile("h_base_sparse",
	                                       "Baseline (Sparse Encoding, All Disks)",
	                                       axisSparse,
	                                       "Encoded Channel (boardID*100 + chanID)",
	                                       "Mean Baseline [ADC]");

	h_baseline_dense_ = makeGlobalProfile("h_base_dense",
	                                      "Baseline (Dense Encoding, All Disks)",
	                                      axisDense,
	                                      "Encoded Channel (boardID*20 + chanID)",
	                                      "Mean Baseline [ADC]");

	h_rms_sparse_ = makeGlobalProfile("h_rms_sparse",
	                                  "RMS (Sparse Encoding, All Disks)",
	                                  axisSparse,
	                                  "Encoded Channel (boardID*100 + chanID)",
	                                  "Mean RMS [ADC]");

	h_rms_dense_ = makeGlobalProfile("h_rms_dense",
	                                 "RMS (Dense Encoding, All Disks)",
	                                 axisDense,
	                                 "Encoded Channel (boardID*20 + chanID)",
	                                 "Mean RMS [ADC]");

	h_amp_sparse_ = makeGlobalProfile("h_amp_sparse",
	                                  "Amplitude (Sparse Encoding, All Disks)",
	                                  axisSparse,
	                                  "Encoded Channel (boardID*100 + chanID)",
	                                  "Mean Amplitude [ADC]");

	h_amp_dense_ = makeGlobalProfile("h_amp_dense",
	                                 "Amplitude (Dense Encoding, All Disks)",
	                                 axisDense,
	                                 "Encoded Channel (boardID*20 + chanID)",
	                                 "Mean Amplitude [ADC]");

	h_maxval_sparse_ = makeGlobalProfile("h_max_sparse",
	                                     "Max ADC (Sparse Encoding, All Disks)",
	                                     axisSparse,
	                                     "Encoded Channel (boardID*100 + chanID)",
	                                     "Mean Peak ADC [ADC]");

	h_maxval_dense_ = makeGlobalProfile("h_max_dense",
	                                    "Max ADC (Dense Encoding, All Disks)",
	                                    axisDense,
	                                    "Encoded Channel (boardID*20 + chanID)",
	                                    "Mean Peak ADC [ADC]");

	h_asymmetry =
	    globalDir_->make<TH1F>("h_asym", "Left-Right Asymmetry", 100, -1.0, 1.0);
	h_asymmetry->GetXaxis()->SetTitle("(L - R)/(L + R)");
	h_asymmetry->GetYaxis()->SetTitle("Frequency");

	h_global_board_dist_ =
	    globalDir_->make<TH1F>("h_board_dist", "Global Board Distribution", 161, 0, 161);
	h_global_board_dist_->GetXaxis()->SetTitle("Board ID");
	h_global_board_dist_->GetYaxis()->SetTitle("Frequency");
}

// ===========================
// analyze()
// ===========================
void CaloDigiDQM::analyze(art::Event const& event)
{
	// Fetch inputs and conditions once per event
	const auto& caloDigis    = *event.getValidHandle<CaloDigiCollection>(caloDigiTag_);
	const auto& calodaqconds = _calodaqconds_h.get(event.id());

	// Stamp rollover protection, preserves correctness without clearing vectors each event
	if(++pairStamp_ == std::numeric_limits<int>::max())
	{
		std::fill(featStamp_.begin(), featStamp_.end(), 0);
		std::fill(pairedStamp_.begin(), pairedStamp_.end(), 0);
		pairStamp_ = 1;
	}
	std::fill(sipmMultiplicity_.begin(), sipmMultiplicity_.end(), 0u);

	auto featSeen = [&](int sid) -> bool {
		return (sid >= 0 && sid < kMaxSipmIdForMaps_ &&
		        featStamp_[(size_t)sid] == pairStamp_);
	};

	auto markFeat = [&](int sid, SipmFeat const& f) {
		if(sid < 0 || sid >= kMaxSipmIdForMaps_)
			return;
		feat_[(size_t)sid]      = f;
		featStamp_[(size_t)sid] = pairStamp_;
	};

	auto paired = [&](int crystalId) -> bool {
		if(crystalId < 0 || (size_t)crystalId >= pairedStamp_.size())
			return true;
		return pairedStamp_[(size_t)crystalId] == pairStamp_;
	};

	auto markPaired = [&](int crystalId) {
		if(crystalId < 0 || (size_t)crystalId >= pairedStamp_.size())
			return;
		pairedStamp_[(size_t)crystalId] = pairStamp_;
	};

	auto betterRep = [&](SipmFeat const& cand, SipmFeat const& cur) -> bool {
		if(cand.amp != cur.amp)
			return cand.amp > cur.amp;  // prefer larger baseline-subtracted amplitude
		if(cand.ampRaw != cur.ampRaw)
			return cand.ampRaw > cur.ampRaw;
		return cand.cidx < cur.cidx;  // stable tie-break
	};

	auto packRepWaveform =
	    [&](SipmFeat& dst, auto const& waveform, float baseline, int wfSize) {
		    const int n = std::min<int>(wfSize, kWaveformNBins);
		    for(int i = 0; i < n; ++i)
			    dst.wfSub[(size_t)i] = (float)waveform[(size_t)i] - baseline;

		    for(int i = n; i < kWaveformNBins; ++i)
			    dst.wfSub[(size_t)i] = 0.0f;
	    };

	std::vector<int> repSipmIds;
	repSipmIds.reserve(caloDigis.size());
	std::array<SipmFeat, kLaserChannels> laserRep{};
	std::array<uint8_t, kLaserChannels>  laserRepSeen{};
	std::vector<int>                     repLaserChannels;
	repLaserChannels.reserve(kLaserChannels);

	// Guard against noisy asymmetry when the denominator is too small
	static constexpr double kMinDenomForAsym = 5.0;

	for(const auto& digi : caloDigis)
	{
		const auto& waveform = digi.waveform();
		const int   wfSize   = (int)waveform.size();

		if(h_waveform_size_)
			h_waveform_size_->Fill(wfSize);

		const int sipmId = digi.SiPMID();
		if(sipmId < 0)
		{
			recordSkip(SkipReason::BadSipmId);
			continue;
		}

		// Translate offline SiPMID to electronics rawId using CaloDAQMap
		const int rawId = calodaqconds.rawId(mu2e::CaloSiPMId(sipmId)).id();
		if(rawId == 9999)
		{
			recordSkip(SkipReason::UnmappedRawId);
			continue;
		}
		if(rawId < 0)
		{
			recordSkip(SkipReason::RawIdNegative);
			continue;
		}

		// Decode rawId into board and channel
		const int boardID = rawId / kChannelsPerBoard;
		const int chanID  = rawId % kChannelsPerBoard;

		// -----------------------
		// Laser board branch
		// -----------------------
		if(isLaserBoard(boardID))
		{
			++nFillLaser_;
			ensureLaserBoardBooked();
			laserBoardUpdated_ = true;

			if(!laserChannelBooked_[(size_t)chanID])
			{
				ensureLaserBaselineDistBooked(chanID);
				ensureLaserRmsDistBooked(chanID);
				ensureLaserMaxDistBooked(chanID);
				ensureLaserLiveWaveformBooked(chanID, rawId, sipmId);
				laserChannelBooked_[(size_t)chanID] = 1u;
				activeLaserChannels_.push_back(chanID);
			}

			if(h_global_board_dist_)
				h_global_board_dist_->Fill(boardID);

			if(h_global_board_vs_channel_)
				h_global_board_vs_channel_->Fill(boardID, chanID);

			if(laserBoardH_.occ)
				laserBoardH_.occ->Fill(chanID);

			// waveform-size monitoring
			updateWaveformStats(laserWfStats_[(size_t)chanID], wfSize);

			const int peakpos = digi.peakpos();
			if(wfSize == 0 || peakpos < 0 || peakpos >= wfSize)
			{
				recordSkip(SkipReason::PeakPosOutOfRange);
				continue;
			}

			float baseline = 0.0f;
			float rms      = 0.0f;
			if(!computeBaselineRms(waveform, wfSize, baseline, rms))
			{
				recordSkip(SkipReason::NonFiniteBaselineOrRms);
				continue;
			}

			if(laserBaselineDist_[(size_t)chanID])
				laserBaselineDist_[(size_t)chanID]->Fill(baseline);

			if(laserRmsDist_[(size_t)chanID])
				laserRmsDist_[(size_t)chanID]->Fill(rms);

			ensureLaserFirstHitBooked(chanID, rawId, sipmId, waveform, wfSize, baseline);

			const float  ampRaw = waveform[(size_t)peakpos];
			const double amp    = (double)ampRaw - (double)baseline;

			SipmFeat cand{};
			cand.amp      = amp;
			cand.baseline = baseline;
			cand.rms      = rms;
			cand.ampRaw   = (double)ampRaw;
			cand.disk     = -1;
			cand.board    = kLaserBoardID;
			cand.chan     = chanID;
			cand.cidx     = -1;

			packRepWaveform(cand, waveform, baseline, wfSize);

			if(!laserRepSeen[(size_t)chanID])
			{
				laserRep[(size_t)chanID]     = cand;
				laserRepSeen[(size_t)chanID] = 1u;
				repLaserChannels.push_back(chanID);
			}
			else if(betterRep(cand, laserRep[(size_t)chanID]))
			{
				laserRep[(size_t)chanID] = cand;
			}

			if(laserMaxDist_[(size_t)chanID])
				laserMaxDist_[(size_t)chanID]->Fill(ampRaw);

			if(h_amp_dist_)
				h_amp_dist_->Fill(amp);

			// laser board should not go into disk maps or L/R pairing
			continue;
		}

		const int disk = boardID / kBoardsPerDisk;
		if(disk < 0 || disk >= kNDisks)
		{
			recordSkip(SkipReason::DiskOutOfRange);
			++nFillMiss_;
			continue;
		}

		// Encoded is per disk, cidx is global across disks
		const int encodedSparse = encodeSparse(boardID, chanID);
		const int encodedDense  = encodeDense(boardID, chanID);
		const int cidx          = channelIndex(disk, boardID, chanID);

		ensureBoardBooked(disk, boardID);

		if(!channelBooked_[(size_t)cidx])
		{
			ensureBaselineDistBooked(disk, boardID, chanID);
			ensureRmsDistBooked(disk, boardID, chanID);
			ensureMaxDistBooked(disk, boardID, chanID);
			ensureLiveWaveformBooked(disk, boardID, chanID, rawId, sipmId);
			channelBooked_[(size_t)cidx] = 1u;
			activeRegularChannels_.push_back(cidx);
		}

		if(disk == 0)
			++nFillDisk0_;
		else
			++nFillDisk1_;

		// Lightweight occupancy distributions for sanity checks
		if(h_global_board_dist_)
			h_global_board_dist_->Fill(boardID);

		if(h_global_board_vs_channel_)
			h_global_board_vs_channel_->Fill(boardID, chanID);
		if(h_occupancy_sparse_)
			h_occupancy_sparse_->Fill(encodedSparse);
		if(h_occupancy_dense_)
			h_occupancy_dense_->Fill(encodedDense);

		// Ensure per board containers exist before filling per board objects

		// Compute bidx once and reuse for all board-level fills
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

		// Collect waveform length behavior per channel for integrity monitoring
		updateWaveformStats(wfStats_[(size_t)cidx], wfSize);

		// Peak position integrity checks
		const int peakpos = digi.peakpos();

		if(wfSize == 0 || peakpos < 0 || peakpos >= wfSize)
		{
			recordSkip(SkipReason::PeakPosOutOfRange);
			continue;
		}

		// Baseline and RMS from the first samples, robust against small wfSize

		float baseline = 0.0f;
		float rms      = 0.0f;
		if(!computeBaselineRms(waveform, wfSize, baseline, rms))
		{
			recordSkip(SkipReason::NonFiniteBaselineOrRms);
			continue;
		}

		if(sipmId >= 0 && sipmId < kMaxSipmIdForMaps_)
			++sipmMultiplicity_[(size_t)sipmId];

		if(h_baseline_dist_[(size_t)cidx])
			h_baseline_dist_[(size_t)cidx]->Fill(baseline);

		if(h_rms_dist_[(size_t)cidx])
			h_rms_dist_[(size_t)cidx]->Fill(rms);

		// Book and fill waveform products for offline and streaming use
		ensureFirstHitBooked(
		    disk, boardID, chanID, rawId, sipmId, waveform, wfSize, baseline);

		// Peak amplitude is baseline subtracted, ampRaw is the raw peak sample
		const float  ampRaw = waveform[(size_t)peakpos];
		const double amp    = (double)ampRaw - (double)baseline;

		if(h_max_dist_[(size_t)cidx])
			h_max_dist_[(size_t)cidx]->Fill(ampRaw);
		if(h_amp_dist_)
			h_amp_dist_->Fill(amp);

		SipmFeat cand{};
		cand.amp      = amp;
		cand.baseline = baseline;
		cand.rms      = rms;
		cand.ampRaw   = (double)ampRaw;
		cand.disk     = disk;
		cand.board    = boardID;
		cand.chan     = chanID;
		cand.cidx     = cidx;
		packRepWaveform(cand, waveform, baseline, wfSize);

		if(!featSeen(sipmId))
		{
			markFeat(sipmId, cand);
			repSipmIds.push_back(sipmId);
		}
		else if(betterRep(cand, feat_[(size_t)sipmId]))
		{
			markFeat(sipmId, cand);
		}
	}

	// Multiple usable digis per SiPM can occur in one event.
	// Pair-based quantities use one representative digi per SiPM,
	// selected earlier by highest baseline-subtracted amplitude.
	for(int sipmId : repSipmIds)
	{
		const auto& f = feat_[(size_t)sipmId];

		if(f.cidx >= 0 && f.cidx < kTotalChannels)
		{
			lastWf_[(size_t)f.cidx]      = f.wfSub;
			lastWfValid_[(size_t)f.cidx] = 1u;

			liveWaveformUpdated_[(size_t)f.cidx] = 1u;
			if(!regularQueuedForSend_[(size_t)f.cidx])
			{
				updatedRegularChannels_.push_back(f.cidx);
				regularQueuedForSend_[(size_t)f.cidx] = 1u;
			}
		}

		const int encodedSparse = encodeSparse(f.board, f.chan);
		const int encodedDense  = encodeDense(f.board, f.chan);
		const int bidx          = boardIndex(f.disk, f.board);

		if(h_amp_sparse_)
			h_amp_sparse_->Fill(encodedSparse, f.amp);
		if(h_amp_dense_)
			h_amp_dense_->Fill(encodedDense, f.amp);

		if(h_baseline_sparse_)
			h_baseline_sparse_->Fill(encodedSparse, f.baseline);
		if(h_baseline_dense_)
			h_baseline_dense_->Fill(encodedDense, f.baseline);

		if(h_rms_sparse_)
			h_rms_sparse_->Fill(encodedSparse, f.rms);
		if(h_rms_dense_)
			h_rms_dense_->Fill(encodedDense, f.rms);

		if(h_maxval_sparse_)
			h_maxval_sparse_->Fill(encodedSparse, f.ampRaw);
		if(h_maxval_dense_)
			h_maxval_dense_->Fill(encodedDense, f.ampRaw);

		if(bidx >= 0 && bidx < kTotalBoards)
		{
			auto& bh = boardH_[(size_t)bidx];
			if(bh.base)
				bh.base->Fill(f.chan, f.baseline);
			if(bh.rms)
				bh.rms->Fill(f.chan, f.rms);
			if(bh.max)
				bh.max->Fill(f.chan, f.ampRaw);
		}

		if(modeEnabled(MapMode::Amp))
			accDisk(MapMode::Amp, f.disk, sipmId, f.amp);
		if(modeEnabled(MapMode::Baseline))
			accDisk(MapMode::Baseline, f.disk, sipmId, f.baseline);
		if(modeEnabled(MapMode::RMS))
			accDisk(MapMode::RMS, f.disk, sipmId, f.rms);
	}

	for(int sipmId : repSipmIds)
	{
		const int crystalId = sipmId / 2;
		if(paired(crystalId))
			continue;

		const int evenId = 2 * crystalId;
		const int oddId  = evenId + 1;

		if(!(featSeen(evenId) && featSeen(oddId)))
			continue;

		const int multL = (evenId >= 0 && evenId < kMaxSipmIdForMaps_)
		                      ? (int)sipmMultiplicity_[(size_t)evenId]
		                      : 0;
		const int multR = (oddId >= 0 && oddId < kMaxSipmIdForMaps_)
		                      ? (int)sipmMultiplicity_[(size_t)oddId]
		                      : 0;

		if(h_pair_multiplicity_)
			h_pair_multiplicity_->Fill(std::max(multL, multR));

		markPaired(crystalId);

		const auto& fL = feat_[(size_t)evenId];
		const auto& fR = feat_[(size_t)oddId];

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

		ensureAsymDistBooked(fL.disk, fL.board, fL.chan);
		ensureAsymDistBooked(fR.disk, fR.board, fR.chan);

		const int cidxL = fL.cidx;
		const int cidxR = fR.cidx;

		if(cidxL >= 0 && cidxL < kTotalChannels && h_asym_dist_[(size_t)cidxL])
			h_asym_dist_[(size_t)cidxL]->Fill(asym);

		if(cidxR >= 0 && cidxR < kTotalChannels && h_asym_dist_[(size_t)cidxR])
			h_asym_dist_[(size_t)cidxR]->Fill(asym);

		if(h_asymmetry)
			h_asymmetry->Fill(asym);

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

		if(fL.disk != fR.disk)
		{
			mf::LogWarning("CaloDigiDQM")
			    << "Disk mismatch for paired crystal " << crystalId << " (SiPM " << evenId
			    << " in disk " << fL.disk << ", SiPM " << oddId << " in disk " << fR.disk
			    << ").";
		}
	}

	for(int chan : repLaserChannels)
	{
		const auto& f = laserRep[(size_t)chan];

		if(laserBoardH_.base)
			laserBoardH_.base->Fill(chan, f.baseline);
		if(laserBoardH_.rms)
			laserBoardH_.rms->Fill(chan, f.rms);
		if(laserBoardH_.max)
			laserBoardH_.max->Fill(chan, f.ampRaw);

		laserLastWf_[(size_t)chan]      = f.wfSub;
		laserLastWfValid_[(size_t)chan] = 1u;

		laserLiveWaveformUpdated_[(size_t)chan] = 1u;
		if(!laserQueuedForSend_[(size_t)chan])
		{
			updatedLaserChannels_.push_back(chan);
			laserQueuedForSend_[(size_t)chan] = 1u;
		}
	}

	++eventCounter_;

	// Decide which groups are scheduled to be sent this event
	const bool doSummariesEvent = (freqDQM_ > 0) && (eventCounter_ % freqDQM_ == 0);
	bool doWaveforms = (freqWaveforms_ > 0) && (eventCounter_ % freqWaveforms_ == 0);

	const int  diskMapPeriod = (freqDQM_ > 0) ? (freqDQM_ + kDiskMapsExtraPeriod) : 0;
	const bool doDiskMaps =
	    enableDiskMaps_ && (diskMapPeriod > 0) && (eventCounter_ % diskMapPeriod == 0);

	// Extra optimization:
	// if waveform streaming is scheduled, but there are no live updates
	// and no unsent first-hit waveforms, skip waveform sending entirely.
	if(doWaveforms)
	{
		const bool haveWaveformUpdates =
		    !updatedRegularChannels_.empty() || !updatedLaserChannels_.empty();

		const bool havePendingFirstHits =
		    (pendingRegularFirstHits_ > 0) || (pendingLaserFirstHits_ > 0);

		if(!haveWaveformUpdates && !havePendingFirstHits)
			doWaveforms = false;
	}

	if(!doSummariesEvent && !doWaveforms && !doDiskMaps)
		return;

	// No sender means no streaming, keep ROOT output path independent
	if(!sendHists_ || !histSender_)
		return;

	// Materialize derived content only when it will be streamed
	if(doDiskMaps)
		refreshDiskMaps();

	if(doWaveforms)
	{
		flushUpdatedLiveWaveforms();
		flushUpdatedLaserLiveWaveforms();
	}

	// Group histograms by otsdaq folder, use :replace to refresh plots in place
	std::map<std::string, std::vector<TH1*>> hists_to_send;
	std::vector<int>                         regularOneHitSentThisCall;
	std::vector<int>                         laserOneHitSentThisCall;
	std::vector<int>                         regularLiveSentThisCall;
	std::vector<int>                         laserLiveSentThisCall;

	if(doSummariesEvent)
	{
		auto& g = hists_to_send[moduleTag_ + "/Global:replace"];

		if(laserBoardUpdated_)
		{
			auto& lg = hists_to_send[moduleTag_ + "/Laser/Board160:replace"];
			if(laserBoardH_.occ)
				lg.push_back(laserBoardH_.occ);
			if(laserBoardH_.base)
				lg.push_back(laserBoardH_.base);
			if(laserBoardH_.rms)
				lg.push_back(laserBoardH_.rms);
			if(laserBoardH_.max)
				lg.push_back(laserBoardH_.max);
		}

		if(h_occupancy_sparse_)
			g.push_back(h_occupancy_sparse_);
		if(ref_h_occ_dense_)
			g.push_back(ref_h_occ_dense_);
		if(h_occupancy_dense_)
			g.push_back(h_occupancy_dense_);

		if(h_baseline_sparse_)
			g.push_back(h_baseline_sparse_);
		if(ref_h_base_dense_)
			g.push_back(ref_h_base_dense_);
		if(h_baseline_dense_)
			g.push_back(h_baseline_dense_);

		if(h_rms_sparse_)
			g.push_back(h_rms_sparse_);
		if(ref_h_rms_dense_)
			g.push_back(ref_h_rms_dense_);
		if(h_rms_dense_)
			g.push_back(h_rms_dense_);

		if(h_maxval_sparse_)
			g.push_back(h_maxval_sparse_);
		if(ref_h_max_dense_)
			g.push_back(ref_h_max_dense_);
		if(h_maxval_dense_)
			g.push_back(h_maxval_dense_);

		if(ref_h_asym_)
			g.push_back(ref_h_asym_);
		if(h_asymmetry)
			g.push_back(h_asymmetry);

		if(h_global_board_dist_)
			g.push_back(h_global_board_dist_);
		if(h_global_board_vs_channel_)
			g.push_back(h_global_board_vs_channel_);
		if(h_global_waveform_density_)
			g.push_back(h_global_waveform_density_);
		if(h_waveform_size_)
			g.push_back(h_waveform_size_);
		if(h_pair_multiplicity_)
			g.push_back(h_pair_multiplicity_);
		if(h_skip_reason_)
			g.push_back(h_skip_reason_);

		// Stream per board summaries only for boards that were booked
		for(int bidx : updatedBoards_)
		{
			const int disk    = bidx / kBoardsPerDisk;
			const int blocal  = bidx % kBoardsPerDisk;
			const int boardID = boardIdFromDiskAndLocal(disk, blocal);

			std::string groupPath =
			    Form("%s/Disk%d/Board%03d:replace", moduleTag_.c_str(), disk, boardID);

			if(disk == 0 && boardID == 27)
			{
				if(ref_D0_B027_Occupancy_)
					hists_to_send[groupPath].push_back(ref_D0_B027_Occupancy_);
				if(ref_D0_B027_Baseline_)
					hists_to_send[groupPath].push_back(ref_D0_B027_Baseline_);
				if(ref_D0_B027_RMS_)
					hists_to_send[groupPath].push_back(ref_D0_B027_RMS_);
				if(ref_D0_B027_Max_)
					hists_to_send[groupPath].push_back(ref_D0_B027_Max_);
			}

			const auto& bh = boardH_[(size_t)bidx];
			if(!bh.occ)
				continue;

			if(bh.occ)
				hists_to_send[groupPath].push_back(bh.occ);
			if(bh.base)
				hists_to_send[groupPath].push_back(bh.base);
			if(bh.rms)
				hists_to_send[groupPath].push_back(bh.rms);
			if(bh.max)
				hists_to_send[groupPath].push_back(bh.max);
		}
	}

	if(doDiskMaps)
	{
		for(auto m : streamModes_)
		{
			if(!streamModeEnabled(m))
				continue;

			const size_t midx = modeIndex(m);
			std::string  groupPath =
			    moduleTag_ + "/DiskMaps/" + modeSuffix(m) + ":replace";

			if(disk0Maps_[midx])
				hists_to_send[groupPath].push_back(disk0Maps_[midx]);
			if(disk1Maps_[midx])
				hists_to_send[groupPath].push_back(disk1Maps_[midx]);
		}
	}

	if(doWaveforms)
	{
		for(int chan : updatedLaserChannels_)
		{
			std::string livePath = Form("%s/Laser/Waveforms/Board160/Channel%02d:replace",
			                            moduleTag_.c_str(),
			                            chan);

			if(laserLiveWf_[(size_t)chan] && laserLiveWaveformUpdated_[(size_t)chan])
			{
				hists_to_send[livePath].push_back(laserLiveWf_[(size_t)chan]);
				laserLiveSentThisCall.push_back(chan);
			}
		}
		for(int chan : activeLaserChannels_)
		{
			std::string oneHitPath =
			    Form("%s/Laser/OneHitWaveforms/Board160/Channel%02d:replace",
			         moduleTag_.c_str(),
			         chan);

			if(laserOneHitWf_[(size_t)chan] && !laserOneHitSent_[(size_t)chan])
			{
				hists_to_send[oneHitPath].push_back(laserOneHitWf_[(size_t)chan]);
				laserOneHitSentThisCall.push_back(chan);
			}
		}
		// live waveforms grouped by disk/board: only updated channels
		for(int cidx : updatedRegularChannels_)
		{
			const int disk    = diskFromCidx(cidx);
			const int enc     = encodedFromCidx(cidx);
			const int blocal  = boardLocalFromEncoded(enc);
			const int chan    = chanFromEncoded(enc);
			const int boardID = boardIdFromDiskAndLocal(disk, blocal);

			std::string livePath =
			    Form("%s/Waveforms/Disk%d/Board%03d/Channel%02d:replace",
			         moduleTag_.c_str(),
			         disk,
			         boardID,
			         chan);

			if(disk == 0 && boardID == 27 && chan == 0)
			{
				if(ref_D0_B027_C00_Waveform_)
					hists_to_send[livePath].push_back(ref_D0_B027_C00_Waveform_);
			}

			if(liveWf_[(size_t)cidx] && liveWaveformUpdated_[(size_t)cidx])
			{
				hists_to_send[livePath].push_back(liveWf_[(size_t)cidx]);
				regularLiveSentThisCall.push_back(cidx);
			}
		}

		// first-hit waveforms grouped by disk/board: any active channel with unsent first hit
		for(int cidx : activeRegularChannels_)
		{
			const int disk    = diskFromCidx(cidx);
			const int enc     = encodedFromCidx(cidx);
			const int blocal  = boardLocalFromEncoded(enc);
			const int chan    = chanFromEncoded(enc);
			const int boardID = boardIdFromDiskAndLocal(disk, blocal);

			std::string oneHitPath =
			    Form("%s/OneHitWaveforms/Disk%d/Board%03d/Channel%02d:replace",
			         moduleTag_.c_str(),
			         disk,
			         boardID,
			         chan);

			if(oneHitWf_[(size_t)cidx] && !oneHitSent_[(size_t)cidx])
			{
				hists_to_send[oneHitPath].push_back(oneHitWf_[(size_t)cidx]);
				regularOneHitSentThisCall.push_back(cidx);
			}
		}
	}

	// Streaming failures are rate limited by disabling sendHists_ after repeated errors
	if(hists_to_send.empty())
		return;
	try
	{
		histSender_->sendHistograms(hists_to_send);
		histSendErrorCount_ = 0;
		for(int cidx : regularOneHitSentThisCall)
		{
			if(!oneHitSent_[(size_t)cidx])
			{
				oneHitSent_[(size_t)cidx] = 1u;
				if(pendingRegularFirstHits_ > 0)
					--pendingRegularFirstHits_;
			}
		}

		for(int chan : laserOneHitSentThisCall)
		{
			if(!laserOneHitSent_[(size_t)chan])
			{
				laserOneHitSent_[(size_t)chan] = 1u;
				if(pendingLaserFirstHits_ > 0)
					--pendingLaserFirstHits_;
			}
		}

		for(int cidx : regularLiveSentThisCall)
			liveWaveformUpdated_[(size_t)cidx] = 0u;

		for(int chan : laserLiveSentThisCall)
			laserLiveWaveformUpdated_[(size_t)chan] = 0u;

		// clear board-summary queues only if summaries were actually sent
		if(doSummariesEvent)
		{
			for(int bidx : updatedBoards_)
				boardQueuedForSend_[(size_t)bidx] = 0u;
			updatedBoards_.clear();
			laserBoardUpdated_ = false;
		}

		// clear waveform queues only if waveforms were actually sent
		if(doWaveforms)
		{
			for(int cidx : updatedRegularChannels_)
				regularQueuedForSend_[(size_t)cidx] = 0u;
			updatedRegularChannels_.clear();

			for(int chan : updatedLaserChannels_)
				laserQueuedForSend_[(size_t)chan] = 0u;
			updatedLaserChannels_.clear();
		}
	}
	catch(const std::exception& e)
	{
		++histSendErrorCount_;
		mf::LogError("CaloDigiDQM") << "HistoSender::sendHistograms exception ("
		                            << histSendErrorCount_ << "): " << e.what();

		if(histSendErrorCount_ >= kMaxSendErrors_)
		{
			sendHists_ = false;
			histSender_.reset();
			mf::LogWarning("CaloDigiDQM")
			    << "Histogram streaming disabled after " << histSendErrorCount_
			    << " consecutive send errors.";
		}
	}
	catch(...)
	{
		++histSendErrorCount_;
		mf::LogError("CaloDigiDQM") << "HistoSender::sendHistograms non-std exception ("
		                            << histSendErrorCount_ << ").";

		if(histSendErrorCount_ >= kMaxSendErrors_)
		{
			sendHists_ = false;
			histSender_.reset();
			mf::LogWarning("CaloDigiDQM")
			    << "Histogram streaming disabled after " << histSendErrorCount_
			    << " consecutive send errors.";
		}
	}
}

// ===========================
// endJob()
// ===========================
void CaloDigiDQM::endJob()
{
	// Final flush ensures ROOT output captures last cached values
	flushAllLiveWaveforms();
	flushAllLaserLiveWaveforms();
	refreshDiskMaps();

	// Summarize skip reasons for quick run validation
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
	                           << " sendErr=" << histSendErrorCount_
	                           << " badSipmId=" << nBadSipmId_ << skips.str();

	// Find channels where waveform size is not stable across the run
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

	// Order by instability so the most suspicious channels appear first
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
		// Print a compact top list to support debugging and data quality follow-up
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

			// Log one summary row per unstable channel:
			//   first    = first waveform size seen for this channel
			//   min/max  = minimum and maximum waveform sizes seen across the run
			//   seen     = number of digis observed for this channel
			//   trans    = number of times waveform size changed relative to previous digi
			//   mismatch = number of times waveform size differed from the first observed size
			//   pad      = number of waveforms shorter than kWaveformNBins
			//   trunc    = number of waveforms longer than kWaveformNBins
			os << "  (D" << disk << " B" << boardID << " C" << chan << ")"
			   << " first=" << r.st.first << " min=" << r.st.min << " max=" << r.st.max
			   << " seen=" << r.st.nSeen << " trans=" << r.st.nTransitions
			   << " mismatch=" << r.st.nMismatchToFirst << " pad=" << r.st.nPadded
			   << " trunc=" << r.st.nTruncated << "\n";
		}
		mf::LogInfo("CaloDigiDQM") << os.str();
	}
	// -----------------------
	// Laser waveform-size summary
	// -----------------------
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
