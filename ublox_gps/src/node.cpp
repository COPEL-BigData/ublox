//==============================================================================
// Copyright (c) 2012, Johannes Meyer, TU Darmstadt
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the Flight Systems and Automatic Control group,
//       TU Darmstadt, nor the names of its contributors may be used to
//       endorse or promote products derived from this software without
//       specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//==============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <ublox_msgs/AidALM.h>
#include <ublox_msgs/AidEPH.h>
#include <ublox_msgs/AidHUI.h>
#include <ublox_msgs/CfgINF.h>
#include <ublox_msgs/CfgINFBlock.h>
#include <ublox_msgs/CfgNAV5.h>
#include <ublox_msgs/CfgPRT.h>
#include <ublox_msgs/Inf.h>
#include <ublox_msgs/MonVER.h>
#include <ublox_msgs/NavCLOCK.h>
#include <ublox_msgs/NavPOSECEF.h>
#include <ublox_msgs/NavSTATUS.h>

#include <ublox_gps/adr_udr_product.hpp>
#include <ublox_gps/fix_diagnostic.hpp>
#include <ublox_gps/fts_product.hpp>
#include <ublox_gps/gnss.hpp>
#include <ublox_gps/hp_pos_rec_product.hpp>
#include <ublox_gps/hpg_ref_product.hpp>
#include <ublox_gps/hpg_rov_product.hpp>
#include <ublox_gps/node.hpp>
#include <ublox_gps/raw_data_product.hpp>
#include <ublox_gps/tim_product.hpp>
#include <ublox_gps/ublox_firmware6.hpp>
#include <ublox_gps/ublox_firmware7.hpp>
#include <ublox_gps/ublox_firmware8.hpp>
#include <ublox_gps/ublox_firmware9.hpp>

namespace ublox_node {

/**
 * @brief Determine dynamic model from human-readable string.
 * @param model One of the following (case-insensitive):
 *  - portable
 *  - stationary
 *  - pedestrian
 *  - automotive
 *  - sea
 *  - airborne1
 *  - airborne2
 *  - airborne4
 *  - wristwatch
 * @return DynamicModel
 * @throws std::runtime_error on invalid argument.
 */
uint8_t modelFromString(const std::string& model) {
  std::string lower = model;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  if (lower == "portable") {
    return ublox_msgs::CfgNAV5::DYN_MODEL_PORTABLE;
  } else if (lower == "stationary") {
    return ublox_msgs::CfgNAV5::DYN_MODEL_STATIONARY;
  } else if (lower == "pedestrian") {
    return ublox_msgs::CfgNAV5::DYN_MODEL_PEDESTRIAN;
  } else if (lower == "automotive") {
    return ublox_msgs::CfgNAV5::DYN_MODEL_AUTOMOTIVE;
  } else if (lower == "sea") {
    return ublox_msgs::CfgNAV5::DYN_MODEL_SEA;
  } else if (lower == "airborne1") {
    return ublox_msgs::CfgNAV5::DYN_MODEL_AIRBORNE_1G;
  } else if (lower == "airborne2") {
    return ublox_msgs::CfgNAV5::DYN_MODEL_AIRBORNE_2G;
  } else if (lower == "airborne4") {
    return ublox_msgs::CfgNAV5::DYN_MODEL_AIRBORNE_4G;
  } else if (lower == "wristwatch") {
    return ublox_msgs::CfgNAV5::DYN_MODEL_WRIST_WATCH;
  }

  throw std::runtime_error("Invalid settings: " + lower +
                           " is not a valid dynamic model.");
}

/**
 * @brief Determine fix mode from human-readable string.
 * @param mode One of the following (case-insensitive):
 *  - 2d
 *  - 3d
 *  - auto
 * @return FixMode
 * @throws std::runtime_error on invalid argument.
 */
uint8_t fixModeFromString(const std::string& mode) {
  std::string lower = mode;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  if (lower == "2d") {
    return ublox_msgs::CfgNAV5::FIX_MODE_2D_ONLY;
  } else if (lower == "3d") {
    return ublox_msgs::CfgNAV5::FIX_MODE_3D_ONLY;
  } else if (lower == "auto") {
    return ublox_msgs::CfgNAV5::FIX_MODE_AUTO;
  }

  throw std::runtime_error("Invalid settings: " + mode +
                           " is not a valid fix mode.");
}

std::vector<std::string> stringSplit(const std::string &str,
                                     const std::string &splitter) {
  std::vector<std::string> ret;
  size_t next = 0;
  size_t current = next;

  if (splitter.empty()) {
    // If the splitter is blank, just return the original
    ret.push_back(str);
    return ret;
  }

  while (next != std::string::npos) {
    next = str.find(splitter, current);
    ret.push_back(str.substr(current, next - current));
    current = next + splitter.length();
  }

  return ret;
}

//
// u-blox ROS Node
//
UbloxNode::UbloxNode() {
  nh_ = std::make_shared<ros::NodeHandle>("~");

  int debug;
  nh_->param("debug", debug, 1);
  if (debug) {
    if (ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME,
                                       ros::console::levels::Debug)) {
      ros::console::notifyLoggerLevelsChanged();
    }
  }

  gps_ = std::make_shared<ublox_gps::Gps>(debug);

  gnss_ = std::make_shared<Gnss>();

  nav_status_pub_ = nh_->advertise<ublox_msgs::NavSTATUS>("navstatus", 1);
  nav_posecef_pub_ = nh_->advertise<ublox_msgs::NavPOSECEF>("navposecef", 1);
  nav_clock_pub_ = nh_->advertise<ublox_msgs::NavCLOCK>("navclock", 1);
  aid_alm_pub_ = nh_->advertise<ublox_msgs::AidALM>("aidalm", 1);
  aid_eph_pub_ = nh_->advertise<ublox_msgs::AidEPH>("aideph", 1);
  aid_hui_pub_ = nh_->advertise<ublox_msgs::AidHUI>("aidhui", 1);

  updater_ = std::make_shared<diagnostic_updater::Updater>();
  updater_->setHardwareID("ublox");

  // configure diagnostic updater for frequency
  freq_diag_ = std::make_shared<FixDiagnostic>(std::string("fix"), kFixFreqTol,
                                               kFixFreqWindow, kTimeStampStatusMin, nav_rate_, meas_rate_, updater_);

  initialize();
}

void UbloxNode::addFirmwareInterface() {
  int ublox_version;
  if (protocol_version_ < 14) {
    components_.push_back(std::make_shared<UbloxFirmware6>(frame_id_, updater_, freq_diag_, gnss_, nh_.get()));
    ublox_version = 6;
  } else if (protocol_version_ >= 14 && protocol_version_ <= 15) {
    components_.push_back(std::make_shared<UbloxFirmware7>(frame_id_, updater_, freq_diag_, gnss_, nh_.get()));
    ublox_version = 7;
  } else if (protocol_version_ > 15 && protocol_version_ <= 23) {
    components_.push_back(std::make_shared<UbloxFirmware8>(frame_id_, updater_, freq_diag_, gnss_, nh_.get()));
    ublox_version = 8;
  } else {
    components_.push_back(std::make_shared<UbloxFirmware9>(frame_id_, updater_, freq_diag_, gnss_, nh_.get()));
    ublox_version = 9;
  }

  ROS_INFO("U-Blox Firmware Version: %d", ublox_version);
}


void UbloxNode::addProductInterface(const std::string & product_category,
                                    const std::string & ref_rov) {
  if (product_category.compare("HPG") == 0 && ref_rov.compare("REF") == 0) {
    components_.push_back(std::make_shared<HpgRefProduct>(nav_rate_, meas_rate_, updater_, rtcms_, nh_.get()));
  } else if (product_category.compare("HPG") == 0 && ref_rov.compare("ROV") == 0) {
    components_.push_back(std::make_shared<HpgRovProduct>(nav_rate_, updater_, nh_.get()));
  } else if (product_category.compare("HPG") == 0) {
    components_.push_back(std::make_shared<HpPosRecProduct>(nav_rate_, meas_rate_, frame_id_, updater_, rtcms_, nh_.get()));
  } else if (product_category.compare("TIM") == 0) {
    components_.push_back(std::make_shared<TimProduct>(frame_id_, updater_, nh_.get()));
  } else if (product_category.compare("ADR") == 0 ||
             product_category.compare("UDR") == 0) {
    components_.push_back(std::make_shared<AdrUdrProduct>(nav_rate_, meas_rate_, frame_id_, updater_, nh_.get()));
  } else if (product_category.compare("FTS") == 0) {
    components_.push_back(std::make_shared<FtsProduct>());
  } else if (product_category.compare("SPG") != 0) {
    ROS_WARN("Product category %s %s from MonVER message not recognized %s",
             product_category.c_str(), ref_rov.c_str(),
             "options are HPG REF, HPG ROV, HPG #.#, TIM, ADR, UDR, FTS, SPG");
  }
}

void UbloxNode::getRosParams() {
  nh_->param("device", device_, std::string("/dev/ttyACM0"));
  nh_->param("frame_id", frame_id_, std::string("gps"));

  // Save configuration parameters
  getRosUint(nh_.get(), "load/mask", load_.load_mask, 0);
  getRosUint(nh_.get(), "load/device", load_.device_mask, 0);
  getRosUint(nh_.get(), "save/mask", save_.save_mask, 0);
  getRosUint(nh_.get(), "save/device", save_.device_mask, 0);

  // UART 1 params
  getRosUint(nh_.get(), "uart1/baudrate", baudrate_, 9600);
  getRosUint(nh_.get(), "uart1/in", uart_in_, ublox_msgs::CfgPRT::PROTO_UBX
                                    | ublox_msgs::CfgPRT::PROTO_NMEA
                                    | ublox_msgs::CfgPRT::PROTO_RTCM);
  getRosUint(nh_.get(), "uart1/out", uart_out_, ublox_msgs::CfgPRT::PROTO_UBX);
  // USB params
  set_usb_ = false;
  if (nh_->hasParam("usb/in") || nh_->hasParam("usb/out")) {
    set_usb_ = true;
    if (!getRosUint(nh_.get(), "usb/in", usb_in_)) {
      throw std::runtime_error(std::string("usb/out is set, therefore ") +
        "usb/in must be set");
    }
    if (!getRosUint(nh_.get(), "usb/out", usb_out_)) {
      throw std::runtime_error(std::string("usb/in is set, therefore ") +
        "usb/out must be set");
    }
    getRosUint(nh_.get(), "usb/tx_ready", usb_tx_, 0);
  }
  // Measurement rate params
  nh_->param("rate", rate_, 4.0);  // in Hz
  getRosUint(nh_.get(), "nav_rate", nav_rate_, 1);  // # of measurement rate cycles
  // RTCM params
  std::vector<uint8_t> rtcm_ids;
  std::vector<uint8_t> rtcm_rates;
  getRosUint(nh_.get(), "rtcm/ids", rtcm_ids);  // RTCM output message IDs
  getRosUint(nh_.get(), "rtcm/rates", rtcm_rates);  // RTCM output message rates
  // PPP: Advanced Setting
  declareRosBoolean(nh_.get(), "enable_ppp", false);
  // SBAS params, only for some devices
  declareRosBoolean(nh_.get(), "gnss/sbas", false);
  declareRosBoolean(nh_.get(), "gnss/gps", true);
  declareRosBoolean(nh_.get(), "gnss/glonass", false);
  declareRosBoolean(nh_.get(), "gnss/qzss", false);
  declareRosBoolean(nh_.get(), "gnss/galileo", false);
  declareRosBoolean(nh_.get(), "gnss/beidou", false);
  declareRosBoolean(nh_.get(), "gnss/imes", false);
  getRosUint(nh_.get(), "sbas/max", max_sbas_, 0); // Maximum number of SBAS channels
  getRosUint(nh_.get(), "sbas/usage", sbas_usage_, 0);
  nh_->param("dynamic_model", dynamic_model_, std::string("portable"));
  nh_->param("fix_mode", fix_mode_, std::string("auto"));
  getRosUint(nh_.get(), "dr_limit", dr_limit_, 0); // Dead reckoning limit

  if (getRosBoolean(nh_.get(), "enable_ppp")) {
    ROS_WARN("Warning: PPP is enabled - this is an expert setting.");
  }

  checkMin(rate_, 0, "rate");

  if (rtcm_ids.size() != rtcm_rates.size()) {
    throw std::runtime_error(std::string("Invalid settings: size of rtcm_ids") +
                             " must match size of rtcm_rates");
  }

  rtcms_.resize(rtcm_ids.size());
  for (size_t i = 0; i < rtcm_ids.size(); ++i) {
    rtcms_[i].id = rtcm_ids[i];
    rtcms_[i].rate = rtcm_rates[i];
  }

  dmodel_ = modelFromString(dynamic_model_);
  fmode_ = fixModeFromString(fix_mode_);

  declareRosBoolean(nh_.get(), "dat/set", false);
  if (getRosBoolean(nh_.get(), "dat/set")) {
    std::vector<float> shift, rot;
    if (!nh_->getParam("dat/majA", cfg_dat_.maj_a)
        || nh_->getParam("dat/flat", cfg_dat_.flat)
        || nh_->getParam("dat/shift", shift)
        || nh_->getParam("dat/rot", rot)
        || nh_->getParam("dat/scale", cfg_dat_.scale)) {
      throw std::runtime_error(std::string("dat/set is true, therefore ") +
         "dat/majA, dat/flat, dat/shift, dat/rot, & dat/scale must be set");
    }
    if (shift.size() != 3 || rot.size() != 3) {
      throw std::runtime_error(std::string("size of dat/shift & dat/rot ") +
                               "must be 3");
    }
    checkRange(cfg_dat_.maj_a, 6300000.0, 6500000.0, "dat/majA");
    checkRange(cfg_dat_.flat, 0.0, 500.0, "dat/flat");

    checkRange(shift, 0.0, 500.0, "dat/shift");
    cfg_dat_.d_x = shift[0];
    cfg_dat_.d_y = shift[1];
    cfg_dat_.d_z = shift[2];

    checkRange(rot, -5000.0, 5000.0, "dat/rot");
    cfg_dat_.rot_x = rot[0];
    cfg_dat_.rot_y = rot[1];
    cfg_dat_.rot_z = rot[2];

    checkRange(cfg_dat_.scale, 0.0, 50.0, "scale");
  }

  // measurement period [ms]
  meas_rate_ = 1000 / rate_;

  // activate/deactivate any config
  declareRosBoolean(nh_.get(), "config_on_startup", true);
  declareRosBoolean(nh_.get(), "raw_data", false);
  declareRosBoolean(nh_.get(), "clear_bbr", false);
  declareRosBoolean(nh_.get(), "save_on_shutdown", false);
  declareRosBoolean(nh_.get(), "use_adr", true);

  declareRosBoolean(nh_.get(), "sv_in/reset", true);

  // raw data stream logging
  rawDataStreamPa_.getRosParams();

  // NMEA parameters
  declareRosBoolean(nh_.get(), "nmea/set", false);
  declareRosBoolean(nh_.get(), "nmea/compat", false);
  declareRosBoolean(nh_.get(), "nmea/consider", false);
  declareRosBoolean(nh_.get(), "nmea/limit82", false);
  declareRosBoolean(nh_.get(), "nmea/high_prec", false);
  declareRosBoolean(nh_.get(), "nmea/filter/pos", false);
  declareRosBoolean(nh_.get(), "nmea/filter/msk_pos", false);
  declareRosBoolean(nh_.get(), "nmea/filter/time", false);
  declareRosBoolean(nh_.get(), "nmea/filter/date", false);
  declareRosBoolean(nh_.get(), "nmea/filter/sbas", false);
  declareRosBoolean(nh_.get(), "nmea/filter/track", false);
  declareRosBoolean(nh_.get(), "nmea/filter/gps_only", false);
  declareRosBoolean(nh_.get(), "nmea/gnssToFilter/gps", false);
  declareRosBoolean(nh_.get(), "nmea/gnssToFilter/sbas", false);
  declareRosBoolean(nh_.get(), "nmea/gnssToFilter/qzss", false);
  declareRosBoolean(nh_.get(), "nmea/gnssToFilter/glonass", false);
  declareRosBoolean(nh_.get(), "nmea/gnssToFilter/beidou", false);

  // Publish parameters
  declareRosBoolean(nh_.get(), "publish/all", false);

  declareRosBoolean(nh_.get(), "publish/nav/all", getRosBoolean(nh_.get(), "publish/all"));
  declareRosBoolean(nh_.get(), "publish/nav/att", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/clock", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/heading", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/posecef", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/posllh", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/pvt", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/relposned", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/sat", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/sol", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/svin", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/svinfo", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/status", getRosBoolean(nh_.get(), "publish/nav/all"));
  declareRosBoolean(nh_.get(), "publish/nav/velned", getRosBoolean(nh_.get(), "publish/nav/all"));

  declareRosBoolean(nh_.get(), "publish/rxm/all", getRosBoolean(nh_.get(), "publish/all"));
  declareRosBoolean(nh_.get(), "publish/rxm/almRaw", getRosBoolean(nh_.get(), "publish/rxm/all"));
  declareRosBoolean(nh_.get(), "publish/rxm/eph", getRosBoolean(nh_.get(), "publish/rxm/all"));
  declareRosBoolean(nh_.get(), "publish/rxm/rtcm", getRosBoolean(nh_.get(), "publish/rxm/all"));
  declareRosBoolean(nh_.get(), "publish/rxm/raw", getRosBoolean(nh_.get(), "publish/rxm/all"));
  declareRosBoolean(nh_.get(), "publish/rxm/sfrb", getRosBoolean(nh_.get(), "publish/rxm/all"));

  declareRosBoolean(nh_.get(), "publish/aid/all", getRosBoolean(nh_.get(), "publish/all"));
  declareRosBoolean(nh_.get(), "publish/aid/alm", getRosBoolean(nh_.get(), "publish/aid/all"));
  declareRosBoolean(nh_.get(), "publish/aid/eph", getRosBoolean(nh_.get(), "publish/aid/all"));
  declareRosBoolean(nh_.get(), "publish/aid/hui", getRosBoolean(nh_.get(), "publish/aid/all"));

  declareRosBoolean(nh_.get(), "publish/mon/all", getRosBoolean(nh_.get(), "publish/all"));
  declareRosBoolean(nh_.get(), "publish/mon/hw", getRosBoolean(nh_.get(), "publish/mon/all"));

  declareRosBoolean(nh_.get(), "publish/tim/tm2", false);

  // INF parameters
  declareRosBoolean(nh_.get(), "inf/all", true);
  declareRosBoolean(nh_.get(), "inf/debug", false);
  declareRosBoolean(nh_.get(), "inf/error", getRosBoolean(nh_.get(), "inf/all"));
  declareRosBoolean(nh_.get(), "inf/notice", getRosBoolean(nh_.get(), "inf/all"));
  declareRosBoolean(nh_.get(), "inf/test", getRosBoolean(nh_.get(), "inf/all"));
  declareRosBoolean(nh_.get(), "inf/warning", getRosBoolean(nh_.get(), "inf/all"));

  // ESF parameters
  declareRosBoolean(nh_.get(), "publish/esf/all", true);
  declareRosBoolean(nh_.get(), "publish/esf/ins", getRosBoolean(nh_.get(), "publish/esf/all"));
  declareRosBoolean(nh_.get(), "publish/esf/meas", getRosBoolean(nh_.get(), "publish/esf/all"));
  declareRosBoolean(nh_.get(), "publish/esf/raw", getRosBoolean(nh_.get(), "publish/esf/all"));
  declareRosBoolean(nh_.get(), "publish/esf/status", getRosBoolean(nh_.get(), "publish/esf/all"));

  // HNR parameters
  declareRosBoolean(nh_.get(), "publish/hnr/pvt", true);
}

void UbloxNode::pollMessages(const ros::TimerEvent& event) {
  static std::vector<uint8_t> payload(1, 1);
  if (getRosBoolean(nh_.get(), "publish/aid/alm")) {
    gps_->poll(ublox_msgs::Class::AID, ublox_msgs::Message::AID::ALM, payload);
  }
  if (getRosBoolean(nh_.get(), "publish/aid/eph")) {
    gps_->poll(ublox_msgs::Class::AID, ublox_msgs::Message::AID::EPH, payload);
  }
  if (getRosBoolean(nh_.get(), "publish/aid/hui")) {
    gps_->poll(ublox_msgs::Class::AID, ublox_msgs::Message::AID::HUI);
  }

  payload[0]++;
  if (payload[0] > 32) {
    payload[0] = 1;
  }
}

void UbloxNode::printInf(const ublox_msgs::Inf &m, uint8_t id) {
  if (id == ublox_msgs::Message::INF::ERROR) {
    ROS_ERROR_STREAM("INF: " << std::string(m.str.begin(), m.str.end()));
  } else if (id == ublox_msgs::Message::INF::WARNING) {
    ROS_WARN_STREAM("INF: " << std::string(m.str.begin(), m.str.end()));
  } else if (id == ublox_msgs::Message::INF::DEBUG) {
    ROS_DEBUG_STREAM("INF: " << std::string(m.str.begin(), m.str.end()));
  } else {
    ROS_INFO_STREAM("INF: " << std::string(m.str.begin(), m.str.end()));
  }
}

void UbloxNode::subscribe() {
  ROS_DEBUG("Subscribing to U-Blox messages");
  // subscribe messages

  // Nav Messages
  if (getRosBoolean(nh_.get(), "publish/nav/status")) {
    gps_->subscribe<ublox_msgs::NavSTATUS>([this](const ublox_msgs::NavSTATUS &m) { nav_status_pub_.publish(m); },
                                           1);
  }

  if (getRosBoolean(nh_.get(), "publish/nav/posecef")) {
    gps_->subscribe<ublox_msgs::NavPOSECEF>([this](const ublox_msgs::NavPOSECEF &m) { nav_posecef_pub_.publish(m); },
                                            1);
  }

  if (getRosBoolean(nh_.get(), "publish/nav/clock")) {
    gps_->subscribe<ublox_msgs::NavCLOCK>([this](const ublox_msgs::NavCLOCK &m) { nav_clock_pub_.publish(m); },
                                          1);
  }

  // INF messages
  if (getRosBoolean(nh_.get(), "inf/debug")) {
    gps_->subscribeId<ublox_msgs::Inf>(
        std::bind(&UbloxNode::printInf, this, std::placeholders::_1,
                    ublox_msgs::Message::INF::DEBUG),
        ublox_msgs::Message::INF::DEBUG);
  }

  if (getRosBoolean(nh_.get(), "inf/error")) {
    gps_->subscribeId<ublox_msgs::Inf>(
        std::bind(&UbloxNode::printInf, this, std::placeholders::_1,
                    ublox_msgs::Message::INF::ERROR),
        ublox_msgs::Message::INF::ERROR);
  }

  if (getRosBoolean(nh_.get(), "inf/notice")) {
    gps_->subscribeId<ublox_msgs::Inf>(
        std::bind(&UbloxNode::printInf, this, std::placeholders::_1,
                    ublox_msgs::Message::INF::NOTICE),
        ublox_msgs::Message::INF::NOTICE);
  }

  if (getRosBoolean(nh_.get(), "inf/test")) {
    gps_->subscribeId<ublox_msgs::Inf>(
        std::bind(&UbloxNode::printInf, this, std::placeholders::_1,
                    ublox_msgs::Message::INF::TEST),
        ublox_msgs::Message::INF::TEST);
  }

  if (getRosBoolean(nh_.get(), "inf/warning")) {
    gps_->subscribeId<ublox_msgs::Inf>(
        std::bind(&UbloxNode::printInf, this, std::placeholders::_1,
                    ublox_msgs::Message::INF::WARNING),
        ublox_msgs::Message::INF::WARNING);
  }

  // AID messages
  if (getRosBoolean(nh_.get(), "publish/aid/alm")) {
    gps_->subscribe<ublox_msgs::AidALM>([this](const ublox_msgs::AidALM &m) { aid_alm_pub_.publish(m); },
                                        1);
  }

  if (getRosBoolean(nh_.get(), "publish/aid/eph")) {
    gps_->subscribe<ublox_msgs::AidEPH>([this](const ublox_msgs::AidEPH &m) { aid_eph_pub_.publish(m); },
                                        1);
  }

  if (getRosBoolean(nh_.get(), "publish/aid/hui")) {
    gps_->subscribe<ublox_msgs::AidHUI>([this](const ublox_msgs::AidHUI &m) { aid_hui_pub_.publish(m); },
                                        1);
  }

  for (size_t i = 0; i < components_.size(); i++) {
    components_[i]->subscribe(gps_);
  }
}

void UbloxNode::initializeRosDiagnostics() {
  declareRosBoolean(nh_.get(), "diagnostic_period", kDiagnosticPeriod);

  for (int i = 0; i < components_.size(); i++) {
    components_[i]->initializeRosDiagnostics();
  }
}

void UbloxNode::processMonVer() {
  ublox_msgs::MonVER monVer;
  if (!gps_->poll(monVer)) {
    throw std::runtime_error("Failed to poll MonVER & set relevant settings");
  }

  ROS_DEBUG("%s, HW VER: %s", monVer.sw_version.c_array(),
            monVer.hw_version.c_array());
  // Convert extension to vector of strings
  std::vector<std::string> extension;
  extension.reserve(monVer.extension.size());
  for (std::size_t i = 0; i < monVer.extension.size(); ++i) {
    ROS_DEBUG("%s", monVer.extension[i].field.c_array());
    // Find the end of the string (null character)
    unsigned char* end = std::find(monVer.extension[i].field.begin(),
          monVer.extension[i].field.end(), '\0');
    extension.push_back(std::string(monVer.extension[i].field.begin(), end));
  }

  // Get the protocol version
  for (std::size_t i = 0; i < extension.size(); ++i) {
    std::size_t found = extension[i].find("PROTVER");
    if (found != std::string::npos) {
      protocol_version_ = ::atof(
          extension[i].substr(8, extension[i].size()-8).c_str());
      break;
    }
  }
  if (protocol_version_ == 0) {
    ROS_WARN("Failed to parse MonVER and determine protocol version. %s",
             "Defaulting to firmware version 6.");
  }
  addFirmwareInterface();

  if (protocol_version_ < 18) {
    // Final line contains supported GNSS delimited by ;
    std::vector<std::string> strs;
    if (extension.size() > 0) {
      strs = stringSplit(extension[extension.size() - 1], ";");
    }
    for (size_t i = 0; i < strs.size(); i++) {
      gnss_->add(strs[i]);
    }
  } else {
    for (std::size_t i = 0; i < extension.size(); ++i) {
      std::vector<std::string> strs;
      // Up to 2nd to last line
      if (i <= extension.size() - 2) {
        strs = stringSplit(extension[i], "=");
        if (strs.size() > 1) {
          if (strs[0].compare(std::string("FWVER")) == 0) {
            if (strs[1].length() > 8) {
              addProductInterface(strs[1].substr(0, 3), strs[1].substr(8, 10));
            } else {
              addProductInterface(strs[1].substr(0, 3));
            }
            continue;
          }
        }
      }
      // Last 1-2 lines contain supported GNSS
      if (i >= extension.size() - 2) {
        strs = stringSplit(extension[i], ";");
        for (size_t i = 0; i < strs.size(); i++) {
          gnss_->add(strs[i]);
        }
      }
    }
  }
}

bool UbloxNode::configureUblox() {
  try {
    if (!gps_->isInitialized()) {
      throw std::runtime_error("Failed to initialize.");
    }
    if (load_.load_mask != 0) {
      ROS_DEBUG("Loading u-blox configuration from memory. %u", load_.load_mask);
      if (!gps_->configure(load_)) {
        throw std::runtime_error(std::string("Failed to load configuration ") +
                                 "from memory");
      }
      if (load_.load_mask & load_.MASK_IO_PORT) {
        ROS_DEBUG("Loaded I/O configuration from memory, resetting serial %s",
          "communications.");
        std::chrono::seconds wait(kResetWait);
        gps_->reset(wait);
        if (!gps_->isConfigured())
          throw std::runtime_error(std::string("Failed to reset serial I/O") +
            "after loading I/O configurations from device memory.");
      }
    }

    if (getRosBoolean(nh_.get(), "config_on_startup")) {
      if (set_usb_) {
        gps_->configUsb(usb_tx_, usb_in_, usb_out_);
      }
      if (!gps_->configRate(meas_rate_, nav_rate_)) {
        std::stringstream ss;
        ss << "Failed to set measurement rate to " << meas_rate_
          << "ms and navigation rate to " << nav_rate_;
        throw std::runtime_error(ss.str());
      }
      // If device doesn't have SBAS, will receive NACK (causes exception)
      if (gnss_->isSupported("SBAS")) {
        if (!gps_->configSbas(getRosBoolean(nh_.get(), "gnss/sbas"), sbas_usage_, max_sbas_)) {
          throw std::runtime_error(std::string("Failed to ") +
                                  (getRosBoolean(nh_.get(), "gnss/sbas") ? "enable" : "disable") +
                                  " SBAS.");
        }
      }
      if (!gps_->setPpp(getRosBoolean(nh_.get(), "enable_ppp"))) {
        throw std::runtime_error(std::string("Failed to ") +
                                (getRosBoolean(nh_.get(), "enable_ppp") ? "enable" : "disable")
                                + " PPP.");
      }
      if (!gps_->setDynamicModel(dmodel_)) {
        throw std::runtime_error("Failed to set model: " + dynamic_model_ + ".");
      }
      if (!gps_->setFixMode(fmode_)) {
        throw std::runtime_error("Failed to set fix mode: " + fix_mode_ + ".");
      }
      if (!gps_->setDeadReckonLimit(dr_limit_)) {
        std::stringstream ss;
        ss << "Failed to set dead reckoning limit: " << dr_limit_ << ".";
        throw std::runtime_error(ss.str());
      }
      if (getRosBoolean(nh_.get(), "dat/set") && !gps_->configure(cfg_dat_)) {
        throw std::runtime_error("Failed to set user-defined datum.");
      }
      // Configure each component
      for (int i = 0; i < components_.size(); i++) {
        if (!components_[i]->configureUblox(gps_)) {
          return false;
        }
      }
    }
    if (save_.save_mask != 0) {
      ROS_DEBUG("Saving the u-blox configuration, mask %u, device %u",
                save_.save_mask, save_.device_mask);
      if (!gps_->configure(save_)) {
        ROS_ERROR("u-blox unable to save configuration to non-volatile memory");
      }
    }
  } catch (std::exception& e) {
    ROS_FATAL("Error configuring u-blox: %s", e.what());
    return false;
  }
  return true;
}

void UbloxNode::configureInf() {
  ublox_msgs::CfgINF msg;
  // Subscribe to UBX INF messages
  ublox_msgs::CfgINFBlock block;
  block.protocol_id = block.PROTOCOL_ID_UBX;
  // Enable desired INF messages on each UBX port
  uint8_t mask = (getRosBoolean(nh_.get(), "inf/error") ? block.INF_MSG_ERROR : 0) |
                 (getRosBoolean(nh_.get(), "inf/warning") ? block.INF_MSG_WARNING : 0) |
                 (getRosBoolean(nh_.get(), "inf/notice") ? block.INF_MSG_NOTICE : 0) |
                 (getRosBoolean(nh_.get(), "inf/test") ? block.INF_MSG_TEST : 0) |
                 (getRosBoolean(nh_.get(), "inf/debug") ? block.INF_MSG_DEBUG : 0);
  for (size_t i = 0; i < block.inf_msg_mask.size(); i++) {
    block.inf_msg_mask[i] = mask;
  }

  msg.blocks.push_back(block);

  // IF NMEA is enabled
  if (uart_in_ & ublox_msgs::CfgPRT::PROTO_NMEA) {
    ublox_msgs::CfgINFBlock block;
    block.protocol_id = block.PROTOCOL_ID_NMEA;
    // Enable desired INF messages on each NMEA port
    for (size_t i = 0; i < block.inf_msg_mask.size(); i++) {
      block.inf_msg_mask[i] = mask;
    }
    msg.blocks.push_back(block);
  }

  ROS_DEBUG("Configuring INF messages");
  if (!gps_->configure(msg)) {
    ROS_WARN("Failed to configure INF messages");
  }
}

void UbloxNode::initializeIo() {
  gps_->setConfigOnStartup(getRosBoolean(nh_.get(), "config_on_startup"));

  std::smatch match;
  if (std::regex_match(device_, match,
                       std::regex("(tcp|udp)://(.+):(\\d+)"))) {
    std::string proto(match[1]);
    if (proto == "tcp") {
      std::string host(match[2]);
      std::string port(match[3]);
      ROS_INFO("Connecting to %s://%s:%s ...", proto.c_str(), host.c_str(),
               port.c_str());
      gps_->initializeTcp(host, port);
    } else {
      throw std::runtime_error("Protocol '" + proto + "' is unsupported");
    }
  } else {
    gps_->initializeSerial(device_, baudrate_, uart_in_, uart_out_);
  }

  // raw data stream logging
  if (rawDataStreamPa_.isEnabled()) {
    gps_->setRawDataCallback(
      std::bind(&RawDataStreamPa::ubloxCallback, &rawDataStreamPa_, std::placeholders::_1, std::placeholders::_2));
    rawDataStreamPa_.initialize();
  }
}

void UbloxNode::initialize() {
  // Params must be set before initializing IO
  getRosParams();
  initializeIo();
  // Must process Mon VER before setting firmware/hardware params
  processMonVer();
  if (protocol_version_ <= 14) {
    if (getRosBoolean(nh_.get(), "raw_data")) {
      components_.push_back(std::make_shared<RawDataProduct>(nav_rate_, meas_rate_, updater_, nh_.get()));
    }
  }
  // Must set firmware & hardware params before initializing diagnostics
  for (size_t i = 0; i < components_.size(); i++) {
    components_[i]->getRosParams();
  }
  // Do this last
  initializeRosDiagnostics();

  if (configureUblox()) {
    ROS_INFO("U-Blox configured successfully.");
    // Subscribe to all U-Blox messages
    subscribe();
    // Configure INF messages (needs INF params, call after subscribing)
    configureInf();

    poller_ = nh_->createTimer(ros::Duration(kPollDuration),
                               &UbloxNode::pollMessages,
                               this);
  }
}

void UbloxNode::shutdown() {
  if (gps_->isInitialized()) {
    gps_->close();
    ROS_INFO("Closed connection to %s.", device_.c_str());
  }
}

UbloxNode::~UbloxNode() {
  shutdown();
}

}  // namespace ublox_node

int main(int argc, char** argv) {
  ros::init(argc, argv, "ublox_gps");
  ublox_node::UbloxNode node;

  ros::spin();

  return 0;
}
