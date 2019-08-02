/*
 * Copyright (C) 2019  Christian Berger
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cluon-complete.hpp"
#include "opendlv-standard-message-set.hpp"
#include "can.hpp"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>

#ifdef __linux__
    #include <linux/if.h>
    #include <linux/can.h>
#endif

#include <unistd.h>

#include <cstring>

#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

int32_t main(int32_t argc, char **argv) {
    int32_t retCode{1};
    auto commandlineArguments = cluon::getCommandlineArguments(argc, argv);
    if ( (0 == commandlineArguments.count("cid")) ) {
        std::cerr << argv[0] << " captures the raw content of a CAN frame from a list of given CAN devices into an opendlv.proxy.RawCANFrame message that are either sent to an ongoing OD4 session or directly dumped to disk." << std::endl;
        std::cerr << "Usage:   " << argv[0] << " --cid=<OD4 session> --can-channels=CANdevice:ID[,CANdevice:ID]* [--verbose]" << std::endl;
        std::cerr << "         --can-channels: list of CAN devices followed by colon and a senderStamp per CAN  channel to differentiate the CAN frames" << std::endl;
        std::cerr << "         --cid:          CID of the OD4Session to send messages" << std::endl;
        std::cerr << "         --remote:       enable remotely activated recording" << std::endl;
        std::cerr << "         --rec:          name of the recording file; default: YYYY-MM-DD_HHMMSS.rec" << std::endl;
        std::cerr << "         --recsuffix:    additional suffix to add to the .rec file" << std::endl;
        std::cerr << "         --verbose:      print received frames" << std::endl;

        std::cerr << "Example: " << argv[0] << " --cid=111 --can-channels=can0:0,can1:1" << std::endl;
    }
    else {
#ifndef __linux__
        std::cerr << "[opendlv-device-can-raw]: SocketCAN not available on this platform. " << std::endl;
        return retCode;
#else
        const bool VERBOSE{commandlineArguments.count("verbose") != 0};
        const std::string CAN_CHANNELS{commandlineArguments["can-channels"]};
        auto getYYYYMMDD_HHMMSS = [](){
          cluon::data::TimeStamp now = cluon::time::now();

          const long int _seconds = now.seconds();
          struct tm *tm = localtime(&_seconds);

          uint32_t year = (1900 + tm->tm_year);
          uint32_t month = (1 + tm->tm_mon);
          uint32_t dayOfMonth = tm->tm_mday;
          uint32_t hours = tm->tm_hour;
          uint32_t minutes = tm->tm_min;
          uint32_t seconds = tm->tm_sec;

          std::stringstream sstr;
          sstr << year << "-" << ( (month < 10) ? "0" : "" ) << month << "-" << ( (dayOfMonth < 10) ? "0" : "" ) << dayOfMonth
                         << "_" << ( (hours < 10) ? "0" : "" ) << hours
                         << ( (minutes < 10) ? "0" : "" ) << minutes
                         << ( (seconds < 10) ? "0" : "" ) << seconds;

          std::string retVal{sstr.str()};
          return retVal;
        };
        const bool REMOTE{commandlineArguments.count("remote") != 0};
        const std::string REC{(commandlineArguments["rec"].size() != 0) ? commandlineArguments["rec"] : ""};
        const std::string RECSUFFIX{commandlineArguments["recsuffix"]};
        const std::string NAME_RECFILE{(REC.size() != 0) ? REC + RECSUFFIX : (getYYYYMMDD_HHMMSS() + RECSUFFIX + ".rec")};

        std::unique_ptr<cluon::OD4Session> od4{new cluon::OD4Session(static_cast<uint16_t>(std::stoi(commandlineArguments["cid"])))};

        std::string nameOfRecFile;
        std::mutex recFileMutex{};
        std::unique_ptr<std::fstream> recFile{nullptr};
        if (!REMOTE && !REC.empty()) {
          recFile.reset(new std::fstream(NAME_RECFILE.c_str(), std::ios::out|std::ios::binary|std::ios::trunc));
          std::cout << "[opendlv-device-can-raw]: Created " << NAME_RECFILE << "." << std::endl;
        }
        else {
          od4.reset(new cluon::OD4Session(static_cast<uint16_t>(std::stoi(commandlineArguments["cid"])),
              [REC, RECSUFFIX, getYYYYMMDD_HHMMSS, &recFileMutex, &recFile, &nameOfRecFile](cluon::data::Envelope &&envelope) noexcept {
            if (cluon::data::RecorderCommand::ID() == envelope.dataType()) {
              std::lock_guard<std::mutex> lck(recFileMutex);
              cluon::data::RecorderCommand rc = cluon::extractMessage<cluon::data::RecorderCommand>(std::move(envelope));
              if (1 == rc.command()) {
                if (recFile && recFile->good()) {
                  recFile->flush();
                  recFile->close();
                  recFile = nullptr;
                  std::cout << "[opendlv-device-can-raw]: Closed " << nameOfRecFile << "." << std::endl;
                }
                nameOfRecFile = (REC.size() != 0) ? REC + RECSUFFIX : (getYYYYMMDD_HHMMSS() + RECSUFFIX + ".rec");
                recFile.reset(new std::fstream(nameOfRecFile.c_str(), std::ios::out|std::ios::binary|std::ios::trunc));
                std::cout << "[opendlv-device-can-raw]: Created " << nameOfRecFile << "." << std::endl;
              }
              else if (2 == rc.command()) {
                if (recFile && recFile->good()) {
                  recFile->flush();
                  recFile->close();
                  std::cout << "[opendlv-device-can-raw]: Closed " << nameOfRecFile << "." << std::endl;
                }
                recFile = nullptr;
              }
            }
            else {
              std::lock_guard<std::mutex> lck(recFileMutex);
              if (recFile && recFile->good()) {
                std::string serializedData{cluon::serializeEnvelope(std::move(envelope))};
                recFile->write(serializedData.data(), serializedData.size());
                recFile->flush();
              }
            }
          }));
        }

        int maxSocketCAN{-1};
        struct CAN_CHANNEL {
          std::string name{""};
          uint32_t ID{0};
          struct sockaddr_can address{};
          int socketCAN{0};
        };

        std::vector<struct CAN_CHANNEL> listOfCANDevices;
        std::vector<std::string> canChannels = stringtoolbox::split(CAN_CHANNELS + ",", ',');
        for (auto canChannel : canChannels) {
          std::vector<std::string> aCanChannel = stringtoolbox::split(canChannel, ':');
          if (aCanChannel.size() == 2) {
            struct CAN_CHANNEL dev;
            dev.name = aCanChannel.at(0);
            dev.ID = static_cast<uint32_t>(std::stoi(aCanChannel.at(1)));

            if (!dev.name.empty()) {
              std::cerr << "[opendlv-device-can-raw] Opening " << dev.name << "... ";
              // Create socket for SocketCAN.
              dev.socketCAN = socket(PF_CAN, SOCK_RAW, CAN_RAW);
              if (dev.socketCAN < 0) {
                  std::cerr << "failed." << std::endl;

                  std::cerr << "[opendlv-device-can-raw] Error while creating socket: " << strerror(errno) << std::endl;
              }
              maxSocketCAN = (dev.socketCAN > maxSocketCAN) ? dev.socketCAN : maxSocketCAN;

              // Try opening the given CAN device node.
              struct ifreq ifr;
              memset(&ifr, 0, sizeof(ifr));
              strcpy(ifr.ifr_name, dev.name.c_str());
              if (0 != ioctl(dev.socketCAN, SIOCGIFINDEX, &ifr)) {
                  std::cerr << "failed." << std::endl;

                  std::cerr << "[opendlv-device-can-raw] Error while getting index for " << dev.name << ": " << strerror(errno) << std::endl;
                  return retCode;
              }

              // Setup address and port.
              memset(&dev.address, 0, sizeof(dev.address));
              dev.address.can_family = AF_CAN;
              dev.address.can_ifindex = ifr.ifr_ifindex;

              if (bind(dev.socketCAN, reinterpret_cast<struct sockaddr *>(&dev.address), sizeof(dev.address)) < 0) {
                  std::cerr << "failed." << std::endl;

                  std::cerr << "[opendlv-device-can-raw] Error while binding socket: " << strerror(errno) << std::endl;
                  return retCode;
              }
              std::cerr << "done." << std::endl;

              listOfCANDevices.push_back(dev);
            }
          }
        }


        struct can_frame frame;
        fd_set rfds;
        struct timeval timeout;
        struct timeval socketTimeStamp;
        int32_t nbytes = 0;

        while (od4->isRunning() && !listOfCANDevices.empty()) {
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            FD_ZERO(&rfds);
            // Monitor all CAN devices.
            for (auto canDevice : listOfCANDevices) {
              FD_SET(canDevice.socketCAN, &rfds);
            }

            select(maxSocketCAN + 1, &rfds, NULL, NULL, &timeout);

            for (auto canDevice : listOfCANDevices) {
                if (FD_ISSET(canDevice.socketCAN, &rfds)) {
                    nbytes = read(canDevice.socketCAN, &frame, sizeof(struct can_frame));
                    if ( (nbytes > 0) && (nbytes == sizeof(struct can_frame)) ) {
                        // Get receiving time stamp.
                        if (0 != ioctl(canDevice.socketCAN, SIOCGSTAMP, &socketTimeStamp)) {
                            // In case the ioctl failed, use traditional vsariant.
                            cluon::data::TimeStamp now{cluon::time::now()};
                            socketTimeStamp.tv_sec = now.seconds();
                            socketTimeStamp.tv_usec = now.microseconds();
                        }
                        union CANData {
                            char bytes[8];
                            uint64_t value{0};
                        } canData;
                        std::memcpy(canData.bytes, reinterpret_cast<char*>(frame.data), frame.can_dlc);

                        if (VERBOSE) {
                            std::cout << "[opendlv-device-can-raw]: " << canDevice.name << " 0x" << std::hex << frame.can_id << std::dec << " [" << +frame.can_dlc << "] " << std::hex << "0x" << canData.value << " (ID = " << canDevice.ID << ")" << std::dec << std::endl;
                        }

                        cluon::data::TimeStamp sampleTimeStamp;
                        sampleTimeStamp.seconds(socketTimeStamp.tv_sec)
                                       .microseconds(socketTimeStamp.tv_usec);
                        opendlv::proxy::RawUInt64CANFrame rawUInt64CANFrame;
                        rawUInt64CANFrame.canID(frame.can_id)
                                         .length(frame.can_dlc)
                                         .data(canData.value);

                        if (recFile && recFile->good()) {
                          cluon::data::Envelope envelope;
                          {
                            cluon::ToProtoVisitor protoEncoder;
                            {
                              envelope.dataType(rawUInt64CANFrame.ID());
                              rawUInt64CANFrame.accept(protoEncoder);
                              envelope.serializedData(protoEncoder.encodedData());
                              envelope.sent(cluon::time::now());
                              envelope.sampleTimeStamp(sampleTimeStamp);
                              envelope.senderStamp(canDevice.ID);
                            }
                          }

                          std::string serializedData{cluon::serializeEnvelope(std::move(envelope))};
                          recFile->write(serializedData.data(), serializedData.size());
                          recFile->flush();
                        }
                        else {
                          od4->send(rawUInt64CANFrame, sampleTimeStamp, canDevice.ID);
                        }
                    }
                }
            }
        }

        for (auto canDevice : listOfCANDevices) {
          std::clog << "[opendlv-device-can-raw] Closing " << canDevice.name << "... ";
          if (canDevice.socketCAN > -1) {
              close(canDevice.socketCAN);
          }
          std::clog << "done." << std::endl;
        }

        retCode = 0;
#endif
    }
    return retCode;
}

