#include "otsdaq-mu2e-dqm/FEInterfaces/FEHistoMakerInterface.h"
#include "otsdaq/Macros/CoutMacros.h"
#include "otsdaq/Macros/InterfacePluginMacros.h"
#include "otsdaq/MessageFacility/MessageFacility.h"

#include <chrono>
#include <thread>

#include <iostream>

using namespace ots;

//========================================================================================================================
FEHistoMakerInterface::FEHistoMakerInterface(
    const std::string&       interfaceUID,
    const ConfigurationTree& theXDAQContextConfigTree,
    const std::string&       configurationPath)
    : FEVInterface(interfaceUID, theXDAQContextConfigTree, configurationPath)
    , TCPPublishServer(theXDAQContextConfigTree.getNode(configurationPath)
                           .getNode("ServerPort")
                           .getValue<unsigned int>(),
                       1)
    , distribution_(50, 10)
{
	std::cout << "[In FEHistoMakerInterface () ] Initiating ..." << std::endl;
	TCPPublishServer::startAccept();
}

//========================================================================================================================
FEHistoMakerInterface::~FEHistoMakerInterface(void) {}

//========================================================================================================================
void FEHistoMakerInterface::configure(void)
{
	std::cout << __PRETTY_FUNCTION__ << "ConfigureDone!" << std::endl;
}

//========================================================================================================================
void FEHistoMakerInterface::halt(void)
{
	std::cout << "[In FEHistoMakerInterface () ] Halting ..." << std::endl;
}

//========================================================================================================================
void FEHistoMakerInterface::pause(void)
{
	std::cout << "[In FEHistoMakerInterface () ] Pausing ..." << std::endl;
}

//========================================================================================================================
void FEHistoMakerInterface::resume(void)
{
	std::cout << "[In FEHistoMakerInterface () ] Resuming ..." << std::endl;
}

//========================================================================================================================
void FEHistoMakerInterface::start(std::string runNumber)
{
	std::cout << "[In FEHistoMakerInterface () ] Starting ..." << std::endl;
}

//========================================================================================================================
// The running state is a thread
bool FEHistoMakerInterface::running(void)
{
	std::cout << "[In FEHistoMakerInterface () ] Running ..." << std::endl;
	double      value = distribution_(generator_);
	std::string buffer(8, '0');
	memcpy(&buffer.at(0), &value, sizeof(double));
	std::cout << __PRETTY_FUNCTION__ << " Value: " << value << " Buffer: " << buffer
	          << std::endl;

	TCPPublishServer::broadcast(buffer);
	std::this_thread::sleep_for(std::chrono::milliseconds(5000));
	return WorkLoop::continueWorkLoop_;  // otherwise it stops!!!!!
}

//========================================================================================================================
void FEHistoMakerInterface::stop(void)
{
	std::cout << "[In FEHistoMakerInterface () ] Stoping ..." << std::endl;
}

DEFINE_OTS_INTERFACE(FEHistoMakerInterface)
