#ifndef _ots_FEHistoMakerInterface_h_
#define _ots_FEHistoMakerInterface_h_

#include "otsdaq/FECore/FEVInterface.h"
#include "otsdaq/NetworkUtilities/TCPPublishServer.h"

#include <random>
#include <string>

namespace ots
{

class FEHistoMakerInterface : public FEVInterface, public TCPPublishServer
{
  public:
  public:
	FEHistoMakerInterface(const std::string&       interfaceUID,
	                      const ConfigurationTree& theXDAQContextConfigTree,
	                      const std::string&       configurationPath);
	virtual ~FEHistoMakerInterface(void);

	void configure(void);
	void halt(void);
	void pause(void);
	void resume(void);
	void start(std::string runNumber) override;
	void stop(void);

	bool running(void);

	void universalRead(char* address, char* readValue) override { ; }
	void universalWrite(char* address, char* writeValue) override { ; }

  private:
	std::default_random_engine       generator_;
	std::normal_distribution<double> distribution_;
};

}  // namespace ots

#endif
