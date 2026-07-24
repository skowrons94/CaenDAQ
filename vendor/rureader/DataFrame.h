#ifndef DATAFRAME_H_INCLUDED
#define DATAFRAME_H_INCLUDED

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include <CAENDigitizerType.h>

#include "Utils.h"
#include "DataFrameBuilderPHA.h"
#include "DataFrameBuilderPSD.h"

// Everything the unpackers need in order to walk through one channel aggregate.
// It is derived from the data format word and is therefore recomputed only when
// a board actually sends a different format word.
struct DataLayout {

  uint16_t evtSize    = 0;      // event size, in 32 bit words
  uint16_t numSamples = 0;      // total number of samples of the trace

  bool dualTrace      = false;  // two analog traces interleaved in one word pair
  bool hasTrace       = false;
  bool hasExtras      = false;  // the extra word is present in the event
  bool hasChannelBit  = false;  // odd/even channel of the couple is in the TS word
  bool extrasConfig   = false;  // the extras format is selected by the format word
  uint8_t extrasMode  = 0;      // meaning of the extra word (0,1,2,4,5,7)

  BitRange fmtTS     { 0, 30 };
  BitRange fmtExtras { 0, 31 };
  BitRange fmtSample { 0, 13 };
  BitRange fmtDP1    { 14, 14 };
  BitRange fmtDP2    { 15, 15 };

  int tsBits = 31;              // width of the time stamp field, cached
};

class DataFrame {

public:

  DataFrame( ) = default;
  DataFrame( CAEN_DGTZ_DPPFirmware_t dppType, std::string dgtzName );

  void Build( );

  // False when the board name is not one of the families the builders know
  // about: in that case every map is empty and nothing can be decoded.
  bool IsSupported( ) const { return fBuilder && !fFlags.empty( ) && !fFormats.empty( ); }

  const std::string& Name( ) const { return fDgtzName; }

  // The channel aggregate size sits in the first word of the channel aggregate,
  // i.e. before the data format word, so it is available without SetDataFormat.
  const BitRange& SizeFormat( ) const { return fSizeFormat; }

  // Returns true if the format word differed from the previous one and the
  // layout had to be recomputed.
  bool SetDataFormat( uint32_t form );

  bool FormatSet( ) const { return fFormatSet; }
  const DataLayout& Layout( ) const { return fLayout; }

private:

  void ResolveLayout( );

  bool     Enabled  ( const std::string& key ) const;  // bit set in the data format word
  bool     HasConfig( const std::string& key ) const;
  uint16_t Config   ( const std::string& key ) const;  // field of the data format word
  bool     HasFormat( const std::string& key ) const;
  BitRange Format   ( const std::string& key, BitRange fallback ) const;

  // Shared, so that copying a DataFrame around cannot leave a dangling builder
  // behind. The old code leaked the raw pointer on purpose to avoid exactly that.
  std::shared_ptr<DataFrameBuilder> fBuilder;

  int fNumSamples = 0;
  std::map<std::string, int>      fFlags;
  std::map<std::string, BitRange> fConfigs;
  std::map<std::string, BitRange> fFormats;

  uint32_t fDataFormat = 0;
  bool     fFormatSet  = false;

  BitRange fSizeFormat { 0, 31 };

  std::string fDgtzName;

  DataLayout fLayout;

};

#endif // DATAFRAME_H_INCLUDED
