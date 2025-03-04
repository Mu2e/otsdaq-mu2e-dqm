// Author: G. Pezzullo
// This module produces histograms of data from the TriggerResults

#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art/Framework/Services/System/TriggerNamesService.h"
#include "art_root_io/TFileService.h"
#include "canvas/Persistency/Common/TriggerResults.h"
#include "fhiclcpp/types/OptionalAtom.h"

#include "TRACE/tracemf.h"

#include <TBufferFile.h>
#include <TF1.h>
#include <TFile.h>
#include <TGraph.h>
#include <TGraphErrors.h>
#include <TH1F.h>
#include <TSpline.h>
#include <TVirtualFitter.h>
#include <map>
#include <vector>

#include "otsdaq-mu2e-dqm/ArtModules/CaloIERCDQMHistoContainer.h"
#include "otsdaq-mu2e/ArtModules/HistoSender.hh"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/Macros/ProcessorPluginMacros.h"
#include "otsdaq/MessageFacility/MessageFacility.h"
#include "otsdaq/NetworkUtilities/TCPSendClient.h"

#include "Offline/RecoDataProducts/inc/CaloDigi.hh"

namespace ots
{
class CaloIERCDQM : public art::EDAnalyzer
{
  public:
	struct Config
	{
		using Name    = fhicl::Name;
		using Comment = fhicl::Comment;
		fhicl::Atom<int> port{
		    Name("port"),
		    Comment("This parameter sets the port where the histogram will be sent")};
		fhicl::Atom<std::string> address{
		    Name("address"),
		    Comment("This paramter sets the IP address where the "
		            "histogram will be sent")};
		fhicl::Atom<std::string> moduleTag{Name("moduleTag"), Comment("Module tag name")};
		fhicl::Atom<std::string> caloDigiTag{Name("caloDigiTag"),
		                                     Comment("Module tag name")};
		fhicl::Sequence<std::string> histType{
		    Name("histType"),
		    Comment("This parameter determines which quantity is histogrammed")};
		fhicl::Atom<int> freqDQM{
		    Name("freqDQM"),
		    Comment("Frequency for sending histograms to the data-receiver")};
		fhicl::Atom<int>         diag{Name("diagLevel"), Comment("Diagnostic level"), 0};
		fhicl::Atom<std::string> spline{
		    Name("splineFilename"), Comment("spline root file path"), ""};
		fhicl::Atom<bool> uset0{
		    Name("uset0"), Comment("Use t0 instead of fitting with templates"), false};
		fhicl::Atom<int> skipAfterN{
		    Name("skipAfterN"), Comment("Don't fit after N hits"), -1};
	};

	typedef art::EDAnalyzer::Table<Config> Parameters;

	explicit CaloIERCDQM(Parameters const& conf);

	void analyze(art::Event const& event) override;
	void beginRun(art::Run const&) override;
	void beginJob() override;
	void endJob() override;

	void summary_fill(CaloIERCDQMHistoContainer*      histos,
	                  const mu2e::CaloDigiCollection* caloDigis);
	void PlotRate(art::Event const& e);

  private:
	Config                                conf_;
	int                                   port_;
	std::string                           address_;
	std::string                           moduleTag_;
	std::string                           caloDigiTag_;
	std::vector<std::string>              histType_;
	int                                   freqDQM_, diagLevel_, evtCounter_;
	art::ServiceHandle<art::TFileService> tfs;
	CaloIERCDQMHistoContainer* histo_container = new CaloIERCDQMHistoContainer();
	HistoSender*               histSender_;
	bool                       doOnspillHist_, doOffspillHist_;
	std::string                splineFilename_;
	bool                       uset0_;
	int                        skipAfterN_;

	int       this_eventNumber;
	TSpline3* spline;
	TF1*      f_spline;
};
}  // namespace ots

ots::CaloIERCDQM::CaloIERCDQM(Parameters const& conf)
    : art::EDAnalyzer(conf)
    , conf_(conf())
    , port_(conf().port())
    , address_(conf().address())
    , moduleTag_(conf().moduleTag())
    , caloDigiTag_(conf().caloDigiTag())
    , histType_(conf().histType())
    , freqDQM_(conf().freqDQM())
    , diagLevel_(conf().diag())
    , evtCounter_(0)
    , doOnspillHist_(false)
    , doOffspillHist_(false)
    , splineFilename_(conf().spline())
    , uset0_(conf().uset0())
    , skipAfterN_(conf().skipAfterN())
{
	histSender_ = new HistoSender(address_, port_);

	if(diagLevel_ > 0)
	{
		__COUT__ << "[CaloIERCDQM::analyze] DQM for " << histType_[0] << std::endl;
	}

	for(std::string name : histType_)
	{
		if(name == "Onspill")
		{
			doOnspillHist_ = true;
		}
		if(name == "Offspill")
		{
			doOffspillHist_ = true;
		}
	}

	TFile* filetemp = TFile::Open(splineFilename_.c_str());
	if(!filetemp->IsOpen())
	{
		std::cout << splineFilename_ << " NOT FOUND" << std::endl;
	}

	spline = (TSpline3*)filetemp->Get("spline_0_1");
	if(!spline)
	{
		std::cout << "No template found in spline file.." << std::endl;
	}

	f_spline = new TF1(
	    "f_spline",
	    [this](double* x, double* par) {
		    return par[0] * this->spline->Eval(x[0] - par[1]) + par[2];
	    },
	    3.,
	    25.,
	    3);
	f_spline->SetParNames("scale", "tpeak", "ped");
	f_spline->SetRange(spline->GetXmin(), spline->GetXmax());
	f_spline->SetNpx(10000);

	TVirtualFitter::SetDefaultFitter("Minuit");
}

void ots::CaloIERCDQM::beginJob()
{
	__COUT__ << "[CaloIERCDQM::beginJob] Beginning job" << std::endl;
}

void ots::CaloIERCDQM::analyze(art::Event const& event)
{
	++evtCounter_;

	art::EventNumber_t eventNumber = event.event();
	this_eventNumber               = (int)eventNumber;
	TLOG(TLVL_DEBUG) << "Analyzing event " << this_eventNumber;

	art::Handle<mu2e::CaloDigiCollection> caloDigisHandle;
	if(!event.getByLabel(caloDigiTag_, caloDigisHandle))
	{
		__COUT__ << "[CaloIERCDQM::" << __func__ << "] No calo digis found with tag "
		         << caloDigiTag_.c_str() << std::endl;
		return;
	}
	else if(diagLevel_ > 0)
	{
		__COUT__ << "[CaloIERCDQM::" << __func__
		         << "] Calo digi collection handle found\n";
	}

	auto caloDigis = caloDigisHandle.product();
	if(!caloDigis)
	{
		__COUT__ << "[CaloIERCDQM::" << __func__ << "] No calo digis found with tag "
		         << caloDigiTag_.c_str() << std::endl;
		return;
	}
	else if(diagLevel_ > 0)
	{
		__COUT__ << "[CaloIERCDQM::" << __func__ << "] Calo digi collection found\n";
	}

	if(!caloDigis)
	{
		TLOG(TLVL_DEBUG) << "CaloDigi pointer is null";
	}
	else
	{
		TLOG(TLVL_DEBUG) << "CaloDigi pointer has size " << caloDigis->size();
	}
	TLOG(TLVL_DEBUG) << "Filling calo DQM plots for event " << this_eventNumber;
	summary_fill(histo_container, caloDigis);

	if(evtCounter_ % freqDQM_ != 0)
		return;

	// send a packet AND reset the histograms
	std::map<std::string, std::vector<TH1*>>    hists_to_send;
	std::map<std::string, std::vector<TGraph*>> graphs_to_send;

	// send the summary hists
	for(auto hist_singlechan_pair : histo_container->map_h1_dt_singlechan)
	{
		__COUT__ << "[CaloIERCDQM::analyze] collecting single chan histogram "
		         << hist_singlechan_pair.second._Hist << std::endl;
		hists_to_send[moduleTag_ + "/singleChan:replace"].push_back(
		    (TH1*)hist_singlechan_pair.second._Hist->Clone());
	}
	for(auto hist_singlechan_pairg : histo_container->map_g_dtevt_singlechan)
	{
		__COUT__ << "[CaloIERCDQM::analyze] collecting single chan graph "
		         << hist_singlechan_pairg.second._Graph << std::endl;
		graphs_to_send[moduleTag_ + "/singleChan"].push_back(
		    (TGraph*)hist_singlechan_pairg.second._Graph->Clone());
	}
	for(auto hist_sameboard_pair : histo_container->map_h1_dt_sameboard)
	{
		__COUT__ << "[CaloIERCDQM::analyze] collecting same board histogram "
		         << hist_sameboard_pair.second._Hist << std::endl;
		hists_to_send[moduleTag_ + "/sameBoard:replace"].push_back(
		    (TH1*)hist_sameboard_pair.second._Hist->Clone());
	}
	for(auto hist_sameboard_pairg : histo_container->map_g_dtevt_sameboard)
	{
		__COUT__ << "[CaloIERCDQM::analyze] collecting same board graph "
		         << hist_sameboard_pairg.second._Graph << std::endl;
		graphs_to_send[moduleTag_ + "/sameBoard"].push_back(
		    (TGraph*)hist_sameboard_pairg.second._Graph->Clone());
	}
	for(auto hist_diffboard_pair : histo_container->map_h1_dt_diffboard)
	{
		__COUT__ << "[CaloIERCDQM::analyze] collecting diff board histogram "
		         << hist_diffboard_pair.second._Hist << std::endl;
		hists_to_send[moduleTag_ + "/diffBoard:replace"].push_back(
		    (TH1*)hist_diffboard_pair.second._Hist->Clone());
	}
	for(auto hist_diffboard_pairg : histo_container->map_g_dtevt_diffboard)
	{
		__COUT__ << "[CaloIERCDQM::analyze] collecting diff board graph "
		         << hist_diffboard_pairg.second._Graph << std::endl;
		graphs_to_send[moduleTag_ + "/diffBoard"].push_back(
		    (TGraph*)hist_diffboard_pairg.second._Graph->Clone());
	}

	histSender_->sendHistograms(hists_to_send);
	histSender_->sendGraphs(graphs_to_send);
}

void ots::CaloIERCDQM::summary_fill(CaloIERCDQMHistoContainer*      histos,
                                    const mu2e::CaloDigiCollection* caloDigis)
{
	std::map<int, std::vector<int>>   digiMap;
	std::map<int, std::vector<float>> timeMap;

	TLOG(TLVL_DEBUG) << "There are " << caloDigis->size() << " calo digis";

	// Fill time-ordered map for each sipm
	digiMap.clear();
	for(uint ihit = 0; ihit < caloDigis->size(); ihit++)
	{
		int   thisID   = caloDigis->at(ihit).SiPMID();
		float thisTime = caloDigis->at(ihit).t0();
		// Check if this hit is not the last, if so insert
		bool sorted = true;
		for(uint storedHit = 0; storedHit < digiMap[thisID].size(); storedHit++)
		{
			auto storedDigi = caloDigis->at(digiMap[thisID][storedHit]);
			if(thisTime < storedDigi.t0())
			{
				std::cout << "Found unsorted hit! (in position " << storedHit << " of "
				          << digiMap[thisID].size() << ") ";
				std::cout << "Current t0: " << thisTime
				          << " , last t0: " << storedDigi.t0() << "\n";
				digiMap[thisID].insert(digiMap[thisID].begin() + storedHit, ihit);
				sorted = false;
				break;
			}
		}
		if(sorted)
		{  // this triggers for the first one and when all is sorted
			digiMap[thisID].push_back(ihit);
		}
	}

	// Now, fit the templates and fill a timeMap
	timeMap.clear();
	if(uset0_)
	{  // Use t0 instead of fitting
		for(auto pair : digiMap)
		{
			for(int idx : pair.second)
			{
				timeMap[pair.first].push_back(5. * caloDigis->at(idx).t0());
			}
		}
	}
	else
	{  // Do template fitting
		TGraphErrors* gadc = new TGraphErrors();
		for(auto pair : digiMap)
		{
			TLOG(TLVL_DEBUG) << "Fitting simpid " << pair.first;
			for(uint ihit = 0; ihit < pair.second.size(); ihit++)
			{
				if(skipAfterN_ > 0 && int(ihit) >= skipAfterN_)
					continue;                  // stop fitting if surpassed N hits
				uint idx = pair.second[ihit];  // idx is the index in the original
				                               // caloDigis vector
				gadc->Set(0);
				auto this_waveform = caloDigis->at(idx).waveform();
				for(uint gi = 0; gi < this_waveform.size(); gi++)
				{  // Fill the tgraph to be fit
					gadc->SetPoint(gi, gi, this_waveform[gi]);
					gadc->SetPointError(gi, 0., 1.);
				}
				f_spline->SetParameters(3850, 0, 0);
				int fitStatus = gadc->Fit("f_spline", "QRN");  // FIT!
				if(fitStatus >= 0)
				{
					timeMap[pair.first].push_back(
					    5. * (caloDigis->at(idx).t0() + f_spline->GetParameter(1)));
				}
				else
				{  // if bad fit, don't fill at all
					std::cout << "Bad fit status: " << fitStatus << " for sipmid "
					          << pair.first << " hit " << idx << "\n";
				}
			}
		}
		delete gadc;
	}

	//--------Now we can fill hists

	// Same channel
	for(auto pair : timeMap)
	{
		int thisID = pair.first;
		if(pair.second.size() > 1)
		{  // there must be at least 2 hits
			if(histo_container->map_h1_dt_singlechan.find(thisID) ==
			   histo_container->map_h1_dt_singlechan.end())
			{  // This hist doesn't exist yet
				TString hname = Form("h1_dt_schan_%d", thisID);
				TString htitle =
				    Form("[SAME CHANNEL] dt between hit 0 and hit 1 of sipm %d", thisID);
				histo_container->BookSingleChannel(
				    tfs, thisID, hname.Data(), htitle.Data(), 600, 10009, 10011);
				// map_h1_dt_singlechan[thisID] =
				// tfs->make<TH1F>(hname,htitle,600,10009,10011);
				TString gname = Form("g_dtevt_schan_%d", thisID);
				TString gtitle =
				    Form("[SAME CHANNEL] dt between hit 0 and hit 1 of sipm %d", thisID);
				histo_container->BookSingleChannelG(
				    tfs, thisID, gname.Data(), gtitle.Data());
				// map_g_dtevt_singlechan[thisID] =
				// tfs->makeAndRegister<TGraph>(gname,gtitle);
			}
			float dt = timeMap[thisID][1] - timeMap[thisID][0];
			histo_container->map_h1_dt_singlechan[thisID]._Hist->Fill(dt);
			histo_container->map_g_dtevt_singlechan[thisID]._Graph->AddPoint(
			    this_eventNumber, dt);
		}
	}

	// Same board
	int previous_DTCROC = -1;
	for(auto pair : timeMap)
	{
		int thisID     = pair.first;
		int thisDTCROC = thisID / 20;
		int thisDTC    = thisDTCROC / 6;
		int thisROC    = thisDTCROC % 6;
		int thisChan   = thisID % 20;
		if(thisDTCROC == previous_DTCROC)
			continue;  // Skip if we are on the same board as previous

		previous_DTCROC = thisDTCROC;
		for(auto nextpair : timeMap)
		{
			int nextID     = nextpair.first;
			int nextDTCROC = nextID / 20;
			int nextChan   = nextID % 20;
			if(thisID == nextID)
				continue;
			if(thisDTCROC == nextDTCROC)
			{  // We found a different channel in the same board
				int idpair = thisID * 10000 + nextID;
				if(histo_container->map_h1_dt_sameboard.find(idpair) ==
				   histo_container->map_h1_dt_sameboard.end())
				{  // This hist doesn't exist yet
					TString hname  = Form("map_h1_dt_sameboard_%d_%d", nextID, thisID);
					TString htitle = Form(
					    "[SAME BOARD] dt between chan %d and %d (DTC: "
					    "%d, Board: %d, hit 0)",
					    nextChan,
					    thisChan,
					    thisDTC,
					    thisROC);
					histo_container->BookSameBoard(
					    tfs, idpair, hname.Data(), htitle.Data(), 5000, -50, 50);
					// map_h1_dt_sameboard[idpair] =
					// tfs->make<TH1F>(hname,htitle,5000,-50,50);
					TString gname  = Form("map_g_dtevt_sameboard_%d_%d", nextID, thisID);
					TString gtitle = Form(
					    "[SAME BOARD] dt between chan %d and %d (DTC: "
					    "%d, Board: %d, hit 0)",
					    nextChan,
					    thisChan,
					    thisDTC,
					    thisROC);
					histo_container->BookSameBoardG(
					    tfs, idpair, gname.Data(), gtitle.Data());
					// map_g_dtevt_sameboard[idpair] =
					// tfs->makeAndRegister<TGraph>(gname,gtitle);
				}
				float dt = timeMap[nextID][0] - timeMap[thisID][0];
				if(abs(dt) > 5000)
					continue;  // They are not the same hit, discard
				std::cout << "Same board, filling evt " << this_eventNumber
				          << ", thisID: " << thisID << ", nextID: " << nextID
				          << ", dt = " << dt << "\n";
				histo_container->map_h1_dt_sameboard[idpair]._Hist->Fill(dt);
				histo_container->map_g_dtevt_sameboard[idpair]._Graph->AddPoint(
				    this_eventNumber, dt);
			}
		}
	}

	// Different board, same chan
	// std::cout<<std::endl;
	for(int chan = 0; chan < 20; chan++)
	{
		int refID = -1;
		for(auto pair : timeMap)
		{
			int thisID     = pair.first;
			int thisDTCROC = thisID / 20;
			int thisDTC    = thisDTCROC / 6;
			int thisROC    = thisDTCROC % 6;
			int thisChan   = thisID % 20;
			// if (chan==1){std::cout<<"thisChan: "<<thisChan<<", thisID:
			// "<<thisID<<", refID: "<<refID<<"\n";}
			if(thisChan == chan)
			{  // Found the channel we want to plot
				if(refID < 0)
				{  // It's the first one, don't do anything
					// if (chan==1) {std::cout<<"refID<0, thisID: "<<thisID<<"\n";}
					refID = thisID;
				}
				else
				{
					// if (chan==1) {std::cout<<"filling event "<<this_eventNumber<<",
					// refID: "<<refID<<", thisID: "<<thisID<<"
					// -------------------------------------\n";}
					int refDTCROC = refID / 20;
					int refDTC    = refDTCROC / 6;
					int refROC    = refDTCROC % 6;
					int idpair    = refID * 10000 + thisID;
					if(histo_container->map_h1_dt_diffboard.find(idpair) ==
					   histo_container->map_h1_dt_diffboard.end())
					{  // This hist doesn't exist yet
						TString hname = Form(
						    "map_h1_dt_diffboard_chan%d_%d_%d", thisChan, thisID, refID);
						TString htitle = Form(
						    "[DIFFERENT BOARD] dt between (DTC: %d, Board: %d) and "
						    "(DTC: %d, Board: %d) [chan %d, hit 0]",
						    thisDTC,
						    thisROC,
						    refDTC,
						    refROC,
						    thisChan);
						histo_container->BookDiffBoard(
						    tfs, idpair, hname.Data(), htitle.Data(), 20000, -200, 200);
						// map_h1_dt_diffboard[idpair] =
						// tfs->make<TH1F>(hname,htitle,20000,-200,200);
						TString gname  = Form("map_g_dtevt_diffboard_chan%d_%d_%d",
                                             thisChan,
                                             thisID,
                                             refID);
						TString gtitle = Form(
						    "[DIFFERENT BOARD] dt between (DTC: %d, Board: %d) and "
						    "(DTC: %d, Board: %d) [chan %d, hit 0]",
						    thisDTC,
						    thisROC,
						    refDTC,
						    refROC,
						    thisChan);
						histo_container->BookDiffBoardG(
						    tfs, idpair, gname.Data(), gtitle.Data());
						// map_g_dtevt_diffboard[idpair] =
						// tfs->makeAndRegister<TGraph>(gname,gtitle);
					}
					float dt = timeMap[thisID][0] - timeMap[refID][0];
					if(abs(dt) > 5000)
						continue;  // They are not the same hit, discard
					histo_container->map_h1_dt_diffboard[idpair]._Hist->Fill(dt);
					histo_container->map_g_dtevt_diffboard[idpair]._Graph->AddPoint(
					    this_eventNumber, dt);
				}
			}
		}
	}
}

void ots::CaloIERCDQM::endJob() {}

void ots::CaloIERCDQM::beginRun(const art::Run& run) {}

DEFINE_ART_MODULE(ots::CaloIERCDQM)
