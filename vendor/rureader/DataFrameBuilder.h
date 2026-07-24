#ifndef DATAFRAMEBUILDER_H_INCLUDED
#define DATAFRAMEBUILDER_H_INCLUDED

#include <map>
#include <string>
#include <utility>

class DataFrameBuilder {

public:

  virtual ~DataFrameBuilder( ) {}

  virtual void ProduceFlags( )      = 0;
  virtual void ProduceConfigs( )    = 0;
  virtual void ProduceFormats( )    = 0;
  virtual void ProduceNumSamples( ) = 0;

  virtual int                                        GetNumSamples( ) = 0;
  virtual std::map<std::string, int>&                GetFlags( )      = 0;
  virtual std::map<std::string, std::pair<int,int>>& GetConfigs( )    = 0;
  virtual std::map<std::string, std::pair<int,int>>& GetFormat( )     = 0;

};

#endif // DATAFRAMEBUILDER_H_INCLUDED
