// CRV metrics for graphite, via the artdaq MetricManager. No histograms: the
// CRV DQM histograms are CrvDQM's. This module counts straight from the
// unpacked products and sends:
//
// Error counts, summed over ROCs/links, one per error type (RunningSum: the
// total since the process started, never reset, so a plot shows the slope and
// a quiet detector reads flat). Where each is localised is in the DQM
// histograms, not here.
//   CRV.Errors.Hardware.<bit>   ROC-reported: MicroBunchStatus error bits
//                               (FEBuBMismatch, FEBBufferIssue, FEBOverflow,
//                               uBMatchError, Truncation; the Group*Issue bits
//                               are left to the DQM), one ROC header = one count
//   CRV.Errors.Link.<bit>       DTC link status bits (ROCTimeout, SeqNumErr,
//                               CRCErr, FatalErr, Error), one status block = one
//   CRV.Errors.Link.LatencyInvalid   0xFFFF latency words (no DTC measurement)
//   CRV.Errors.DAQ.RocDtcEwtMismatch ROC headers whose event window tag differs
//                               from the DTC data header's
//   CRV.Errors.DAQ.<code>       CrvDAQerror codes from the unpacker
//
// Occupancy (Average over the metric interval, so independent of the
// dispatcher prescale): digis per event, in total and per global FEB ID of the
// configured readout (0 for a FEB with no digi in an event).
//   CRV.DigisPerEvent
//   CRV.FEB<global FEB ID>.DigisPerEvent
//
// Every series is sent every event, zeros included, so none goes stale. The
// metrics block that routes them (graphite host, levels) is the art process's
// ArtdaqGlobalsService configuration, not this module's.
//
// Defaults match the otsdaq online job (unpacker label "crvDigi", KPP
// readout), so the analyzer needs no parameters there.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"

#include "artdaq-utilities/Plugins/MetricManager.hh"
#include "artdaq/DAQdata/Globals.hh"
#include "cetlib_except/exception.h"

#include "Offline/CRVDQM/inc/CRVDQMRun1.hh"
#include "Offline/CRVDQM/inc/CRVStatusDQM.hh"
#include "Offline/RecoDataProducts/inc/CrvDAQerror.hh"
#include "Offline/RecoDataProducts/inc/CrvDigi.hh"
#include "Offline/RecoDataProducts/inc/CrvStatus.hh"

#include "TString.h"

namespace
{
// ROC-reported error bits sent as metrics: indices into CRVStatusDQM's eight
// MicroBunchStatus error bits (24-31), without the three Group*Issue bits.
constexpr int kNHardwareBits                = 5;
constexpr int kHardwareBits[kNHardwareBits] = {0, 1, 2, 6, 7};

// DTC link status bits, as CRVStatusDQM histograms them.
constexpr int         kNLinkStatusBits                      = 5;
constexpr uint8_t     kLinkStatusBits[kNLinkStatusBits]     = {0, 2, 3, 6, 7};
constexpr const char* kLinkStatusBitNames[kNLinkStatusBits] = {
    "ROCTimeout", "SeqNumErr", "CRCErr", "FatalErr", "Error"};

// The CRVDQMRun1 configuration named by `configuration` (its layout name,
// default extracted = KPP); it sets which global FEB IDs get a series.
int configurationFromFhicl(fhicl::ParameterSet const& ps)
{
	const std::string name = ps.get<std::string>("configuration", "extracted");
	for(int c = 0; c < mu2e::CRVDQMRun1::kNConfigurations; ++c)
		if(name == mu2e::CRVDQMRun1::configurationName(c))
			return c;
	throw cet::exception("CrvStatusMetrics")
	    << "configuration \"" << name << "\" is not a CRVDQMRun1 configuration "
	    << "(run1a, extracted).\n";
}

// Keys of the module's earlier, histogramming version. Rejected rather than
// silently ignored.
constexpr const char* kRemovedKeys[] = {"status",
                                        "sendHists",
                                        "address",
                                        "port",
                                        "sendIntervalSec",
                                        "outputTag",
                                        "segmentation",
                                        "fillLivePlots"};
}  // namespace

namespace ots
{

class CrvStatusMetrics : public art::EDAnalyzer
{
  public:
	explicit CrvStatusMetrics(fhicl::ParameterSet const& ps);
	~CrvStatusMetrics() override = default;

  private:
	void beginJob() override;
	void analyze(art::Event const& e) override;
	void endJob() override;

	void sendErrorMetrics(const mu2e::CrvStatusCollection&   crvStatus,
	                      const mu2e::CrvDAQerrorCollection* crvDaqErrors) const;
	void sendDigiMetrics(const mu2e::CrvDigiCollection& crvDigis) const;

	template<typename T>
	void sendMetric(const std::string& name,
	                T                  value,
	                const std::string& units,
	                artdaq::MetricMode mode) const;

	int           configuration_;
	art::InputTag crvStatusTag_;
	art::InputTag crvDaqErrorTag_;
	art::InputTag crvDigiTag_;
	bool          sendErrorMetrics_;
	bool          sendDigiMetrics_;
	int           diagLevel_;
	int           metricLevel_;

	size_t      eventCount_{0};
	size_t      noStatusCount_{0};
	size_t      noDigiCount_{0};
	std::string outputPrefix_;
};

CrvStatusMetrics::CrvStatusMetrics(fhicl::ParameterSet const& ps)
    : art::EDAnalyzer(ps)
    , configuration_(configurationFromFhicl(ps))
    , crvStatusTag_(ps.get<std::string>("crvStatusTag", "crvDigi"))
    , crvDaqErrorTag_(ps.get<std::string>("crvDaqErrorTag", "crvDigi"))
    , crvDigiTag_(ps.get<std::string>("crvDigiTag", "crvDigi"))
    , sendErrorMetrics_(ps.get<bool>("sendErrorMetrics", true))
    , sendDigiMetrics_(ps.get<bool>("sendDigiMetrics", true))
    , diagLevel_(ps.get<int>("diagLevel", 1))
    , metricLevel_(ps.get<int>("metricLevel", 3))
{
	outputPrefix_ = "[CrvStatusMetrics] ";
	for(const char* key : kRemovedKeys)
	{
		if(ps.has_key(key))
		{
			throw cet::exception("CrvStatusMetrics")
			    << "parameter \"" << key << "\" no longer exists: CrvStatusMetrics sends "
			    << "metrics only; the status histograms are CrvDQM's.\n";
		}
	}
}

void CrvStatusMetrics::beginJob()
{
	if(diagLevel_ > 0)
	{
		std::cout << outputPrefix_ << "beginJob: configuration="
		          << mu2e::CRVDQMRun1::configurationName(configuration_) << " ("
		          << mu2e::CRVDQMRun1::nFebs(configuration_) << " FEB series)"
		          << " metricLevel=" << metricLevel_ << " errors=" << sendErrorMetrics_
		          << " digis=" << sendDigiMetrics_
		          << " metricManager=" << (metricMan ? "yes" : "NO (nothing is sent)")
		          << std::endl;
	}
}

template<typename T>
void CrvStatusMetrics::sendMetric(const std::string& name,
                                  T                  value,
                                  const std::string& units,
                                  artdaq::MetricMode mode) const
{
	if(metricMan != nullptr)
	{
		metricMan->sendMetric(name, value, units, metricLevel_, mode);
	}
}

void CrvStatusMetrics::sendErrorMetrics(
    const mu2e::CrvStatusCollection&   crvStatus,
    const mu2e::CrvDAQerrorCollection* crvDaqErrors) const
{
	using mu2e::CRVStatusDQM;
	uint64_t firmware[CRVStatusDQM::kNErrorBits] = {};
	uint64_t link[kNLinkStatusBits]              = {};
	uint64_t latencyInvalid = 0, ewtMismatch = 0;

	for(const auto& status : crvStatus)
	{
		const uint8_t linkStatus = status.GetLinkStatus();
		for(int i = 0; i < kNLinkStatusBits; ++i)
			if((linkStatus >> kLinkStatusBits[i]) & 1u)
				++link[i];
		if(status.GetLinkLatency() == CRVStatusDQM::kLatencySentinel)
			++latencyInvalid;

		if(!status.HasROCHeader())
			continue;
		const auto&    roc      = status.GetROCHeader().front();
		const uint32_t ubStatus = roc.GetMicroBunchStatus();
		for(int b = 0; b < CRVStatusDQM::kNErrorBits; ++b)
			if((ubStatus >> (CRVStatusDQM::kErrorBitOffset + b)) & 1u)
				++firmware[b];
		if(roc.GetEventWindowTag() != status.GetEventWindowTag())
			++ewtMismatch;
	}

	const auto sum = artdaq::MetricMode::RunningSum;
	for(int b : kHardwareBits)
		sendMetric(std::string("CRV.Errors.Hardware.") + CRVStatusDQM::errorBitLabel(b),
		           firmware[b],
		           "counts",
		           sum);
	for(int i = 0; i < kNLinkStatusBits; ++i)
		sendMetric(std::string("CRV.Errors.Link.") + kLinkStatusBitNames[i],
		           link[i],
		           "counts",
		           sum);
	sendMetric("CRV.Errors.Link.LatencyInvalid", latencyInvalid, "counts", sum);
	sendMetric("CRV.Errors.DAQ.RocDtcEwtMismatch", ewtMismatch, "counts", sum);

	// Every known code is sent, zero included, so the set of series is fixed.
	std::map<int, uint64_t> daq;
	for(const auto& [code, name] : mu2e::CrvDAQerrorCodeDetail::names())
		daq[static_cast<int>(code)] = 0;
	if(crvDaqErrors != nullptr)
		for(const auto& err : *crvDaqErrors)
			++daq[static_cast<int>(err.GetErrorCode())];
	for(const auto& [code, name] : mu2e::CrvDAQerrorCodeDetail::names())
		sendMetric("CRV.Errors.DAQ." + name, daq[static_cast<int>(code)], "counts", sum);
}

void CrvStatusMetrics::sendDigiMetrics(const mu2e::CrvDigiCollection& crvDigis) const
{
	using namespace mu2e::CRVDQMRun1;
	const int             nFeb = nFebs(configuration_);
	std::vector<uint64_t> perFeb(nFeb, 0);
	for(const auto& digi : crvDigis)
	{
		const int roc = digi.GetROC(), feb = digi.GetFEB();
		if(!onlineIdInRange(roc, feb, digi.GetFEBchannel()))
			continue;
		const int globalFeb = globalFebId(roc, feb);
		if(febInConfiguration(configuration_, globalFeb))
			++perFeb[globalFeb];
	}
	const auto avg = artdaq::MetricMode::Average;
	sendMetric("CRV.DigisPerEvent", static_cast<uint64_t>(crvDigis.size()), "digis", avg);
	for(int f = 0; f < nFeb; ++f)
		sendMetric(Form("CRV.FEB%02d.DigisPerEvent", f), perFeb[f], "digis", avg);
}

void CrvStatusMetrics::analyze(art::Event const& e)
{
	++eventCount_;

	if(sendErrorMetrics_)
	{
		art::Handle<mu2e::CrvStatusCollection> statusHandle;
		e.getByLabel(crvStatusTag_, statusHandle);
		if(statusHandle.isValid() && statusHandle.product() != nullptr)
		{
			art::Handle<mu2e::CrvDAQerrorCollection> daqHandle;
			e.getByLabel(crvDaqErrorTag_, daqHandle);
			const bool haveDaq = daqHandle.isValid() && daqHandle.product() != nullptr;
			sendErrorMetrics(*statusHandle, haveDaq ? daqHandle.product() : nullptr);
		}
		else
		{
			++noStatusCount_;
			if(diagLevel_ > 1)
				std::cout << outputPrefix_ << e.id() << " no CrvStatus at "
				          << crvStatusTag_ << "; no error metrics" << std::endl;
		}
	}

	if(sendDigiMetrics_)
	{
		art::Handle<mu2e::CrvDigiCollection> digiHandle;
		e.getByLabel(crvDigiTag_, digiHandle);
		if(digiHandle.isValid() && digiHandle.product() != nullptr)
		{
			sendDigiMetrics(*digiHandle);
		}
		else
		{
			++noDigiCount_;
			if(diagLevel_ > 1)
				std::cout << outputPrefix_ << e.id() << " no CrvDigi at " << crvDigiTag_
				          << "; no digi metrics" << std::endl;
		}
	}
}

void CrvStatusMetrics::endJob()
{
	if(diagLevel_ > 0)
	{
		std::cout << outputPrefix_ << "endJob: events=" << eventCount_
		          << " without CrvStatus=" << noStatusCount_
		          << " without CrvDigi=" << noDigiCount_ << std::endl;
	}
}

}  // namespace ots

DEFINE_ART_MODULE(ots::CrvStatusMetrics)
