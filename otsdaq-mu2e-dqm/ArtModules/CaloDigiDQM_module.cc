// CaloDigiDQM_module.cc
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
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mu2e
{
class CaloDigiDQM : public art::EDAnalyzer
{
  public:
	struct Config
	{
		// otsdaq streaming endpoint and namespace
		fhicl::Atom<std::string> address{fhicl::Name("address"),
		                                 "mu2edaq11-data.fnal.gov"};
		fhicl::Atom<int>         port{fhicl::Name("port"), 6000};
		fhicl::Atom<std::string> moduleTag{fhicl::Name("moduleTag"), "CaloDQM"};
		fhicl::Atom<bool>        sendHists{fhicl::Name("sendHists"), false};

		// Streaming cadence for summaries and waveforms
		fhicl::Atom<int> freqDQM{fhicl::Name("freqDQM"), 100};
		fhicl::Atom<int> freqWaveforms{fhicl::Name("freqWaveforms"),
		                               0};  // 0 = disable waveform streaming

		// Input label for CaloDigiCollection
		fhicl::Atom<std::string> caloDigiModuleLabel{fhicl::Name("caloDigiModuleLabel"),
		                                             "CaloDigi"};

		// Disk maps are always saved to the ROOT file
		// enableDiskMaps controls disk map streaming only
		fhicl::Atom<bool> enableDiskMaps{fhicl::Name("enableDiskMaps"), true};

		// Disk map streaming selection only, examples {"asym"} or {"asym","sum"}
		fhicl::Sequence<std::string> diskCombines{fhicl::Name("diskCombines"),
		                                          std::vector<std::string>{"asym"}};
	};

	explicit CaloDigiDQM(const art::EDAnalyzer::Table<Config>& config);
	void analyze(art::Event const& event) override;
	void endJob() override;

  private:
	// -----------------------
	// Fixed waveform settings
	// -----------------------
	static constexpr int kWaveformNBins       = 64;   // Live waveform histogram binning
	static constexpr int kWaveformSizeHistMax = 200;  // Cap for size histogram axis

	// -----------------------
	// Disk-map streaming cadence
	// -----------------------
	static constexpr int kDiskMapsExtraPeriod = 100;  // Extra spacing beyond freqDQM

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

	// Parse a user string into a map mode, default is Amp for unknown values
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
		return MapMode::Amp;
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
			main   = "SiPM amplitude (peak - baseline)";
			ztitle = "Amplitude [ADC]";
			break;
		case MapMode::Sum:
			main   = "Crystal sum (L+R) amplitude";
			ztitle = "L+R [ADC]";
			break;
		case MapMode::Asym:
			main   = "Asymmetry (L-R)/(L+R)";
			ztitle = "Asymmetry";
			h->SetMinimum(-1.0);
			h->SetMaximum(1.0);
			break;
		case MapMode::Baseline:
			main   = "SiPM baseline";
			ztitle = "Baseline [ADC]";
			break;
		case MapMode::RMS:
			main   = "SiPM baseline RMS";
			ztitle = "RMS [ADC]";
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
		BoardOutOfRange,
		ChanOutOfRange,
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
		case SkipReason::BoardOutOfRange:
			return "BoardOutOfRange";
		case SkipReason::ChanOutOfRange:
			return "ChanOutOfRange";
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
		double amp{0.0};
		double baseline{0.0};
		double rms{0.0};
		double ampRaw{0.0};
		int    disk{-1};
		int    board{-1};
		int    chan{-1};
		int    cidx{-1};
	};
	// Stamp based validity avoids per event clearing of large vectors
	int                   pairStamp_{0};
	std::vector<SipmFeat> feat_;       // Indexed by sipmId
	std::vector<int>      featStamp_;  // featStamp[sipmId] equals pairStamp when valid
	std::vector<int> pairedStamp_;  // pairedStamp[crystalId] equals pairStamp when paired

	// -----------------------
	// Waveform caching
	// -----------------------
	std::vector<std::array<float, kWaveformNBins>> lastWf_;
	std::vector<uint16_t>                          lastWfSize_;
	std::vector<uint8_t>                           lastWfValid_;

	// Cache up to kWaveformNBins samples for streaming without rewalking the digi collection
	inline void cacheWaveform(int cidx, auto const& waveform, float baseline)
	{
		const int n = std::min<int>((int)waveform.size(), kWaveformNBins);
		for(int i = 0; i < n; ++i)
			lastWf_[(size_t)cidx][(size_t)i] = (float)waveform[(size_t)i] - baseline;
		for(int i = n; i < kWaveformNBins; ++i)
			lastWf_[(size_t)cidx][(size_t)i] = 0.0f;
		lastWfSize_[(size_t)cidx]  = (uint16_t)waveform.size();
		lastWfValid_[(size_t)cidx] = 1u;
	}

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
		for(int cidx = 0; cidx < kTotalChannels; ++cidx)
		{
			if(liveWf_[(size_t)cidx])
				flushCachedWaveformToHist(liveWf_[(size_t)cidx], cidx);
		}
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
	std::string   caloDigiModuleLabel_;

	int         freqDQM_{100};
	int         freqWaveforms_{0};
	std::string address_;
	int         port_{6000};
	std::string moduleTag_;
	bool        sendHists_{false};

	std::unique_ptr<ots::HistoSender> histSender_;
	int                               eventCounter_{0};
	int                               waveformCounter_{0};
	int                               histSendErrorCount_{0};
	static constexpr int              kMaxSendErrors_ = 10;

	// Streaming toggle for disk maps, saving is always enabled
	bool enableDiskMaps_{true};

	// Simple counters for coverage reporting
	int nFillDisk0_{0}, nFillDisk1_{0}, nFillMiss_{0};

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

	std::array<BoardHists, kTotalBoards>                           boardH_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardHistDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardChanDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardBaselineDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardRmsDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardMaxDir_{};
	std::array<std::unique_ptr<art::TFileDirectory>, kTotalBoards> boardAsymDir_{};

	// -----------------------
	// Per-channel waveforms
	// -----------------------
	std::vector<TH1F*>             liveWf_;
	std::vector<TH1F*>             oneHitWf_;
	std::vector<uint8_t>           oneHitSeen_;
	std::vector<uint8_t>           chSeen_;
	std::vector<WaveformSizeStats> wfStats_;
	std::vector<TH1F*>             h_baseline_dist_;
	std::vector<TH1F*>             h_rms_dist_;
	std::vector<TH1F*>             h_max_dist_;
	std::vector<TH1F*>             h_asym_dist_;

	std::vector<uint8_t> channelBooked_;

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
    , caloDigiModuleLabel_(config().caloDigiModuleLabel())
    , freqDQM_(config().freqDQM())
    , freqWaveforms_(config().freqWaveforms())
    , address_(config().address())
    , port_(config().port())
    , moduleTag_(config().moduleTag())
    , sendHists_(config().sendHists())
    , enableDiskMaps_(config().enableDiskMaps())
{
	// -----------------------
	// ROOT directory layout
	// -----------------------
	art::ServiceHandle<art::TFileService> tfs;
	disk0Dir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk0"));
	disk1Dir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk1"));
	globalDir_ = std::make_unique<art::TFileDirectory>(tfs->mkdir("Global_Histograms"));

	// -----------------------
	// Stream selection controls otsdaq streaming only
	// -----------------------
	std::vector<std::string> rawStream = config().diskCombines();
	if(rawStream.empty())
		rawStream = {"asym"};

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
	chSeen_.assign(kTotalChannels, 0u);
	wfStats_.assign(kTotalChannels, WaveformSizeStats{});
	h_baseline_dist_.assign(kTotalChannels, nullptr);
	h_rms_dist_.assign(kTotalChannels, nullptr);
	h_max_dist_.assign(kTotalChannels, nullptr);
	h_asym_dist_.assign(kTotalChannels, nullptr);
	channelBooked_.assign(kTotalChannels, 0u);

	lastWf_.assign(kTotalChannels, std::array<float, kWaveformNBins>{});
	lastWfSize_.assign(kTotalChannels, 0u);
	lastWfValid_.assign(kTotalChannels, 0u);

	feat_.assign((size_t)kMaxSipmIdForMaps_, SipmFeat{});
	featStamp_.assign((size_t)kMaxSipmIdForMaps_, 0);
	pairedStamp_.assign((size_t)(kMaxSipmIdForMaps_ / 2 + 2), 0);

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
	    "h_board_vs_channel", "Board vs Channel Occupancy", 160, 0, 160, 20, 0, 20);
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
	    globalDir_->make<TH1F>("h_board_dist", "Global Board Distribution", 160, 0, 160);
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

	// Local lambdas keep pairing logic readable and branch light
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

		// Range checks keep array indexing safe and skip counters informative
		if(chanID < 0 || chanID >= kChannelsPerBoard)
		{
			recordSkip(SkipReason::ChanOutOfRange);
			continue;
		}
		if(boardID < 0 || boardID >= (kBoardsPerDisk * kNDisks))
		{
			recordSkip(SkipReason::BoardOutOfRange);
			++nFillMiss_;
			continue;
		}

		const int disk = boardID / kBoardsPerDisk;
		if(disk < 0 || disk >= kNDisks)
		{
			recordSkip(SkipReason::DiskOutOfRange);
			++nFillMiss_;
			continue;
		}

		const int bmin = boardMinForDisk(disk);
		if(boardID < bmin || boardID >= bmin + kBoardsPerDisk)
		{
			recordSkip(SkipReason::BoardOutOfRange);
			++nFillMiss_;
			continue;
		}

		// Encoded is per disk, cidx is global across disks
		const int encodedSparse = encodeSparse(boardID, chanID);
		const int encodedDense  = encodeDense(boardID, chanID);
		const int cidx          = channelIndex(disk, boardID, chanID);
		if(cidx < 0 || cidx >= kTotalChannels)
		{
			recordSkip(SkipReason::ChanOutOfRange);
			++nFillMiss_;
			continue;
		}

		ensureBoardBooked(disk, boardID);

		if(!channelBooked_[(size_t)cidx])
		{
			ensureBaselineDistBooked(disk, boardID, chanID);
			ensureRmsDistBooked(disk, boardID, chanID);
			ensureMaxDistBooked(disk, boardID, chanID);
			ensureLiveWaveformBooked(disk, boardID, chanID, rawId, sipmId);
			channelBooked_[(size_t)cidx] = 1u;
		}
		// Track which channels were observed for endJob summaries
		chSeen_[(size_t)cidx] = 1u;

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
		}

		// Collect waveform length behavior per channel for integrity monitoring
		{
			const uint32_t sz = (uint32_t)wfSize;
			auto&          st = wfStats_[(size_t)cidx];

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

		// Peak position integrity checks
		const int peakpos = digi.peakpos();

		if(wfSize == 0 || peakpos < 0 || peakpos >= wfSize)
		{
			recordSkip(SkipReason::PeakPosOutOfRange);
			continue;
		}

		// Baseline and RMS from the first samples, robust against small wfSize
		const int nBase = std::min<int>(5, wfSize);

		float sum   = 0.0f;
		float sumsq = 0.0f;
		for(int i = 0; i < nBase; ++i)
		{
			const float x = waveform[(size_t)i];
			sum += x;
			sumsq += x * x;
		}

		const float baseline = sum / (float)nBase;
		const float mean_sq  = sumsq / (float)nBase;
		const float rms      = (mean_sq > baseline * baseline)
		                           ? std::sqrt(mean_sq - baseline * baseline)
		                           : 0.0f;

		if(!std::isfinite(baseline) || !std::isfinite(rms))
		{
			recordSkip(SkipReason::NonFiniteBaselineOrRms);
			continue;
		}

		if(h_baseline_dist_[(size_t)cidx])
			h_baseline_dist_[(size_t)cidx]->Fill(baseline);

		if(h_rms_dist_[(size_t)cidx])
			h_rms_dist_[(size_t)cidx]->Fill(rms);

		// Cache waveform for later streaming without reallocations
		cacheWaveform(cidx, waveform, baseline);

		// Book and fill waveform products for offline and streaming use
		ensureFirstHitBooked(
		    disk, boardID, chanID, rawId, sipmId, waveform, wfSize, baseline);

		// Peak amplitude is baseline subtracted, ampRaw is the raw peak sample
		const float  ampRaw = waveform[(size_t)peakpos];
		const double amp    = (double)ampRaw - (double)baseline;

		if(h_max_dist_[(size_t)cidx])
			h_max_dist_[(size_t)cidx]->Fill(ampRaw);

		if(h_amp_sparse_)
			h_amp_sparse_->Fill(encodedSparse, amp);
		if(h_amp_dense_)
			h_amp_dense_->Fill(encodedDense, amp);
		if(h_amp_dist_)
			h_amp_dist_->Fill(amp);

		if(h_baseline_sparse_)
			h_baseline_sparse_->Fill(encodedSparse, baseline);
		if(h_baseline_dense_)
			h_baseline_dense_->Fill(encodedDense, baseline);

		if(h_rms_sparse_)
			h_rms_sparse_->Fill(encodedSparse, rms);
		if(h_rms_dense_)
			h_rms_dense_->Fill(encodedDense, rms);

		if(h_maxval_sparse_)
			h_maxval_sparse_->Fill(encodedSparse, ampRaw);
		if(h_maxval_dense_)
			h_maxval_dense_->Fill(encodedDense, ampRaw);

		// Board-level profiles reuse bidx computed above
		if(bidx >= 0 && bidx < kTotalBoards)
		{
			auto& bh = boardH_[(size_t)bidx];
			if(bh.base)
				bh.base->Fill(chanID, baseline);
			if(bh.rms)
				bh.rms->Fill(chanID, rms);
			if(bh.max)
				bh.max->Fill(chanID, ampRaw);
		}

		// Accumulate per SiPM means for disk maps
		if(modeEnabled(MapMode::Amp))
			accDisk(MapMode::Amp, disk, sipmId, amp);
		if(modeEnabled(MapMode::Baseline))
			accDisk(MapMode::Baseline, disk, sipmId, baseline);
		if(modeEnabled(MapMode::RMS))
			accDisk(MapMode::RMS, disk, sipmId, rms);

		// Store features for later pairing into crystals
		markFeat(
		    sipmId,
		    SipmFeat{amp, baseline, rms, (double)ampRaw, disk, boardID, chanID, cidx});

		// Pair two SiPMs per crystal and compute sum and asymmetry once per pair
		const int crystalId = sipmId / 2;
		if(!paired(crystalId))
		{
			const int evenId = 2 * crystalId;
			const int oddId  = evenId + 1;

			if(featSeen(evenId) && featSeen(oddId))
			{
				markPaired(crystalId);

				const auto& fL = feat_[(size_t)evenId];
				const auto& fR = feat_[(size_t)oddId];

				const double L = fL.amp;
				const double R = fR.amp;

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

				const int dL = fL.disk;
				const int dR = fR.disk;

				if(modeEnabled(MapMode::Sum))
				{
					accDisk(MapMode::Sum, dL, evenId, sumLR);
					accDisk(MapMode::Sum, dR, oddId, sumLR);
				}
				if(modeEnabled(MapMode::Asym))
				{
					accDisk(MapMode::Asym, dL, evenId, asym);
					accDisk(MapMode::Asym, dR, oddId, asym);
				}

				// Disk mismatch is unexpected and indicates mapping inconsistency
				if(dL != dR)
				{
					mf::LogWarning("CaloDigiDQM")
					    << "Disk mismatch for paired crystal " << crystalId << " (SiPM "
					    << evenId << " in disk " << dL << ", SiPM " << oddId
					    << " in disk " << dR << ").";
				}
			}
		}
	}

	++eventCounter_;
	++waveformCounter_;

	// Decide which groups to send this event, avoid any work if nothing is due
	const bool doSummariesEvent = (freqDQM_ > 0) && (eventCounter_ % freqDQM_ == 0);
	const bool doWaveforms =
	    (freqWaveforms_ > 0) && (waveformCounter_ % freqWaveforms_ == 0);

	const int  diskMapPeriod = (freqDQM_ > 0) ? (freqDQM_ + kDiskMapsExtraPeriod) : 0;
	const bool doDiskMaps =
	    enableDiskMaps_ && (diskMapPeriod > 0) && (eventCounter_ % diskMapPeriod == 0);

	if(!doSummariesEvent && !doWaveforms && !doDiskMaps)
		return;

	// No sender means no streaming, keep ROOT output path independent
	if(!sendHists_ || !histSender_)
		return;

	// Materialize derived content only when it will be streamed
	if(doDiskMaps)
		refreshDiskMaps();

	if(doWaveforms)
		flushAllLiveWaveforms();

	// Group histograms by otsdaq folder, use :replace to refresh plots in place
	std::map<std::string, std::vector<TH1*>> hists_to_send;

	if(doSummariesEvent)
	{
		hists_to_send[moduleTag_ + "/Global:replace"] = {h_occupancy_sparse_,
		                                                 h_occupancy_dense_,
		                                                 h_baseline_sparse_,
		                                                 h_baseline_dense_,
		                                                 h_rms_sparse_,
		                                                 h_rms_dense_,
		                                                 h_maxval_sparse_,
		                                                 h_maxval_dense_,
		                                                 h_asymmetry,
		                                                 h_global_board_dist_,
		                                                 h_global_board_vs_channel_,
		                                                 h_global_waveform_density_,
		                                                 h_waveform_size_,
		                                                 h_skip_reason_};

		// Stream per board summaries only for boards that were booked
		for(int disk = 0; disk < kNDisks; ++disk)
		{
			for(int blocal = 0; blocal < kBoardsPerDisk; ++blocal)
			{
				const int boardID = boardIdFromDiskAndLocal(disk, blocal);
				const int bidx    = disk * kBoardsPerDisk + blocal;

				const auto& bh = boardH_[(size_t)bidx];
				if(!bh.occ)
					continue;

				std::string groupPath = Form(
				    "%s/Disk%d/Board%03d:replace", moduleTag_.c_str(), disk, boardID);

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
		// Stream waveforms grouped by disk and board to keep dashboard navigation stable
		for(int disk = 0; disk < kNDisks; ++disk)
		{
			for(int blocal = 0; blocal < kBoardsPerDisk; ++blocal)
			{
				const int boardID = boardIdFromDiskAndLocal(disk, blocal);

				std::string livePath = Form("%s/Waveforms/Disk%d/Board%03d:replace",
				                            moduleTag_.c_str(),
				                            disk,
				                            boardID);
				std::string oneHitPath =
				    Form("%s/OneHitWaveforms/Disk%d/Board%03d:replace",
				         moduleTag_.c_str(),
				         disk,
				         boardID);

				for(int chan = 0; chan < kChannelsPerBoard; ++chan)
				{
					const int cidx =
					    disk * kChannelsPerDisk + blocal * kChannelsPerBoard + chan;

					if(liveWf_[(size_t)cidx])
						hists_to_send[livePath].push_back(liveWf_[(size_t)cidx]);
					if(oneHitWf_[(size_t)cidx])
						hists_to_send[oneHitPath].push_back(oneHitWf_[(size_t)cidx]);
				}
			}
		}
	}

	// Streaming failures are rate limited by disabling sendHists_ after repeated errors
	try
	{
		histSender_->sendHistograms(hists_to_send);
		histSendErrorCount_ = 0;
	}
	catch(const std::exception& e)
	{
		++histSendErrorCount_;
		mf::LogError("CaloDigiDQM") << "HistoSender::sendHistograms exception ("
		                            << histSendErrorCount_ << "): " << e.what();
		if(histSendErrorCount_ >= kMaxSendErrors_)
			sendHists_ = false;
	}
	catch(...)
	{
		++histSendErrorCount_;
		mf::LogError("CaloDigiDQM") << "HistoSender::sendHistograms non-std exception ("
		                            << histSendErrorCount_ << ").";
		if(histSendErrorCount_ >= kMaxSendErrors_)
			sendHists_ = false;
	}
}

// ===========================
// endJob()
// ===========================
void CaloDigiDQM::endJob()
{
	// Final flush ensures ROOT output captures last cached values
	flushAllLiveWaveforms();
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
	                           << " d1=" << nFillDisk1_ << " miss=" << nFillMiss_
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
	for(int cidx = 0; cidx < kTotalChannels; ++cidx)
	{
		if(!chSeen_[(size_t)cidx])
			continue;
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

	const size_t top = std::min<size_t>(20, offenders.size());
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

			os << "  (D" << disk << " B" << boardID << " C" << chan << ")"
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
