// DQM and viewer for the CRV
// Sends histograms to otsdaq visualizer and standalone THttpServer
// Sam Grant, Simon Corrodi
//
// Histogram booking/filling is owned by mu2e::CRVDigiDQM (digis) and
// mu2e::CRVStatusDQM (per-link status, under status/) in Offline/CRVDQM,
// whose binning is fixed. This module keeps I/O, HistoSender, THttpServer,
// styling, display zoom, and PDF export. What is published is chosen by the
// `digi` and `status` tables (Offline/CRVDQM/fcl/prolog.fcl).

// C++ includes
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// art includes
#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art/Framework/Principal/Run.h"
#include "art/Framework/Principal/SubRun.h"
#include "cetlib_except/exception.h"

// art/root includes
#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"

// ROOT includes
#include <TCanvas.h>
#include <TColor.h>
#include <TDirectory.h>
#include <TGraph.h>
#include <TH1.h>
#include <TH2.h>
#include <THttpServer.h>
#include <TPad.h>
#include <TPaveStats.h>
#include <TRandom3.h>
#include <TStyle.h>
#include <TSystem.h>

// OTS includes
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/Macros/ProcessorPluginMacros.h"

// Offline includes
#include "Offline/CRVConditions/inc/CRVOrdinal.hh"
#include "Offline/CRVConditions/inc/CRVStatus.hh"
#include "Offline/CosmicRayShieldGeom/inc/CosmicRayShield.hh"
#include "Offline/CRVDQM/inc/CRVDQMLayout.hh"
#include "Offline/CRVDQM/inc/CRVDQMRun1.hh"
#include "Offline/CRVDQM/inc/CRVDigiDQM.hh"
#include "Offline/CRVDQM/inc/CRVStatusDQM.hh"
#include "Offline/DQMHelpers/inc/DQMHistSetConfig.hh"
#include "Offline/DQMHelpers/inc/DQMStyle.hh"
#include "Offline/GeometryService/inc/GeomHandle.hh"
#include "Offline/ProditionsService/inc/ProditionsHandle.hh"
#include "Offline/RecoDataProducts/inc/CrvDigi.hh"
#include "Offline/RecoDataProducts/inc/CrvStatus.hh"

namespace
{
// What the online display shows, declared once.
struct HistPad
{
	const char* name;       // as mu2e::CRVDigiDQM books it
	int         pad;        // 1-based canvas pad
	const char* drawOpt;
	bool        logx;
	bool        logy;
	bool        autoRange;  // rescale the Y axis from the data on every refresh
	double      yFloor;     // lower edge, used for both SetMinimum and autoRange
};

// The full-window ("2") views: KPP's ~400 us readout overflows the short ones.
constexpr HistPad kHistPads[] = {
    {"h1_digisPerEvt2", 2, "HIST", true, true, true, 0.5},
    {"h1_peakAdc", 3, "HIST", false, false, false, 0.0},
    {"h1_tdc2", 4, "HIST", false, false, false, 0.0},
    {"h1_channels", 5, "HIST", false, false, true, 0.5},
    {"h2_channels", 6, "COLZ", false, false, false, 0.0},
    // Partner-FEB timing: a slipped FEB is a displaced column.
    {"dtPartner_SameModuleSameSide", 8, "COLZ", false, false, false, 0.0},
};

// The two EWT graphs keep their own pads: their axis handling is bespoke
// (a sliding X window tied to the event window tag), not table-driven.
constexpr int kPadDigisVsEwt     = 1;
constexpr int kPadDigisAvgVsEwt  = 7;
constexpr int kPadInvalidLatency = 9;

constexpr int kCanvasCols = 3;
constexpr int kCanvasRows = 3;
static_assert(kCanvasCols * kCanvasRows >= kPadInvalidLatency,
              "canvas division is too small for the declared pads");

// Width of the sliding EWT window on the digis-vs-EWT pad (display only).
constexpr double kEwtXRange = 1000000;

// Keys the module took before the DQM clients fixed their binning. Rejected,
// because a plain ParameterSet would otherwise ignore them silently.
constexpr const char* kRemovedKeys[] = {
    "cfFraction",     "minAmplitude",   "dtBinSize",          "dtRange",
    "dtVsFebBinSize", "dtVsFebRange",   "nBinsDigisPerEvt",   "maxDigisPerEvt",
    "nBinsPeakAdc",   "maxPeakAdc",     "nBinsTdc",           "maxTdc",
    "avgBlockSize",   "avgGraphPoints", "channelsWindowEwts", "fillCrvIdRates",
    "kppReadout",     "fillLivePlots",  "statusGraphs",       "maxLatencyGraphPoints",
    "segmentation",   "statusSegmentation"};

void rejectRemovedKeys(fhicl::ParameterSet const& ps)
{
	for(const char* key : kRemovedKeys)
	{
		if(ps.has_key(key))
		{
			throw cet::exception("CrvDQM")
			    << "parameter \"" << key << "\" no longer exists: binning and CF timing are "
			    << "fixed in Offline/CRVDQM, and copies/publishing are chosen by the "
			    << "`digi` and `status` tables (Offline/CRVDQM/fcl/prolog.fcl).\n";
		}
	}
}

// A client's `hists` table, validated; an absent table is the job-only default.
mu2e::DQMHistSet::Config clientHists(fhicl::ParameterSet const& ps, const char* key)
{
	return mu2e::toConfig(
	    fhicl::Table<mu2e::DQMClientFhicl>(ps.get<fhicl::ParameterSet>(key, {}))().hists());
}

// Workaround for ROOT fatal "TPad::Range: y1 == y2 == 0" on empty histograms drawn
// in the web canvas. Put a tiny entry into bin 1 so max_bin_content > 0; it is
// overwritten as soon as real data arrives.
void seedEmptyFrame(TH1* h)
{
	if(!h)
		return;
	if(h->GetMaximum() <= h->GetMinimum())
	{
		h->SetBinContent(1, 1e-9);
		h->SetEntries(0);
	}
}

}  // namespace

namespace ots
{

class CrvDQM : public art::EDAnalyzer
{
  public:
	// Constructor
	explicit CrvDQM(fhicl::ParameterSet const& ps);
	// Destructor
	~CrvDQM() override;

  private:
	// Standard art methods
	void analyze(art::Event const& event) override;
	void beginJob() override;
	void beginRun(art::Run const& run) override;
	void beginSubRun(art::SubRun const& subRun) override;
	void endSubRun(art::SubRun const& subRun) override;
	void endJob() override;

	/// Module methods
	void Send();
	// What `publish` selected, printed once so an operator can see whether a
	// FHiCL change did what they meant.
	void logPublished();
	// The job copy of a digi histogram.
	TH1* jobCopy(const char* name);
	// The digi client's live graphs; nullptr unless its table sets liveSeries.
	TGraph* digiGraph(const char* name);
	// Style and register with the HTTP server any status object the helper
	// booked since the last call (per-link graphs appear on a link's first status).
	void registerNewStatusObjects();
	// Invalid (0xFFFF) link-latency words / status blocks, per link, over the
	// rolling window when the status table keeps one, else over the run.
	void updateInvalidLatencyRate();
	// Display-only zoom of the FEB-port axes onto the ports that have data.
	void zoomToActivePorts();
	// Geometry + channel map -> the sector map and FEB topology the digi client
	// needs for its per-sector occupancy and its partner-FEB timing. Redone on
	// every new run, since the channel map can change between runs.
	void updateLayout(art::Event const& event);
	void startHttpServer();
	void stopHttpServer();
	void updateWebDisplay(bool force = false);

	// fcl parameters
	art::InputTag crvDigiTag_;    // producer module label
	art::InputTag crvStatusTag_;  // CrvStatus producer module label
	int           diagLevel_;
	int           port_;  // port to connect to
	std::string   address_;
	std::string   outputTag_;
	bool          sendHists_;
	bool          dummyHist_;
	bool          saveCanvasesToPdf_;
	bool          showSameFpgaTimingInCanvas_;
	std::string   canvasPdfFile_;

	// HISTOGRAM SENDING
	std::unique_ptr<HistoSender> histoSender_;
	float                        sendIntervalSec_;

	// ROOT TFileService
	art::ServiceHandle<art::TFileService> tfs_;

	// Geometry-derived layout, injected into the digi client
	mu2e::ProditionsHandle<mu2e::CRVOrdinal> channelMap_;
	mu2e::ProditionsHandle<mu2e::CRVStatus>  sipmStatus_;
	int                                      layoutRun_{-1};
	bool                                     warnedLayout_{false};

	// Digi DQM histograms (occupancy, ADC, TDC, CF timing, EWT graphs)
	mu2e::CRVDigiDQM dqm_;
	// Per-link status summaries and graphs (status/, status/graphs/)
	mu2e::CRVStatusDQM statusDqm_;
	std::set<TObject*> registeredStatus_;
	// Display-only, not part of the fixed set: filled from the status client.
	std::unique_ptr<TH1F> h_invalidLatencyRate_;
	// End-of-job FPGA-pair slices drawn on the timing canvases.
	std::vector<std::unique_ptr<TH1>> timingSlices_;

	// Dummy histogram for HistoSender/THttpServer plumbing tests
	TH1F* h1_dummy_;

	// HTTP server & visualisation
	bool                                               enableHttpServer_;
	int                                                httpPort_;
	float                                              onlineRefreshPeriodMs_;
	std::string                                        histColor_;
	std::string                                        canvasName_;
	std::string                                        httpDefaultPage_;
	TCanvas*                                           webCanvas_;
	THttpServer*                                       httpServer_;
	std::chrono::time_point<std::chrono::steady_clock> lastRefreshTime_;

	// Event counter for display refresh (includes dummyHist events)
	std::size_t eventCounts_{0};

	// Rate counters (printed every statLogPeriodSec_ seconds at diag level 0)
	double      statLogPeriodSec_{10.0};
	std::size_t statAnalyze_{0};
	std::size_t statUpdate_{0};
	std::size_t statUpdateCalls_{0};  // all calls, including those gated out
	std::size_t statUpdateGateA_{0};  // returned because disabled / no canvas
	std::size_t statUpdateGateB_{0};  // returned because refresh period not elapsed
	std::size_t statProcEvents_{0};
	std::size_t statSend_{0};
	std::chrono::time_point<std::chrono::steady_clock> statLastLog_;

	// Misc member variables
	std::chrono::time_point<std::chrono::steady_clock> lastSendTime_;
	std::string                                        outputPrefix_;
	TRandom3                                           random_;
};

// Constructor impl
CrvDQM::CrvDQM(fhicl::ParameterSet const& ps)
    : art::EDAnalyzer(ps)
    , crvDigiTag_(ps.get<std::string>("crvDigiTag", "CrvDigi"))
    , crvStatusTag_(ps.get<std::string>("crvStatusTag", crvDigiTag_.label()))
    , diagLevel_(ps.get<int>("diagLevel", 3))
    , port_(ps.get<int>("port", 6000))
    , address_(ps.get<std::string>("address", "localhost"))
    , outputTag_(ps.get<std::string>("outputTag", "CrvDQM"))
    , sendHists_(ps.get<bool>("sendHists", true))
    , dummyHist_(ps.get<bool>("dummyHist", false))
    , saveCanvasesToPdf_(ps.get<bool>("saveCanvasesToPdf", false))
    , showSameFpgaTimingInCanvas_(ps.get<bool>("showSameFpgaTimingInCanvas", true))
    , canvasPdfFile_(ps.get<std::string>("canvasPdfFile", "CrvDQM.pdf"))
    , sendIntervalSec_(ps.get<float>("sendIntervalSec", 0.5))
    , dqm_((rejectRemovedKeys(ps), clientHists(ps, "digi")))
    , statusDqm_(clientHists(ps, "status"))
    , h1_dummy_(nullptr)
    , enableHttpServer_(ps.get<bool>("enableHttpServer", true))
    , httpPort_(ps.get<int>("httpPort", 8877))
    , onlineRefreshPeriodMs_(ps.get<float>("onlineRefreshPeriod", 500.f))
    , histColor_(ps.get<std::string>("histColor", "black"))
    , canvasName_(ps.get<std::string>("canvasName", "CrvDisplay"))
    , httpDefaultPage_(ps.get<std::string>(
          "httpDefaultPage", "$OTS_SOURCE/otsdaq-mu2e-dqm/UserWebGUI/html/CrvDQM.html"))
    , webCanvas_(nullptr)
    , httpServer_(nullptr)
{
	outputPrefix_ = "[CrvDQM] ";
	std::cout << outputPrefix_ << "Initialised"
	          << " (onlineRefreshPeriodMs=" << onlineRefreshPeriodMs_
	          << ", sendIntervalSec=" << sendIntervalSec_
	          << ", enableHttpServer=" << enableHttpServer_ << ")" << std::endl;
	// ROOT::EnableThreadSafety();
}

// Destructor impl
CrvDQM::~CrvDQM()
{
	// Nothing to clean up
}

void CrvDQM::beginSubRun(art::SubRun const& subRun)
{
	if(dummyHist_)
		return;
	const int run    = static_cast<int>(subRun.run());
	const int subrun = static_cast<int>(subRun.subRun());
	dqm_.BeginSubRun(run, subrun);
	statusDqm_.BeginSubRun(run, subrun);
}

void CrvDQM::endSubRun(art::SubRun const&)
{
	if(dummyHist_)
		return;
	dqm_.EndSubRun();
	statusDqm_.EndSubRun();
}

void CrvDQM::beginRun(art::Run const& run)
{
	// The DQM art process can outlive a run, and everything here is streamed in replace
	// mode without ever being cleared, so a new run would otherwise inherit the previous
	// run's contents. Objects stay booked (and registered with the HTTP server); only
	// their contents and the rolling windows behind them are cleared.
	std::cout << outputPrefix_ << "Run " << run.run()
	          << ": resetting histograms, graphs, and rolling windows" << std::endl;

	if(dummyHist_)
	{
		if(h1_dummy_)
			h1_dummy_->Reset("ICES");
		return;
	}

	dqm_.ResetForNewRun();
	statusDqm_.ResetForNewRun();
	if(h_invalidLatencyRate_)
		h_invalidLatencyRate_->Reset("ICES");

	// The web canvas draws these directly; keep them drawable while empty.
	if(enableHttpServer_ && webCanvas_)
	{
		for(const auto& spec : kHistPads)
			seedEmptyFrame(jobCopy(spec.name));
		seedEmptyFrame(h_invalidLatencyRate_.get());
	}
}

void CrvDQM::beginJob()
{
	// Apply styling before booking histograms so they inherit the style
	mu2e::DQMStyle::SetStyle();

	if(diagLevel_ > 1)
	{
		std::cout << outputPrefix_ << "Beginning job" << std::endl;
	}

	// Initialise histoSender
	if(sendHists_)
	{
		try
		{
			histoSender_ = std::make_unique<HistoSender>(address_, port_);
			std::cout << outputPrefix_ << "Successfully connected HistoSender to "
			          << address_ << ":" << port_ << std::endl;
		}
		catch(const std::exception& e)
		{
			std::cout << outputPrefix_ << "Failed to initialise HistoSender: " << e.what()
			          << std::endl;
			// Disable histogram sending if connection fails
			sendHists_ = false;
		}
	}

	art::TFileDirectory dir = tfs_->mkdir(outputTag_);
	if(dummyHist_)
	{
		h1_dummy_ = dir.make<TH1F>("h1_dummy", "Dummy Gaussian", 200, -100, 100);
	}
	else
	{
		dqm_.Book(dir);
		statusDqm_.Book(tfs_->mkdir(outputTag_ + "/status"));
		for(const char* name : {"g_digisVsEwt", "g_digisAvgVsEwt"})
		{
			if(TGraph* g = digiGraph(name))
			{
				mu2e::DQMStyle::FormatGraph(g, histColor_);
				g->SetMarkerColor(g->GetLineColor());
				g->SetDrawOption("AP");
			}
		}
		const mu2e::DQMAxis link = mu2e::CRVStatusDQM::kLink;
		h_invalidLatencyRate_ = std::make_unique<TH1F>(
		    "linkLatencyInvalidRate",
		    "Invalid link-latency words / status blocks;DTC#times6 + link ID;Fraction",
		    link.n, link.lo, link.hi);
		h_invalidLatencyRate_->SetDirectory(nullptr);
	}

	// Seed TRandom3
	random_.SetSeed(12345);

	// Start last update time
	lastSendTime_    = std::chrono::steady_clock::now();
	lastRefreshTime_ = lastSendTime_;
	statLastLog_     = lastSendTime_;

	if(enableHttpServer_)
	{
		try
		{
			startHttpServer();
		}
		catch(const std::exception& e)
		{
			std::cout << outputPrefix_ << "Failed to start HTTP server: " << e.what()
			          << std::endl;
			enableHttpServer_ = false;
		}
	}

	// Everything in the status set is booked up front; per-link graphs follow per link.
	registerNewStatusObjects();

	updateWebDisplay();
}

void CrvDQM::Send()
{
	// Check flag
	if(!sendHists_)
	{
		return;
	}

	// Check pointer
	if(histoSender_ == nullptr)
	{
		std::cout << outputPrefix_ << "ERROR: histoSender pointer is null" << std::endl;
		return;
	}

	// The live segment copies are labelled with the range they hold; refresh
	// that before shipping, so a title read on the GUI is never behind.
	dqm_.hists().RefreshLabels();
	statusDqm_.hists().RefreshLabels();

	// Use the map method (three methods in HistoSender.cc)
	std::map<std::string, std::vector<TH1*>> hists;
	if(dummyHist_)
	{
		hists["crv/h1_gaus:replace"] = {h1_dummy_};
	}
	else
	{
		for(const auto* set : {&dqm_.hists(), &statusDqm_.hists()})
		{
			for(const auto& [group, copies] : set->publishedCopies())
			{
				auto& out = hists["crv/" + group + ":replace"];
				out.insert(out.end(), copies.begin(), copies.end());
			}
		}
		updateInvalidLatencyRate();
		hists["crv/linkLatencyInvalidRate:replace"] = {h_invalidLatencyRate_.get()};
	}

	// Call send method
	histoSender_->sendHistograms(hists);
	++statSend_;

	// Send graphs: whatever the two clients booked (their tables' liveSeries)
	if(!dummyHist_)
	{
		std::map<std::string, std::vector<TGraph*>> graphs;
		auto& out = graphs["crv/graphs:replace"];
		for(const auto* series : {&dqm_.series(), &statusDqm_.series()})
			out.insert(out.end(), series->graphs().begin(), series->graphs().end());
		if(!out.empty())
			histoSender_->sendGraphs(graphs);
	}

	if(diagLevel_ > 1)
	{
		std::cout << outputPrefix_ << "Sent histograms to " << address_ << ":" << port_
		          << std::endl;
	}
}

void CrvDQM::logPublished()
{
	auto published = dqm_.hists().publishedCopies();
	for(const auto& [group, copies] : statusDqm_.hists().publishedCopies())
	{
		auto& out = published[group];
		out.insert(out.end(), copies.begin(), copies.end());
	}
	std::cout << outputPrefix_ << "publishing " << published.size() + 1
	          << " HistoSender group(s):" << std::endl;
	for(const auto& [group, copies] : published)
	{
		std::cout << outputPrefix_ << "  crv/" << group << ":replace  ("
		          << copies.size() << ")";
		// Naming every member of a large group helps nobody.
		if(copies.size() <= 8)
		{
			for(TH1* h : copies)
				std::cout << " " << h->GetName();
		}
		std::cout << std::endl;
	}
	std::cout << outputPrefix_ << "  crv/linkLatencyInvalidRate:replace  (1)" << std::endl;
}

TH1* CrvDQM::jobCopy(const char* name)
{
	const auto copies = dqm_.hists().copies(name);
	return copies.empty() ? nullptr : copies.front();
}

TGraph* CrvDQM::digiGraph(const char* name)
{
	return dqm_.series().get(name).graph();
}

void CrvDQM::registerNewStatusObjects()
{
	if(dummyHist_)
		return;
	for(TH1* h : statusDqm_.hists().allCopies())
	{
		if(!registeredStatus_.insert(h).second)
			continue;
		if(auto* h2 = dynamic_cast<TH2*>(h))
		{
			mu2e::DQMStyle::FormatHist2D(h2);
			h2->SetOption("COLZ");
		}
		else
		{
			mu2e::DQMStyle::FormatHist(h, histColor_);
			if(std::string(h->GetName()).rfind("h1_latency", 0) == 0)
			{
				h->SetStatOverflows(TH1::kConsider);
				h->SetStats(1);
			}
		}
		if(enableHttpServer_ && httpServer_)
			httpServer_->Register("/", h);
	}
	for(TGraph* g : statusDqm_.series().graphs())
	{
		if(!registeredStatus_.insert(g).second)
			continue;
		mu2e::DQMStyle::FormatGraph(g, histColor_);
		g->SetMarkerColor(g->GetLineColor());
		g->SetDrawOption("AP");
		if(enableHttpServer_ && httpServer_)
			httpServer_->Register("/", g);
	}
}

void CrvDQM::updateInvalidLatencyRate()
{
	if(!h_invalidLatencyRate_)
		return;
	TH1* invalid = statusDqm_.hists().live("linkLatencyInvalid");
	TH1* blocks  = statusDqm_.hists().live("statusBlocksByLink");
	const bool window = invalid != nullptr && blocks != nullptr;
	if(!window)
	{
		invalid = statusDqm_.linkLatencyInvalid();
		blocks  = statusDqm_.statusBlocksByLink();
	}
	h_invalidLatencyRate_->Reset("ICES");
	h_invalidLatencyRate_->SetTitle(
	    window ? "Invalid link-latency words / status blocks [rolling window];"
	             "DTC#times6 + link ID;Fraction"
	           : "Invalid link-latency words / status blocks [this run];"
	             "DTC#times6 + link ID;Fraction");
	if(invalid != nullptr && blocks != nullptr)
		h_invalidLatencyRate_->Divide(invalid, blocks, 1., 1., "B");
}

void CrvDQM::updateLayout(art::Event const& event)
{
	if(static_cast<int>(event.run()) == layoutRun_)
		return;
	layoutRun_ = static_cast<int>(event.run());
	// A DQM process must not die on a conditions or geometry problem: without a
	// layout the occupancy-per-sector and partner-timing plots stay empty, and
	// everything else keeps filling.
	try
	{
		mu2e::GeomHandle<mu2e::CosmicRayShield> crs;
		const int configuration = mu2e::CRVDQMLayout::configuration(*crs);
		// CRV status conditions are optional here; they only drop notConnected
		// channels from the per-sector occupancy.
		const mu2e::CRVStatus* sipmStatus = nullptr;
		try
		{
			sipmStatus = &sipmStatus_.get(event.id());
		}
		catch(const std::exception& e)
		{
			std::cout << outputPrefix_ << "no CRV status conditions (" << e.what()
			          << "); no channel is treated as disconnected" << std::endl;
		}
		dqm_.SetConfiguration(
		    configuration,
		    mu2e::CRVDQMLayout::channelToSector(*crs, configuration, sipmStatus));

		std::vector<mu2e::CRVDigiDQM::FebTopology> topology;
		std::vector<int>                           channelToLayer;
		mu2e::CRVDQMLayout::febTopology(*crs, channelMap_.get(event.id()), topology,
		                                channelToLayer);
		dqm_.SetFebTopology(topology, channelToLayer);
		std::cout << outputPrefix_ << "run " << layoutRun_ << ": CRV layout from geometry \""
		          << crs->getName() << "\" (configuration "
		          << mu2e::CRVDQMRun1::configurationName(configuration) << ")" << std::endl;
	}
	catch(const std::exception& e)
	{
		if(!warnedLayout_)
		{
			warnedLayout_ = true;
			std::cout << outputPrefix_ << "ERROR: no CRV layout (" << e.what()
			          << "); crvDigisPerChannelAndEvent_* and the partner-FEB timing "
			          << "(dtPartner_*, febNoGroup, groupsPerEvent) stay EMPTY" << std::endl;
		}
	}
}

void CrvDQM::zoomToActivePorts()
{
	const auto& ports = dqm_.activeFebPorts();
	if(ports.empty())
		return;
	// One port of margin, so an edge FEB going quiet is still visible.
	const int lo = std::max(*ports.begin() - 1, 0);
	const int hi = std::min(*ports.rbegin() + 1, mu2e::CRVDQMRun1::kNFebPorts - 1);
	const int nChan = mu2e::CRVDQMRun1::kNChanPerFEB;
	for(TH1* h : dqm_.hists().copies("h1_channels"))
		h->GetXaxis()->SetRangeUser(lo * nChan - 0.5, (hi + 1) * nChan - 0.5);
	for(TH1* h : dqm_.hists().copies("h2_channels"))
		h->GetYaxis()->SetRangeUser(lo - 0.5, hi + 0.5);
	if(TH1* h = jobCopy("dtPartner_SameModuleSameSide"))
		h->GetXaxis()->SetRangeUser(lo - 0.5, hi + 0.5);
}

void CrvDQM::startHttpServer()
{
	// Create HTTP server
	httpServer_ = new THttpServer(Form("http:%d", httpPort_));

	// Create canvas
	webCanvas_ = new TCanvas(canvasName_.c_str(), "CRV DQM");
	if(dummyHist_)
	{
		webCanvas_->Divide(1, 1);
	}
	else
	{
		webCanvas_->Divide(kCanvasCols, kCanvasRows);
	}

	if(dummyHist_)
	{
		webCanvas_->cd(1);
		mu2e::DQMStyle::FormatHist(h1_dummy_, histColor_);
		seedEmptyFrame(h1_dummy_);
		h1_dummy_->Draw("HIST");
	}
	else
	{
		// The two EWT graphs: bespoke axis handling, so they stay explicit. They
		// exist only when the digi table sets liveSeries.
		auto drawGraph = [&](TGraph* g, int pad) {
			if(g == nullptr)
				return;
			webCanvas_->cd(pad);
			mu2e::DQMStyle::FormatGraph(g, histColor_);
			if(TH1F* frame = g->GetHistogram())
			{
				frame->GetXaxis()->SetLimits(0.0, 1.0);
				frame->SetMinimum(0.0);
				frame->SetMaximum(1.0);
			}
			g->Draw("AP");
		};
		drawGraph(digiGraph("g_digisVsEwt"), kPadDigisVsEwt);
		drawGraph(digiGraph("g_digisAvgVsEwt"), kPadDigisAvgVsEwt);

		// Everything else comes off the table. The job copy is what the canvas
		// draws; the segment copies are registered and restyled below but are
		// not given pads of their own.
		for(const auto& spec : kHistPads)
		{
			TH1* h = jobCopy(spec.name);
			if(h == nullptr)
				continue;
			webCanvas_->cd(spec.pad);
			if(spec.logx)
				gPad->SetLogx();
			if(spec.logy)
				gPad->SetLogy();
			if(auto* h2 = dynamic_cast<TH2*>(h))
			{
				// Colour maps need room for the palette and a Z title.
				gPad->SetRightMargin(0.14);
				mu2e::DQMStyle::FormatHist2D(h2);
				h2->GetZaxis()->SetTitle("Entries");
				gStyle->SetPalette(kInvertedDarkBodyRadiator);
			}
			else
			{
				mu2e::DQMStyle::FormatHist(h, histColor_);
				if(spec.yFloor > 0.0)
					h->SetMinimum(spec.yFloor);
			}
			seedEmptyFrame(h);
			h->Draw(spec.drawOpt);

			// One genuine special: the occupancy axis is wide enough that ROOT
			// drops the stat box styling, so re-apply it once the pad is drawn.
			if(std::string(spec.name) == "h1_channels")
			{
				gPad->Update();
				if(auto* st = dynamic_cast<TPaveStats*>(h->FindObject("stats")))
				{
					st->SetBorderSize(0);
					st->SetFillStyle(0);
					st->SetTextFont(42);
					st->SetTextSize(0.040);
					st->SetOptStat(111110);
				}
			}
		}

		// Live invalid-latency rate per link.
		webCanvas_->cd(kPadInvalidLatency);
		mu2e::DQMStyle::FormatHist(h_invalidLatencyRate_.get(), histColor_);
		seedEmptyFrame(h_invalidLatencyRate_.get());
		h_invalidLatencyRate_->Draw("HIST");
	}

	// Register canvas and histograms with server
	httpServer_->Register("/", webCanvas_);
	if(!dummyHist_)
	{
		// Every copy of every displayed histogram, so a configured window or
		// subrun copy is reachable on the server without a change here.
		for(const auto& spec : kHistPads)
		{
			for(TH1* h : dqm_.hists().copies(spec.name))
			{
				httpServer_->Register("/", h);
			}
		}
		for(TGraph* g : dqm_.series().graphs())
			httpServer_->Register("/", g);
		httpServer_->Register("/", h_invalidLatencyRate_.get());
	}

	// Publish refresh period so the HTML page can read it
	httpServer_->CreateItem("/config/refreshMs", Form("%.0f", onlineRefreshPeriodMs_));

	// Setup custom page. The path is configuration, not code: the page is
	// deployed with the online GUI, not shipped in this repo, so a hard-coded
	// package name here goes stale the moment the module moves. $OTS_SOURCE is
	// expanded if present, and a page that is not there is reported rather than
	// handed to SetDefaultPage, which would silently serve nothing.
	if(!httpDefaultPage_.empty())
	{
		std::string page = httpDefaultPage_;
		const std::string token = "$OTS_SOURCE";
		if(const size_t at = page.find(token); at != std::string::npos)
		{
			const char* otsSource = getenv("OTS_SOURCE");
			if(!otsSource)
			{
				std::cout << outputPrefix_
				          << "httpDefaultPage wants $OTS_SOURCE but it is not set; "
				             "serving the built-in THttpServer page"
				          << std::endl;
				page.clear();
			}
			else
				page.replace(at, token.size(), otsSource);
		}

		if(!page.empty() && gSystem->AccessPathName(page.c_str()))
		{
			std::cout << outputPrefix_ << "httpDefaultPage " << page
			          << " does not exist; serving the built-in THttpServer page" << std::endl;
			page.clear();
		}

		if(!page.empty())
			httpServer_->SetDefaultPage(page);
	}

	lastRefreshTime_ = std::chrono::steady_clock::now();

	std::cout << outputPrefix_ << "HTTP server running on http://localhost:" << httpPort_
	          << "/" << std::endl;
}

void CrvDQM::stopHttpServer()
{
	if(httpServer_ != nullptr)
	{
		delete httpServer_;
		httpServer_ = nullptr;
	}

	if(webCanvas_ != nullptr)
	{
		delete webCanvas_;
		webCanvas_ = nullptr;
	}
}

void CrvDQM::updateWebDisplay(bool force)
{
	++statUpdateCalls_;

	if(!enableHttpServer_ || webCanvas_ == nullptr)
	{
		++statUpdateGateA_;
		return;
	}

	auto                                      now     = std::chrono::steady_clock::now();
	std::chrono::duration<double, std::milli> elapsed = now - lastRefreshTime_;

	if(!force && elapsed.count() < onlineRefreshPeriodMs_)
	{
		++statUpdateGateB_;
		return;
	}

	// The live segment copies carry the range they hold in their titles, and
	// the canvas draws those titles; bring them up to date before redrawing.
	dqm_.hists().RefreshLabels();
	statusDqm_.hists().RefreshLabels();

	++statUpdate_;

	if(dummyHist_ && h1_dummy_)
	{
		double maxContent = h1_dummy_->GetBinContent(h1_dummy_->GetMaximumBin());
		h1_dummy_->GetYaxis()->SetRangeUser(0.0, std::max(1.0, 1.15 * maxContent));
	}
	else
	{
		// Every copy of every histogram the table marks autoRange -- so the
		// rolling window copy is rescaled with its parent, whatever it is
		// called and however many older spans are configured.
		for(const auto& spec : kHistPads)
		{
			if(!spec.autoRange)
				continue;
			for(TH1* h : dqm_.hists().copies(spec.name))
			{
				const double maxContent = h->GetBinContent(h->GetMaximumBin());
				h->GetYaxis()->SetRangeUser(spec.yFloor,
				                            std::max(1.0, 1.15 * maxContent));
			}
		}
		zoomToActivePorts();
		updateInvalidLatencyRate();
		h_invalidLatencyRate_->GetYaxis()->SetRangeUser(
		    0.0, std::max(1e-3, 1.15 * h_invalidLatencyRate_->GetMaximum()));
	}

	// Re-apply palette right before update: global TColor state is fragile
	gStyle->SetPalette(kInvertedDarkBodyRadiator);

	// Re-apply per-object formatting that ROOT loses when internal
	// structures are recreated (e.g. TGraph histogram after SetPoint/RemovePoint)
	if(!dummyHist_)
	{
		for(const auto& spec : kHistPads)
		{
			for(TH1* h : dqm_.hists().copies(spec.name))
			{
				if(auto* h2 = dynamic_cast<TH2*>(h))
					mu2e::DQMStyle::FormatHist2D(h2);
				else
					mu2e::DQMStyle::FormatHist(h, histColor_);
			}
		}

		TGraph* g_digisVsEwt    = digiGraph("g_digisVsEwt");
		TGraph* g_digisAvgVsEwt = digiGraph("g_digisAvgVsEwt");

		// Auto-range both hits-graphs' Y axes from current data.
		auto autoRangeGraphY = [](TGraph* g) {
			if(!g || g->GetN() <= 0)
				return;
			double* y   = g->GetY();
			int     n   = g->GetN();
			double  yLo = *std::min_element(y, y + n);
			double  yHi = *std::max_element(y, y + n);
			if(yHi <= yLo)
				yHi = yLo + 1.0;
			double margin = 0.1 * (yHi - yLo);
			g->SetMinimum(std::max(0.0, yLo - margin));
			g->SetMaximum(yHi + margin);
			if(TH1F* frame = g->GetHistogram())
			{
				frame->SetMinimum(std::max(0.0, yLo - margin));
				frame->SetMaximum(yHi + margin);
			}
		};
		if(g_digisVsEwt != nullptr && dqm_.hasEwtWindow())
		{
			mu2e::DQMStyle::FormatGraph(g_digisVsEwt, histColor_);
			autoRangeGraphY(g_digisVsEwt);
			// autoRangeGraphY may make TGraph::GetHistogram() recreate the frame,
			// whose X limits then default to the data range; keep the sliding window.
			double currentEwt = static_cast<double>(dqm_.lastEwt());
			double xLo = std::max(0.0, currentEwt - kEwtXRange);
			double xHi = currentEwt;
			if(xHi <= xLo)
				xHi = xLo + 1.0;
			if(TH1F* frame = g_digisVsEwt->GetHistogram())
				frame->GetXaxis()->SetLimits(xLo, xHi);
		}
		if(g_digisAvgVsEwt != nullptr && g_digisAvgVsEwt->GetN() > 0)
		{
			autoRangeGraphY(g_digisAvgVsEwt);
			// Span the points the averaged graph currently holds.
			double* ax   = g_digisAvgVsEwt->GetX();
			int     nAvg = g_digisAvgVsEwt->GetN();
			double  aLo  = *std::min_element(ax, ax + nAvg);
			double  aHi  = *std::max_element(ax, ax + nAvg);
			if(aHi <= aLo)
				aHi = aLo + 1.0;
			if(TH1F* frame = g_digisAvgVsEwt->GetHistogram())
				frame->GetXaxis()->SetLimits(aLo, aHi);
		}
	}

	if(eventCounts_ > 0)
	{
		for(int i = 1; i <= webCanvas_->GetListOfPrimitives()->GetSize(); ++i)
		{
			webCanvas_->cd(i);
			gPad->Modified();
		}
	}
	webCanvas_->cd();
	webCanvas_->Modified();
	webCanvas_->Update();

	gSystem->ProcessEvents();
	++statProcEvents_;
	lastRefreshTime_ = now;
}

void CrvDQM::analyze(art::Event const& event)
{
	++statAnalyze_;

	art::EventID eventID = event.id();

	if(diagLevel_ > 1)
	{
		std::cout << outputPrefix_ << "=================== " << eventID
		          << " ===================" << std::endl;
	}

	if(dummyHist_)
	{
		// Fill dummy histogram only
		double randomValue = random_.Gaus(0, 25);
		h1_dummy_->Fill(randomValue);

		if(diagLevel_ > 2)
		{
			std::cout << outputPrefix_
			          << "Filled dummy histogram with value: " << randomValue
			          << std::endl;
		}
	}
	else
	{
		art::Handle<mu2e::CrvDigiCollection> crvDigisHandle;
		event.getByLabel(crvDigiTag_, crvDigisHandle);

		const mu2e::CrvDigiCollection emptyDigis;
		const mu2e::CrvDigiCollection& crvDigis =
		    (crvDigisHandle.isValid() && crvDigisHandle.product() != nullptr)
		        ? *crvDigisHandle
		        : emptyDigis;

		if(!crvDigisHandle.isValid() || crvDigis.empty())
		{
			if(diagLevel_ > 1)
			{
				std::cout << outputPrefix_ << "Warning! No CRV digis found" << std::endl;
			}
		}
		else if(diagLevel_ > 1)
		{
			std::cout << outputPrefix_ << "Found " << crvDigis.size() << std::endl;
		}

		art::Handle<mu2e::CrvStatusCollection> crvStatusHandle;
		event.getByLabel(crvStatusTag_, crvStatusHandle);
		const mu2e::CrvStatusCollection emptyStatus;
		const mu2e::CrvStatusCollection& crvStatus =
		    (crvStatusHandle.isValid() && crvStatusHandle.product() != nullptr)
		        ? *crvStatusHandle
		        : emptyStatus;

		updateLayout(event);
		dqm_.Fill(crvDigis, crvStatus);
		statusDqm_.Fill(crvStatus);
		registerNewStatusObjects();
	}

	///////////////////// Send /////////////////////

	// Send histograms in fixed time intervals
	auto                             currentTime = std::chrono::steady_clock::now();
	std::chrono::duration<double> elapsed = currentTime - lastSendTime_;

	if(elapsed.count() >= sendIntervalSec_)
	{
		Send();
		// Update last send time
		lastSendTime_ = currentTime;
	}

	updateWebDisplay();

	// Update event counter
	++eventCounts_;

	// Periodic rate stats
	std::chrono::duration<double> statElapsed = currentTime - statLastLog_;
	if(statElapsed.count() >= statLogPeriodSec_)
	{
		double dt = statElapsed.count();
		std::cout << outputPrefix_ << "Rates (last " << dt << " s): "
		          << "analyze=" << (statAnalyze_ / dt) << " Hz, "
		          << "updateWebDisplay=" << (statUpdate_ / dt) << " Hz"
		          << " (calls=" << statUpdateCalls_ << ", gateA=" << statUpdateGateA_
		          << ", gateB=" << statUpdateGateB_ << "), "
		          << "gSystem->ProcessEvents=" << (statProcEvents_ / dt) << " Hz, "
		          << "sendHistograms=" << (statSend_ / dt) << " Hz" << std::endl;
		statAnalyze_       = 0;
		statUpdate_        = 0;
		statUpdateCalls_   = 0;
		statUpdateGateA_   = 0;
		statUpdateGateB_   = 0;
		statProcEvents_    = 0;
		statSend_          = 0;
		statLastLog_       = currentTime;
	}
}

void CrvDQM::endJob()
{
	if(diagLevel_ > 0)
	{
		// Print job-level statistics
		std::cout << outputPrefix_
		          << "================= End job summary =================" << std::endl;
		std::cout << outputPrefix_ << "Total events: "
		          << (dummyHist_ ? eventCounts_ : dqm_.nEvents()) << std::endl;
		if(!dummyHist_)
		{
			std::cout << outputPrefix_ << "Total digis: " << dqm_.nDigis() << std::endl;
			std::cout << outputPrefix_ << "Active FEB ports: " << dqm_.activeFebPorts().size()
			          << std::endl;
			// Print FEBs per ROC
			for(auto& [roc, febs] : dqm_.rocFEBMap())
			{
				std::cout << outputPrefix_ << "ROC " << (int)roc << " has " << febs.size()
				          << " FEBs: ";
				for(auto feb : febs)
				{
					std::cout << (int)feb << " ";
				}
				std::cout << std::endl;
			}
			for(TGraph* g : statusDqm_.series().graphs())
			{
				std::cout << outputPrefix_ << g->GetName() << ": " << g->GetN()
				          << " points recorded" << std::endl;
			}
		}
		std::cout << outputPrefix_
		          << "===============================================" << std::endl;
	}

	if(diagLevel_ > 1)
	{
		std::cout << outputPrefix_ << "Ending job" << std::endl;
	}

	if(!dummyHist_)
	{
		dqm_.EndJob();
		statusDqm_.EndJob();
	}

	// Send final histograms & clean up
	if(sendHists_ && histoSender_ != nullptr)
	{
		Send();
		histoSender_.reset();
	}

	std::vector<TCanvas*> canvasesForPdf;
	if(enableHttpServer_ && webCanvas_ != nullptr)
	{
		canvasesForPdf.push_back(webCanvas_);
	}

	if(enableHttpServer_)
	{
		updateWebDisplay(true);
	}

	// Summary canvases: one per FEB port with hits, showing its FPGA-pair dt
	// columns of dtFpgaPairs as histograms.
	if(!dummyHist_ && dqm_.dtFpgaPairs() != nullptr)
	{
		using mu2e::CRVDQMRun1::fpgaPairIndex;
		using mu2e::CRVDQMRun1::kNFpgaPairs;
		TH2F* pairs = dqm_.dtFpgaPairs();
		art::TFileDirectory canvasDir =
		    tfs_->mkdir(outputTag_).mkdir("timing_feb_canvases");

		for(int port = 0; port < mu2e::CRVDQMRun1::kNFebPorts; ++port)
		{
			const int firstCol = port * kNFpgaPairs + 1;
			if(pairs->Integral(firstCol, firstCol + kNFpgaPairs - 1, 0, pairs->GetNbinsY() + 1) <= 0.)
				continue;
			const int   roc    = port / mu2e::CRVDQMRun1::kNFebPerROC + 1;
			const int   feb    = port % mu2e::CRVDQMRun1::kNFebPerROC + 1;
			std::string cName  = Form("c_timing_port%03d", port);
			std::string cTitle = Form("FPGA timing, FEB port %d (ROC %d FEB %d)", port, roc, feb);
			TCanvas*    c =
			    canvasDir.make<TCanvas>(cName.c_str(), cTitle.c_str(), 1200, 1200);
			TDirectory* saveDir = gDirectory;
			c->Divide(4, 4);

			for(int fpgaA = 0; fpgaA < 4; ++fpgaA)
			{
				for(int fpgaB = fpgaA; fpgaB < 4; ++fpgaB)
				{
					if(!showSameFpgaTimingInCanvas_ && fpgaA == fpgaB)
						continue;
					const int col = port * kNFpgaPairs + fpgaPairIndex(fpgaA, fpgaB) + 1;
					TH1* slice = pairs->ProjectionY(
					    Form("dt_port%03d_fpga%d_fpga%d", port, fpgaA, fpgaB), col, col);
					slice->SetDirectory(nullptr);
					slice->SetTitle(Form("#Deltat FEB port %d FPGA %d - FPGA %d;#Deltat [ns];Entries",
					                     port, fpgaA, fpgaB));
					timingSlices_.emplace_back(slice);
					c->cd(fpgaA * 4 + fpgaB + 1);
					slice->Draw("HIST");
				}
			}
			c->Update();
			saveDir->cd();
			c->Write();
			canvasesForPdf.push_back(c);
		}
	}

	if(diagLevel_ > 0)
	{
		logPublished();
	}

	if(saveCanvasesToPdf_)
	{
		if(canvasesForPdf.empty())
		{
			std::cout << outputPrefix_
			          << "No canvases available for PDF export (requested file: "
			          << canvasPdfFile_ << ")" << std::endl;
		}
		else
		{
			canvasesForPdf.front()->Print((canvasPdfFile_ + "[").c_str());
			for(TCanvas* c : canvasesForPdf)
			{
				if(c == nullptr)
					continue;
				c->Modified();
				c->Update();
				c->Print(canvasPdfFile_.c_str());
			}
			canvasesForPdf.back()->Print((canvasPdfFile_ + "]").c_str());
			std::cout << outputPrefix_ << "Saved " << canvasesForPdf.size()
			          << " canvases to PDF: " << canvasPdfFile_ << std::endl;
		}
	}

	if(enableHttpServer_)
	{
		stopHttpServer();
	}
}

DEFINE_ART_MODULE(ots::CrvDQM)
}  // namespace ots
