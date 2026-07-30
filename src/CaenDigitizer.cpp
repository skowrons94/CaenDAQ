#include "caendaq/CaenDigitizer.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <json/json.h>

#include "caendaq/Log.hpp"
#include "class_caen_dgtz.h" // vendored driver (pulls in CAENDigitizer.h)

namespace caendaq {

namespace {

// ── Registers used for multi-board synchronisation ──────────────────────────
// (UM5678 DPP-PHA rev.3 / UM4380 DPP-PSD rev.6, x725 & x730 register manuals)
constexpr std::uint32_t kRegBoardConfiguration = 0x8000;
constexpr std::uint32_t kRegAcquisitionControl = 0x8100; // [1:0] start mode, [2] start/arm
constexpr std::uint32_t kRegGlobalTriggerMask  = 0x810C;
constexpr std::uint32_t kRegTrgOutEnableMask   = 0x8110;
constexpr std::uint32_t kRegFrontPanelIOCtrl   = 0x811C; // [17:16] TRG-OUT/GPO mode
constexpr std::uint32_t kRegRunDelay           = 0x8170; // start-propagation compensation

// Human-readable Start/Stop Mode, for log messages.
const char* startModeName(std::uint32_t mode) {
    switch (mode) {
        case kStartModeSW:        return "SW controlled";
        case kStartModeSInGpi:    return "S-IN/GPI controlled";
        case kStartModeFirstTrig: return "first trigger controlled";
        case kStartModeLVDS:      return "LVDS controlled";
        default:                  return "unknown";
    }
}

// Map the CAEN firmware enum to our stable, ABI-independent DppType.
DppType mapDpp(CAEN_DGTZ_DPPFirmware_t fw) {
    switch (fw) {
        case CAEN_DGTZ_DPPFirmware_PHA: return DppType::PHA;
        case CAEN_DGTZ_DPPFirmware_PSD: return DppType::PSD;
        case CAEN_DGTZ_DPPFirmware_CI:  return DppType::CI;
        case CAEN_DGTZ_DPPFirmware_ZLE: return DppType::ZLE;
        case CAEN_DGTZ_DPPFirmware_QDC: return DppType::QDC;
        case CAEN_DGTZ_DPPFirmware_DAW: return DppType::DAW;
        default:                        return DppType::Std; // NotDPPFirmware = standard waveform
    }
}
} // namespace

CaenDigitizer::CaenDigitizer(BoardParams params) : params_(std::move(params)) {
    info_.connType = params_.connType;
    info_.linkNum  = params_.linkNum;
    info_.node     = params_.node;
    info_.vmeBase  = params_.vmeBase;
}

CaenDigitizer::~CaenDigitizer() { close(); }

bool CaenDigitizer::connected() const {
    return dgtz_ && dgtz_->IsActive(/*silent=*/1) == 1;
}

bool CaenDigitizer::activate() {
    if (!dgtz_) {
        dgtz_ = std::make_unique<CAENDgtz>();
        dgtz_->SetExternalErrorCode(&errcode_);
        dgtz_->SetExternalErrorDesc(&errdesc_);
    }
    errcode_ = 0;
    dgtz_->Activate(params_.connType, params_.linkNum, params_.node, params_.vmeBase);
    if (errcode_ != 0 || dgtz_->IsActive(1) != 1) {
        LOG_ERROR(params_.name << ": OpenDigitizer failed (" << dgtz_->GetErrorDesc() << ")");
        return false;
    }
    refreshBoardInfo();
    LOG_INFO(params_.name << ": opened " << info_.modelName
             << " (code " << info_.model << ", " << info_.channels << " ch, "
             << info_.adcNBits << "-bit, serial " << info_.serialNumber << ")");
    return true;
}

void CaenDigitizer::refreshBoardInfo() {
    if (!dgtz_) return;
    CAEN_DGTZ_BoardInfo_t bi = dgtz_->GetInfo();
    info_.modelName    = bi.ModelName;
    info_.familyCode   = bi.FamilyCode;
    info_.channels     = dgtz_->GetNumChannels();
    info_.adcNBits     = bi.ADC_NBits;
    info_.serialNumber = bi.SerialNumber;
    info_.rocFirmware  = bi.ROC_FirmwareRel;
    info_.amcFirmware  = bi.AMC_FirmwareRel;
    info_.model        = static_cast<std::uint32_t>(dgtz_->GetModel());
    info_.dppType      = mapDpp(dgtz_->GetDPPVersion());
    info_.nsPerSample  = dgtz_->GetNsPerSample();
    info_.nsPerTimetag = dgtz_->GetNsPerTimetag();
    info_.boardRegId   = dgtz_->GetBoardRegID();
    info_.channelEnableMask = dgtz_->GetChannelEnableMask();
    info_.formFactor   = bi.FormFactor;
    info_.pcbRevision  = bi.PCB_Revision;
#ifdef MAX_LICENSE_LENGTH
    // DPP firmware licence string (CAENDigitizer >= 2.6). Recorded so the run
    // metadata pins the exact firmware the data was taken with.
    info_.license.assign(bi.License, strnlen(bi.License, MAX_LICENSE_LENGTH));
#endif

    // Read back the acquisition-relevant registers so the metadata reflects
    // what the board is ACTUALLY programmed with, not just what the config file
    // asked for. Failures are non-fatal: leave the field at 0 and carry on.
    const auto readReg = [this](std::uint32_t addr) -> std::uint32_t {
        errcode_ = 0;
        const std::uint32_t v = dgtz_->ReadRegister(addr, "refreshBoardInfo");
        if (errcode_ != 0) { errcode_ = 0; return 0; }
        return v;
    };
    info_.acquisitionControl  = readReg(kRegAcquisitionControl);
    info_.boardConfiguration  = readReg(kRegBoardConfiguration);
    info_.frontPanelIOControl = readReg(kRegFrontPanelIOCtrl);
    info_.globalTriggerMask   = readReg(kRegGlobalTriggerMask);
    info_.trgOutEnableMask    = readReg(kRegTrgOutEnableMask);
    info_.runDelay            = readReg(kRegRunDelay);
}

std::uint32_t CaenDigitizer::startMode() const {
    // The board's own configuration owns this — we only read it back to decide
    // how the board has to be started. Cached from the last refreshBoardInfo().
    return info_.acquisitionControl & 0x3u;
}

bool CaenDigitizer::open() {
    return activate();
}

bool CaenDigitizer::applyRegisterDump(const std::string& path) {
    std::ifstream f(path, std::ifstream::in);
    if (!f.good()) return false;

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(f, root, false)) return false;
    if (!root.isMember("registers")) return false; // not this format

    const Json::Value& regs = root["registers"];
    int written = 0, failed = 0;
    for (const std::string& key : regs.getMemberNames()) {
        const Json::Value& r = regs[key];
        if (!r.isMember("address") || !r.isMember("value")) continue;
        const std::uint32_t addr = static_cast<std::uint32_t>(
            std::strtoul(r["address"].asString().c_str(), nullptr, 16));
        const std::uint32_t val = static_cast<std::uint32_t>(
            std::strtoul(r["value"].asString().c_str(), nullptr, 16));
        errcode_ = 0;
        dgtz_->WriteRegister(addr, val, "applyRegisterDump");
        if (errcode_ != 0) { ++failed; errcode_ = 0; } else { ++written; }
    }
    LOG_INFO(params_.name << ": applied register dump from " << path
             << " (" << written << " ok, " << failed << " failed)");
    return true; // handled the "registers" format (individual failures tolerated)
}

bool CaenDigitizer::applyConfig() {
    if (!dgtz_) return false;

    if (!params_.configPath.empty()) {
        // WebDAQ stores per-board config as a flat register dump
        //   {"registers": { "reg_XXXX": {"address":"0x..","value":"0x.."}, ... }}
        // Prefer that; otherwise fall back to the XDAQ high-level "dgtzs" format.
        if (!applyRegisterDump(params_.configPath)) {
            errcode_ = 0;
            dgtz_->ConfigureFromFile(params_.configPath.c_str());
            if (errcode_ == -60 /* CONFIG_FILE_NOT_FOUND */) {
                LOG_ERROR(params_.name << ": config file not found: " << params_.configPath);
                return false;
            }
            if (errcode_ != 0) {
                LOG_WARN(params_.name << ": configuration reported issues ("
                         << errdesc_ << ") — continuing");
            }
        }
    } else {
        LOG_WARN(params_.name << ": no config file given; using board defaults");
    }

    // Aggregate organisation is left to the board's own register config file
    // (Aggregate Organization 0x800C, Events per Aggregate 0x8034/0xEF1C).
    // Calling SetDPPEventAggregation(0, 0) here would hand the choice back to
    // the CAEN library and overwrite what the register dump programmed, so it
    // stays disabled:
    //     dgtz_->SetDPPEventAggregation(0, 0);   // for DPP-PHA/PSD/CI

    // (Re)allocate the readout buffer AFTER programming the board — its size
    // depends on the configuration.
    if (readoutBuffer_) {
        dgtz_->FreeReadoutBuffer(&readoutBuffer_);
        readoutBuffer_ = nullptr;
        bufferSize_ = 0;
    }
    errcode_ = 0;
    dgtz_->MallocReadoutBuffer(&readoutBuffer_, &bufferSize_);
    if (errcode_ != 0 || readoutBuffer_ == nullptr) {
        LOG_ERROR(params_.name << ": MallocReadoutBuffer failed");
        return false;
    }
    LOG_INFO(params_.name << ": readout buffer allocated (" << bufferSize_ << " bytes)");
    return true;
}

bool CaenDigitizer::configure() {
    if (!applyConfig()) return false;
    refreshBoardInfo(); // pick up the configured channel-enable mask
    configured_ = true;
    return true;
}

bool CaenDigitizer::start() {
    if (!dgtz_) return false;
    errcode_ = 0;
    dgtz_->ClearData();
    dgtz_->SWStartAcquisition();
    if (errcode_ != 0) {
        LOG_ERROR(params_.name << ": SWStartAcquisition failed");
        return false;
    }
    running_ = true;
    return true;
}

bool CaenDigitizer::arm() {
    if (!dgtz_) return false;
    // Same register bit as a software start (Acquisition Control bit[2]) — but
    // because this board's start mode is not "SW controlled", setting it ARMS
    // the board instead of running it. Acquisition begins when the external
    // start (TRG-IN / S-IN / LVDS) arrives.
    errcode_ = 0;
    dgtz_->ClearData();
    dgtz_->SetRegisterSpecificBits(kRegAcquisitionControl, 2, 2, 1, "arm");
    if (errcode_ != 0) {
        LOG_ERROR(params_.name << ": could not arm the acquisition (0x8100[2])");
        return false;
    }
    running_ = true;
    LOG_INFO(params_.name << ": armed (start mode "
             << startModeName(startMode()) << ") — waiting for the external start");
    return true;
}

bool CaenDigitizer::sendSWTrigger() {
    if (!dgtz_) return false;
    errcode_ = 0;
    dgtz_->SendSWTrigger();
    if (errcode_ != 0) {
        LOG_ERROR(params_.name << ": SendSWTrigger failed");
        return false;
    }
    LOG_INFO(params_.name << ": software trigger sent");
    return true;
}

bool CaenDigitizer::reconnect() {
    LOG_WARN(params_.name << ": attempting reconnect...");
    if (dgtz_) {
        if (readoutBuffer_) { dgtz_->FreeReadoutBuffer(&readoutBuffer_); readoutBuffer_ = nullptr; }
        dgtz_->Deactivate();       // best-effort; ignore errors
    }
    if (!activate())   return false;
    if (!applyConfig()) return false;
    refreshBoardInfo();
    if (running_) {
        // A synchronised board cannot restart itself — its start comes from the
        // chain. Re-arm it so it picks up the next start signal, and say so:
        // until then this board contributes no data.
        if (synchronised()) {
            running_ = false;          // arm() sets it again on success
            if (!arm()) { LOG_ERROR(params_.name << ": re-arm after reconnect failed"); return false; }
            LOG_WARN(params_.name << ": re-armed after reconnect — it will only resume "
                     "on the next start signal from the chain");
        } else {
            errcode_ = 0;
            dgtz_->ClearData();
            dgtz_->SWStartAcquisition();
            if (errcode_ != 0) { LOG_ERROR(params_.name << ": restart after reconnect failed"); return false; }
        }
    }
    LOG_INFO(params_.name << ": reconnected");
    return true;
}

bool CaenDigitizer::read(const char** data, std::size_t* size) {
    *data = nullptr;
    *size = 0;
    if (!dgtz_ || readoutBuffer_ == nullptr) return false;

    // Held for the whole cycle so an online register write cannot land in the
    // middle of a ReadData on the same handle. Uncontended cost is negligible
    // next to the readout itself.
    std::lock_guard<std::mutex> guard(ioMutex_);

    if (!connected()) {
        if (!reconnect()) return false; // still down: signal runner to back off
        // reconnected — fall through and read this cycle
    }

    errcode_ = 0;
    bufferSize_ = 0;
    dgtz_->ReadData(CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT, readoutBuffer_, &bufferSize_);
    if (errcode_ != 0) {
        // Communication error — the driver drops isActive; recover next cycle.
        LOG_ERROR(params_.name << ": ReadData error (" << errdesc_ << ")");
        return false;
    }
    *data = readoutBuffer_;
    *size = bufferSize_; // may legitimately be 0 when no data is pending
    return true;
}

bool CaenDigitizer::writeRegister(std::uint32_t address, std::uint32_t value) {
    if (!dgtz_) return false;

    std::lock_guard<std::mutex> guard(ioMutex_);
    if (!connected()) {
        LOG_WARN(params_.name << ": register write to 0x" << std::hex << address
                 << std::dec << " skipped — board not connected");
        return false;
    }

    errcode_ = 0;
    dgtz_->WriteRegister(address, value, "writeRegister");
    if (errcode_ != 0) {
        LOG_ERROR(params_.name << ": write of 0x" << std::hex << value << " to 0x"
                  << address << std::dec << " failed (" << errdesc_ << ")");
        errcode_ = 0;
        return false;
    }
    LOG_INFO(params_.name << ": wrote 0x" << std::hex << value << " to register 0x"
             << address << std::dec);
    return true;
}

bool CaenDigitizer::readRegister(std::uint32_t address, std::uint32_t* value) {
    if (!dgtz_ || value == nullptr) return false;

    std::lock_guard<std::mutex> guard(ioMutex_);
    if (!connected()) return false;

    errcode_ = 0;
    const std::uint32_t v = dgtz_->ReadRegister(address, "readRegister");
    if (errcode_ != 0) {
        errcode_ = 0;
        return false;
    }
    *value = v;
    return true;
}

bool CaenDigitizer::stop() {
    if (!dgtz_) return false;
    running_ = false;
    errcode_ = 0;
    dgtz_->SWStopAcquisition();
    if (errcode_ != 0) {
        LOG_WARN(params_.name << ": SWStopAcquisition reported an error");
        return false;
    }
    return true;
}

void CaenDigitizer::close() {
    if (!dgtz_) return;
    if (running_) stop();
    if (readoutBuffer_) {
        dgtz_->FreeReadoutBuffer(&readoutBuffer_);
        readoutBuffer_ = nullptr;
        bufferSize_ = 0;
    }
    dgtz_->Deactivate();
    dgtz_.reset();
    LOG_INFO(params_.name << ": closed");
}

} // namespace caendaq
