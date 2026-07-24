#pragma once
//
// IDigitizer — hardware abstraction for one digitizer board.
//
// The rest of the DAQ (reader thread, file writer, and later the parallel
// decoder) talks only to this interface, so the exact same pipeline runs
// against real CAEN hardware (CaenDigitizer) or a synthetic source
// (MockDigitizer) for hardware-free testing.
//
// Lifecycle mirrors the proven CAEN sequence:
//     open()  ->  configure()  ->  start()  ->  read() * N  ->  stop()  ->  close()
//
// Contract: NO method throws. Failures are reported via the bool return value
// and logged. This is central to the "never crash" requirement.
//
#include <cstddef>
#include <cstdint>
#include <string>

#include "caendaq/BoardInfo.hpp"

namespace caendaq {

struct BoardParams {
    std::string   name       = "board"; // logical name, embedded in output filenames
    int           connType   = 0;        // CAEN connection type (0=USB, 1=Optical, 5=A4818, ...)
    int           linkNum    = 0;
    int           node       = 0;
    std::uint32_t vmeBase    = 0;        // VME base address (0 for USB/desktop)
    std::string   configPath;            // JSON register-config file (empty = skip config)
};

class IDigitizer {
public:
    virtual ~IDigitizer() = default;

    // Open the connection and read board info. False on failure.
    virtual bool open() = 0;

    // Apply the register configuration and allocate the readout buffer.
    virtual bool configure() = 0;

    // Clear board memory and start acquisition.
    virtual bool start() = 0;

    // Read one readout buffer. On success returns true and sets *data / *size to
    // a buffer owned by the digitizer, valid until the next read()/stop()/close().
    // *size may be 0 when no data is currently available (this is NOT an error).
    // Returns false on a communication error (caller should attempt recovery).
    virtual bool read(const char** data, std::size_t* size) = 0;

    // Stop acquisition (leaves the board open/configured).
    virtual bool stop() = 0;

    // Close the connection and release resources. Idempotent.
    virtual void close() = 0;

    // Board identity/geometry, valid after a successful configure() (before
    // that it may be partially populated). Embedded in the .caendat header.
    virtual BoardInfo info() const = 0;

    virtual const std::string& name() const = 0;
    virtual bool connected() const = 0;
};

} // namespace caendaq
