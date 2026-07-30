#include "caendaq/CaenDigitizer.hpp"

#include <cstdlib>
#include <fstream>

#include <json/json.h>

#include "caendaq/Log.hpp"
#include "class_caen_dgtz.h" // vendored driver (pulls in CAENDigitizer.h)

namespace caendaq {

namespace {
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

    // For DPP firmwares, let the library choose the aggregate organisation.
    const auto dpp = dgtz_->GetDPPVersion();
    if (dpp == CAEN_DGTZ_DPPFirmware_PHA ||
        dpp == CAEN_DGTZ_DPPFirmware_PSD ||
        dpp == CAEN_DGTZ_DPPFirmware_CI) {
        errcode_ = 0;
        //dgtz_->SetDPPEventAggregation(0, 0);
        if (errcode_ != 0) {
            LOG_ERROR(params_.name << ": SetDPPEventAggregation failed");
            return false;
        }
    }

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
        errcode_ = 0;
        dgtz_->ClearData();
        dgtz_->SWStartAcquisition();
        if (errcode_ != 0) { LOG_ERROR(params_.name << ": restart after reconnect failed"); return false; }
    }
    LOG_INFO(params_.name << ": reconnected");
    return true;
}

bool CaenDigitizer::read(const char** data, std::size_t* size) {
    *data = nullptr;
    *size = 0;
    if (!dgtz_ || readoutBuffer_ == nullptr) return false;

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
