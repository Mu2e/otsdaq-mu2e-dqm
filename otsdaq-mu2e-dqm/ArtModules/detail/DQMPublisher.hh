#ifndef OTSDAQ_DQM_ARTMODULES_DETAIL_DQMPUBLISHER_HH
#define OTSDAQ_DQM_ARTMODULES_DETAIL_DQMPUBLISHER_HH
// Ships what DQMHelpers clients (Offline/DQMHelpers) publish to the otsdaq
// visualizer through HistoSender. A client's FHiCL rules choose the copies and
// their groups; this sends each group as "<prefix>/<group>:replace" and the
// clients' live graphs as "<prefix>/graphs:replace", at most once per interval.
//
//   ots::DQMPublisher publisher_{ps, "trk"};   // reads address, port, sendHists, sendIntervalSec
//   beginJob: publisher_.add(dqm_); publisher_.connect();
//   analyze:  if(publisher_.due()) publisher_.send();
//   endJob:   publisher_.send(); publisher_.logPublished(); publisher_.disconnect();
//
// Booking, filling and the run/subrun lifecycle stay with the clients.

#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <TGraph.h>
#include <TH1.h>

#include "fhiclcpp/ParameterSet.h"

#include "Offline/DQMHelpers/inc/DQMClient.hh"
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"

namespace ots
{

class DQMPublisher
{
  public:
	// Histograms a module sends besides its clients' (display-only objects),
	// keyed by the name they are sent under, without prefix or ":replace".
	using Extra = std::map<std::string, std::vector<TH1*>>;

	DQMPublisher(fhicl::ParameterSet const& ps, std::string prefix)
	    : prefix_(std::move(prefix))
	    , address_(ps.get<std::string>("address", "localhost"))
	    , port_(ps.get<int>("port", 6000))
	    , enabled_(ps.get<bool>("sendHists", true))
	    , intervalSec_(ps.get<double>("sendIntervalSec", 0.5))
	    , logPrefix_("[" + prefix_ + " DQMPublisher] ")
	{
	}

	void add(mu2e::DQMClient& client) { clients_.push_back(&client); }

	// A DQM process that cannot reach the visualizer keeps filling and writing
	// its file; it only stops sending.
	void connect()
	{
		if(!enabled_)
			return;
		try
		{
			sender_ = std::make_unique<HistoSender>(address_, port_);
			std::cout << logPrefix_ << "connected HistoSender to " << address_ << ":"
			          << port_ << std::endl;
		}
		catch(const std::exception& e)
		{
			std::cout << logPrefix_ << "failed to initialise HistoSender: " << e.what()
			          << "; sending disabled" << std::endl;
			enabled_ = false;
		}
		lastSend_ = std::chrono::steady_clock::now();
	}

	void disconnect() { sender_.reset(); }

	bool enabled() const { return enabled_ && sender_ != nullptr; }
	std::size_t nSends() const { return nSends_; }

	// True when sending is on and sendIntervalSec has passed since the last send.
	bool due() const
	{
		if(!enabled())
			return false;
		const std::chrono::duration<double> elapsed =
		    std::chrono::steady_clock::now() - lastSend_;
		return elapsed.count() >= intervalSec_;
	}

	void send(const Extra& extra = {})
	{
		lastSend_ = std::chrono::steady_clock::now();
		if(!enabled())
			return;
		// Live copies are titled with the range they hold; refresh before shipping.
		for(mu2e::DQMClient* client : clients_)
			client->hists().RefreshLabels();

		std::map<std::string, std::vector<TH1*>> hists;
		for(const auto& [group, copies] : published())
			hists[folder(group)] = copies;
		for(const auto& [name, objs] : extra)
		{
			auto& out = hists[folder(name)];
			out.insert(out.end(), objs.begin(), objs.end());
		}
		if(!hists.empty())
			sender_->sendHistograms(hists);

		std::map<std::string, std::vector<TGraph*>> graphs;
		auto&                                       out = graphs[folder("graphs")];
		for(const mu2e::DQMClient* client : clients_)
			out.insert(out.end(),
			           client->series().graphs().begin(),
			           client->series().graphs().end());
		if(!out.empty())
			sender_->sendGraphs(graphs);
		++nSends_;
	}

	// What the FHiCL rules selected, printed once so an operator can see whether
	// a change did what they meant. Valid after disconnect().
	void logPublished(const Extra& extra = {}) const
	{
		const auto groups = published();
		std::cout << logPrefix_ << "publishing " << groups.size() + extra.size()
		          << " HistoSender group(s)" << (enabled_ ? "" : " (sending disabled)") << ", "
		          << nSends_ << " send(s):" << std::endl;
		for(const auto& [group, copies] : groups)
		{
			std::cout << logPrefix_ << "  " << folder(group) << "  (" << copies.size() << ")";
			if(copies.size() <= 8)  // naming every member of a large group helps nobody
				for(const TH1* h : copies)
					std::cout << " " << h->GetName();
			std::cout << std::endl;
		}
		for(const auto& [name, objs] : extra)
			std::cout << logPrefix_ << "  " << folder(name) << "  (" << objs.size() << ")"
			          << std::endl;
	}

  private:
	std::string folder(const std::string& group) const
	{
		return prefix_ + "/" + group + ":replace";
	}

	// All clients' published copies, merged by group.
	std::map<std::string, std::vector<TH1*>> published() const
	{
		std::map<std::string, std::vector<TH1*>> all;
		for(const mu2e::DQMClient* client : clients_)
			for(const auto& [group, copies] : client->hists().publishedCopies())
			{
				auto& out = all[group];
				out.insert(out.end(), copies.begin(), copies.end());
			}
		return all;
	}

	std::string                                        prefix_;
	std::string                                        address_;
	int                                                port_;
	bool                                               enabled_;
	double                                             intervalSec_;
	std::string                                        logPrefix_;
	std::vector<mu2e::DQMClient*>                      clients_;
	std::unique_ptr<HistoSender>                       sender_;
	std::chrono::time_point<std::chrono::steady_clock> lastSend_{
	    std::chrono::steady_clock::now()};
	std::size_t nSends_{0};
};

}  // namespace ots

#endif /* OTSDAQ_DQM_ARTMODULES_DETAIL_DQMPUBLISHER_HH */
