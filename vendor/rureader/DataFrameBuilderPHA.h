#ifndef DATAFRAMEBUILDERPHA_H_INCLUDED
#define DATAFRAMEBUILDERPHA_H_INCLUDED

#include <map>
#include <string>
#include <utility>

#include "DataFrameBuilder.h"

class DataFrameBuilderPHA : public DataFrameBuilder {

public:

  DataFrameBuilderPHA( std::string dgtzName );
  ~DataFrameBuilderPHA( );

  void ProduceNumSamples( ) override;
  void ProduceFlags( )      override;
  void ProduceConfigs( )    override;
  void ProduceFormats( )    override;

  int                                        GetNumSamples( ) override { return fNumSamples; }
  std::map<std::string, int>&                GetFlags( )      override { return fFlags;      }
  std::map<std::string, std::pair<int,int>>& GetConfigs( )    override { return fConfigs;    }
  std::map<std::string, std::pair<int,int>>& GetFormat( )     override { return fFormats;    }

private:

  std::string fDgtzName;

  int fNumSamples = 0;
  std::map<std::string, int>                fFlags;
  std::map<std::string, std::pair<int,int>> fConfigs;
  std::map<std::string, std::pair<int,int>> fFormats;

};

#endif // DATAFRAMEBUILDERPHA_H_INCLUDED
