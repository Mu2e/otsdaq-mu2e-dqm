// Per-straw hit counts from reconstructed hits. No event or hit selection.
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Offline/DataProducts/inc/StrawId.hh"
#include "Offline/RecoDataProducts/inc/StrawHit.hh"
#include "TH1D.h"
#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Run.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art_root_io/TFileService.h"
#include "canvas/Utilities/InputTag.h"
#include "cetlib_except/exception.h"
#include "fhiclcpp/types/Atom.h"
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"

namespace ots
{
class TrackerHitOccupancy : public art::EDAnalyzer
{
  public:
	struct Config
	{
		fhicl::Atom<art::InputTag> hits{fhicl::Name("hits"),
		                                art::InputTag("TrigComMakeSH")};
		fhicl::Atom<bool>          publish{fhicl::Name("publish"), false};
		fhicl::Atom<std::string>   address{fhicl::Name("address"), ""};
		fhicl::Atom<int>           port{fhicl::Name("port"), 0};
		fhicl::Atom<unsigned>      publishEvery{fhicl::Name("publishEvery"), 100};
		fhicl::Atom<std::string>   directory{fhicl::Name("directory"),
                                           "TrackerHitOccupancy"};
	};
	using Parameters = art::EDAnalyzer::Table<Config>;
	explicit TrackerHitOccupancy(Parameters const& p)
	    : art::EDAnalyzer(p)
	    , token_(consumes<mu2e::StrawHitCollection>(p().hits()))
	    , publish_(p().publish())
	    , address_(p().address())
	    , port_(p().port())
	    , every_(p().publishEvery())
	    , directory_(p().directory())
	{
		if(every_ == 0 || directory_.empty() ||
		   (publish_ && (address_.empty() || port_ < 1 || port_ > 65535)))
			throw cet::exception("Configuration")
			    << "Invalid TrackerHitOccupancy publishing settings";
	}

  private:
	static constexpr unsigned nPanels = mu2e::StrawId::_nplanes * mu2e::StrawId::_npanels;
	art::ProductToken<mu2e::StrawHitCollection> token_;
	bool                                        publish_;
	std::string                                 address_;
	int                                         port_;
	unsigned                                    every_;
	std::string                                 directory_, runDirectory_;
	std::uint64_t                               pendingEvents_ = 0;
	std::array<TH1D*, nPanels>                  totals_{};
	std::array<std::unique_ptr<TH1D>, nPanels>  deltas_{};
	TH1D*                                       events_ = nullptr;
	std::unique_ptr<TH1D>                       eventDelta_;
	std::unique_ptr<HistoSender>                sender_;

	void beginRun(art::Run const& run) override
	{
		pendingEvents_ = 0;
		runDirectory_  = directory_ + "/run_" + std::to_string(run.run());
		art::ServiceHandle<art::TFileService> fs;
		auto runDir = fs->mkdir("run_" + std::to_string(run.run()));
		events_     = runDir.make<TH1D>(
            "Events",
            "Monitored events (including zero-hit events);Counter;Events",
            1,
            0.,
            1.);
		eventDelta_.reset(static_cast<TH1D*>(events_->Clone()));
		eventDelta_->SetDirectory(nullptr);
		for(unsigned plane = 0; plane < mu2e::StrawId::_nplanes; ++plane)
		{
			auto dir = runDir.mkdir("plane_" + std::to_string(plane));
			for(unsigned panel = 0; panel < mu2e::StrawId::_npanels; ++panel)
			{
				auto i = plane * mu2e::StrawId::_npanels + panel;
				auto name =
				    "Panel_" + std::to_string(plane) + "_" + std::to_string(panel);
				totals_[i] = dir.make<TH1D>(
				    name.c_str(),
				    (name + ";Straw number;Hits in monitored events").c_str(),
				    mu2e::StrawId::_nstraws,
				    -0.5,
				    mu2e::StrawId::_nstraws - 0.5);
				deltas_[i].reset(static_cast<TH1D*>(totals_[i]->Clone()));
				deltas_[i]->SetDirectory(nullptr);
			}
		}
		if(publish_ && !sender_)
			sender_ = std::make_unique<HistoSender>(address_, port_);
	}

	void analyze(art::Event const& event) override
	{
		auto hits = event.getValidHandle(token_);
		// Validate before filling, so malformed IDs cannot partially count an event.
		for(auto const& hit : *hits)
		{
			auto sid = hit.strawId();
			if(!mu2e::StrawId::validPlane(sid.plane()) ||
			   !mu2e::StrawId::validPanel(sid.panel()) ||
			   !mu2e::StrawId::validStraw(sid.straw()))
				throw cet::exception("TrackerHitOccupancy")
				    << "Invalid StrawId in event " << event.id();
		}
		for(auto const& hit : *hits)
		{
			auto sid = hit.strawId();
			totals_[sid.uniquePanel()]->Fill(sid.straw());
			deltas_[sid.uniquePanel()]->Fill(sid.straw());
		}
		events_->Fill(0.5);
		eventDelta_->Fill(0.5);
		++pendingEvents_;
		if(publish_ && pendingEvents_ >= every_)
			flush();
	}

	void flush()
	{
		if(!publish_ || pendingEvents_ == 0)
			return;
		// The deployed receiver adds histograms. Send increments, never cumulative
		// ROOT-file totals, to avoid counting earlier events again at every publish.
		std::map<std::string, std::vector<TH1*>> packet;
		packet[runDirectory_].push_back(eventDelta_.get());
		for(unsigned i = 0; i < nPanels; ++i)
			packet[runDirectory_ + "/plane_" +
			       std::to_string(i / mu2e::StrawId::_npanels)]
			    .push_back(deltas_[i].get());
		sender_->sendHistograms(
		    packet);  // Serializes synchronously; does not own the histograms.
		for(auto& h : deltas_)
			h->Reset();
		eventDelta_->Reset();
		pendingEvents_ = 0;
	}
	void endRun(art::Run const&) override { flush(); }
	void endJob() override { flush(); }
};
}  // namespace ots
DEFINE_ART_MODULE(ots::TrackerHitOccupancy)
