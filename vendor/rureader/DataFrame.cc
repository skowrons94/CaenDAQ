#include "DataFrame.h"
#include "Utils.h"

DataFrame::DataFrame( CAEN_DGTZ_DPPFirmware_t dppType, std::string dgtzName )
{
  switch( dppType ){
    case CAEN_DGTZ_DPPFirmware_PHA:
      fBuilder = std::make_shared<DataFrameBuilderPHA>( dgtzName );
      break;
    case CAEN_DGTZ_DPPFirmware_PSD:
      fBuilder = std::make_shared<DataFrameBuilderPSD>( dgtzName );
      break;
    default:
      break;
  }

  fDgtzName = dgtzName;
}

void DataFrame::Build( )
{
  if( !fBuilder ) return;

  fBuilder->ProduceFlags( );
  fBuilder->ProduceConfigs( );
  fBuilder->ProduceFormats( );

  fFlags      = fBuilder->GetFlags( );
  fConfigs    = fBuilder->GetConfigs( );
  fFormats    = fBuilder->GetFormat( );
  fNumSamples = fBuilder->GetNumSamples( );

  fSizeFormat = Format( "Size", BitRange( 0, 31 ) );
}

bool DataFrame::Enabled( const std::string& key ) const
{
  auto it = fFlags.find( key );
  if( it == fFlags.end( ) ) return false;
  return ( fDataFormat >> it->second ) & 0x1u;
}

bool DataFrame::HasConfig( const std::string& key ) const
{
  return fConfigs.find( key ) != fConfigs.end( );
}

uint16_t DataFrame::Config( const std::string& key ) const
{
  auto it = fConfigs.find( key );
  if( it == fConfigs.end( ) ) return 0;

  const uint32_t value = ExtractBits( fDataFormat, it->second );

  // The number of samples is stored in units of fNumSamples samples.
  if( key == "NumSamples" ) return static_cast<uint16_t>( value * fNumSamples );

  return static_cast<uint16_t>( value );
}

bool DataFrame::HasFormat( const std::string& key ) const
{
  return fFormats.find( key ) != fFormats.end( );
}

BitRange DataFrame::Format( const std::string& key, BitRange fallback ) const
{
  auto it = fFormats.find( key );
  if( it == fFormats.end( ) ) return fallback;
  return it->second;
}

bool DataFrame::SetDataFormat( uint32_t form )
{
  if( fFormatSet && form == fDataFormat ) return false;

  fDataFormat = form;
  fFormatSet  = true;
  ResolveLayout( );

  return true;
}

void DataFrame::ResolveLayout( )
{
  DataLayout l;

  l.dualTrace     = Enabled( "DT" );
  l.hasTrace      = Enabled( "Trace" );
  l.hasChannelBit = HasFormat( "CH" );
  l.numSamples    = Config( "NumSamples" );

  // Boards of the x724/x781/x782 family do not ship the extra word even when
  // the corresponding bit of the format word is set.
  const bool extrasWordAvailable = fDgtzName.find( "724" ) == std::string::npos &&
                                   fDgtzName.find( "781" ) == std::string::npos &&
                                   fDgtzName.find( "782" ) == std::string::npos;

  l.hasExtras    = Enabled( "Extras" ) && extrasWordAvailable;
  l.extrasConfig = HasConfig( "Extras" );
  l.extrasMode   = l.extrasConfig ? static_cast<uint8_t>( Config( "Extras" ) ) : 0;

  l.fmtTS     = Format( "TS"    , BitRange(  0, 30 ) );
  l.fmtExtras = Format( "Extras", BitRange(  0, 31 ) );
  l.fmtSample = Format( "Sample", BitRange(  0, 13 ) );

  // PHA calls the two digital probes "DP" and "TR", PSD calls them "DP1" and
  // "DP2". Accept both spellings, otherwise the PHA probes silently decoded
  // bit 0 of the sample word.
  l.fmtDP1 = HasFormat( "DP1" ) ? Format( "DP1", BitRange( 14, 14 ) )
                                : Format( "DP" , BitRange( 14, 14 ) );
  l.fmtDP2 = HasFormat( "DP2" ) ? Format( "DP2", BitRange( 15, 15 ) )
                                : Format( "TR" , BitRange( 15, 15 ) );

  l.tsBits = RangeWidth( l.fmtTS );

  // Event size, in 32 bit words.
  uint16_t size = 0;
  if( Enabled( "Energy" ) ) size += 1;
  if( Enabled( "Charge" ) ) size += 1;
  if( Enabled( "TS"     ) ) size += 1;
  if( l.hasExtras         ) size += 1;
  if( l.hasTrace          ) size += l.numSamples / 2;
  l.evtSize = size;

  fLayout = l;
}
