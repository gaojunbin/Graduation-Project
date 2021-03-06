//
// The MIT License (MIT)
//
// Copyright (c) 2019 Livox. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <apr_general.h>
#include <apr_getopt.h>
#include <string.h>
#include <algorithm>
#include "lvx_file.h"

DeviceItem devices[kMaxLidarCount];
LvxFileHandle lvx_file_handler;
std::list<LvxBasePackDetail> point_packet_list;
std::condition_variable condition_variable;
std::mutex mtx;
int lidar_units_index[32];
int lvx_file_save_time = 10;
bool is_finish_extrinsic_parameter = false;

#define FRAME_RATE 20

/** Connect the first broadcast hub in default and connect specific device when use program options or broadcast_code_list is not empty. */
std::vector<std::string> broadcast_code_list = {
  //"000000000000001"
};

/** Receiving point cloud data from Livox Hub. */
void GetHubData(uint8_t handle, LivoxEthPacket *data, uint32_t data_num, void *client_data) {
  if (data) {
    if (is_finish_extrinsic_parameter) {
      std::lock_guard<std::mutex> lock(mtx);
      LvxBasePackDetail packet;
      packet.device_index = lidar_units_index[HubGetLidarHandle(data->slot, data->id)];
      lvx_file_handler.BasePointsHandle(data, packet);
      point_packet_list.push_back(packet);

      if (point_packet_list.size() % (50 * broadcast_code_list.size()) == 0) {
        condition_variable.notify_one();
      }
    }
  }
}

/** Callback function of starting sampling. */
void OnSampleCallback(uint8_t status, uint8_t handle, uint8_t response, void *data) {
  printf("OnSampleCallback statue %d handle %d response %d \n", status, handle, response);
  if (status == kStatusSuccess) {
    if (response != 0) {
      devices[handle].device_state = kDeviceStateConnect;
    }
  } else if (status == kStatusTimeout) {
    devices[handle].device_state = kDeviceStateConnect;
  }
}

/** Callback function of stopping sampling. */
void OnStopSampleCallback(uint8_t status, uint8_t handle, uint8_t response, void *data) {
}

/** Callback function of get LiDAR units' extrinsic parameter. */
void OnGetLidarUnitsExtrinsicParameter(uint8_t status, uint8_t handle, HubGetExtrinsicParameterResponse *response, void *data) {
  if (status == kStatusSuccess) {
    if (response != 0) {
      printf("OnGetLidarUnitsExtrinsicParameter statue %d handle %d response %d \n", status, handle, response->ret_code);
      std::lock_guard<std::mutex> lock(mtx);
      LvxDeviceInfo lidar_info;
      for (int i = 0; i < response->count; i++) {
        ExtrinsicParameterResponseItem temp;
        memcpy(&temp, (void *)(response->parameter_list + i), sizeof(ExtrinsicParameterResponseItem));
        strncpy((char *)lidar_info.lidar_broadcast_code, temp.broadcast_code, kBroadcastCodeSize);
        strncpy((char *)lidar_info.hub_broadcast_code, broadcast_code_list[0].c_str(), kBroadcastCodeSize);

        std::unique_ptr<DeviceInfo[]> device_list(new DeviceInfo[kMaxLidarCount]);
        std::unique_ptr<uint8_t> size(new uint8_t(kMaxLidarCount));
        GetConnectedDevices(device_list.get(), size.get());
        for (int j = 0; j < kMaxLidarCount; j++) {
          if (strncmp(device_list[j].broadcast_code, temp.broadcast_code, kBroadcastCodeSize) == 0) {
            lidar_units_index[device_list[j].handle] = i;
            lidar_info.device_index = i;
            break;
          }
        }

        lidar_info.device_type = devices[handle].info.type;
        lidar_info.pitch = temp.pitch;
        lidar_info.roll = temp.roll;
        lidar_info.yaw = temp.yaw;
        lidar_info.x = static_cast<float>(temp.x / 1000.0);
        lidar_info.y = static_cast<float>(temp.y / 1000.0);
        lidar_info.z = static_cast<float>(temp.z / 1000.0);
        lvx_file_handler.AddDeviceInfo(lidar_info);
      }
      is_finish_extrinsic_parameter = true;
      condition_variable.notify_one();
    }
  }
  else if (status == kStatusTimeout) {
    printf("GetLidarUnitsExtrinsicParameter timeout! \n");
  }
}

void OnHubLidarInfo(uint8_t status, uint8_t handle, HubQueryLidarInformationResponse *response, void *client_data) {
  if (status != kStatusSuccess) {
    printf("Device Query Informations Failed %d\n", status);
  }
  if (response) {
    int i = 0;
    for (i = 0; i < response->count; ++i) {
      printf("Hub Lidar Info broadcast code %s id %d slot %d \n ",
        response->device_info_list[i].broadcast_code,
        response->device_info_list[i].id,
        response->device_info_list[i].slot);
    }
  }
}

/** Callback function of changing of device state. */
void OnDeviceChange(const DeviceInfo *info, DeviceEvent type) {
  if (info == nullptr) {
    return;
  }
  printf("OnDeviceChange broadcast code %s update type %d\n", info->broadcast_code, type);
  uint8_t handle = info->handle;
  if (handle >= kMaxLidarCount) {
    return;
  }
  if (type == kEventConnect) {
    HubQueryLidarInformation(OnHubLidarInfo, nullptr);
    if (devices[handle].device_state == kDeviceStateDisconnect) {
      devices[handle].device_state = kDeviceStateConnect;
      devices[handle].info = *info;
    }
  } else if (type == kEventDisconnect) {
    devices[handle].device_state = kDeviceStateDisconnect;
  } else if (type == kEventStateChange) {
    devices[handle].info = *info;
  }

  if (devices[handle].device_state == kDeviceStateConnect) {
    printf("Device State error_code %d\n", devices[handle].info.status.status_code);
    printf("Device State working state %d\n", devices[handle].info.state);
    printf("Device feature %d\n", devices[handle].info.feature);
    if (devices[handle].info.state == kLidarStateNormal) {
      if (devices[handle].info.type == kDeviceTypeHub) {
        HubGetExtrinsicParameter(OnGetLidarUnitsExtrinsicParameter, nullptr);
        HubStartSampling(OnSampleCallback, nullptr);
        devices[handle].device_state = kDeviceStateSampling;
      }
    }
  }
}

/** Callback function when broadcast message received.
 * You need to add listening device broadcast code and set the point cloud data callback in this function.
 */
void OnDeviceBroadcast(const BroadcastDeviceInfo *info) {
  if (info == nullptr) {
    return;
  }

  printf("Receive Broadcast Code %s\n", info->broadcast_code);

  if (broadcast_code_list.size() > 0) {
    bool found = false;
    uint8_t i = 0;
    for (i = 0; i < broadcast_code_list.size(); ++i) {
      if (strncmp(info->broadcast_code, broadcast_code_list[i].c_str(), kBroadcastCodeSize) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      return;
    }
  }
  else {
    broadcast_code_list.push_back(info->broadcast_code);
    return;
  }

  bool result = false;
  uint8_t handle = 0;
  result = AddHubToConnect(info->broadcast_code, &handle);
  if (result == kStatusSuccess) {
    SetDataCallback(handle, GetHubData, nullptr);
    devices[handle].handle = handle;
    devices[handle].device_state = kDeviceStateDisconnect;
  }
}

/** Set the program options.
* You can input the registered device broadcast code and decide whether to save the log file.
*/
int SetProgramOption(int argc, const char *argv[]) {
  apr_status_t rv;
  apr_pool_t *mp = nullptr;
  static const apr_getopt_option_t opt_option[] = {
    /** Long-option, short-option, has-arg flag, description */
    { "code", 'c', 1, "Register device broadcast code" },     
    { "log", 'l', 0, "Save the log file" },    
    { "time", 't', 1, "Time to save point cloud to the lvx file" },
    { "help", 'h', 0, "Show help" },    
    { nullptr, 0, 0, nullptr },
  };
  apr_getopt_t *opt = nullptr;
  int optch = 0;
  const char *optarg = nullptr;

  if (apr_initialize() != APR_SUCCESS) {
    return -1;
  }

  if (apr_pool_create(&mp, NULL) != APR_SUCCESS) {
    return -1;
  }

  rv = apr_getopt_init(&opt, mp, argc, argv);
  if (rv != APR_SUCCESS) {
    printf("Program options initialization failed.\n");
    return -1;
  }

  /** Parse the all options based on opt_option[] */
  bool is_help = false;
  while ((rv = apr_getopt_long(opt, opt_option, &optch, &optarg)) == APR_SUCCESS) {
    switch (optch) {
    case 'c': {
      printf("Register broadcast code: %s\n", optarg);
      broadcast_code_list.push_back(optarg);
      break;
    }
    case 'l': {
      printf("Save the log file.\n");
      SaveLoggerFile();
      break;
    }
    case 't': {
      printf("Time to save point cloud to the lvx file:%s.\n", optarg);
      lvx_file_save_time = atoi(optarg);
      break;
    }
    case 'h': {
      printf(
        " [-c] Register device broadcast code\n"
        " [-l] Save the log file\n"
        " [-t] Time to save point cloud to the lvx file\n"
        " [-h] Show help\n"
      );
      is_help = true;
      break;
    }
    }
  }
  if (rv != APR_EOF) {
    printf("Invalid options.\n");
  }

  apr_pool_destroy(mp);
  mp = nullptr;
  if (is_help)
    return 1;
  return 0;
}

int main(int argc, const char *argv[]) {
/** Set the program options. */
  if (SetProgramOption(argc, argv))
    return 0;

  printf("Livox SDK initializing.\n");
/** Initialize Livox-SDK. */
  if (!Init()) {
    return -1;
  }
  printf("Livox SDK has been initialized.\n");

  LivoxSdkVersion _sdkversion;
  GetLivoxSdkVersion(&_sdkversion);
  printf("Livox SDK version %d.%d.%d .\n", _sdkversion.major, _sdkversion.minor, _sdkversion.patch);

  memset(devices, 0, sizeof(devices));

/** Set the callback function receiving broadcast message from Livox LiDAR. */
  SetBroadcastCallback(OnDeviceBroadcast);

/** Set the callback function called when device state change,
 * which means connection/disconnection and changing of LiDAR state.
 */
  SetDeviceStateUpdateCallback(OnDeviceChange);

/** Start the device discovering routine. */
  if (!Start()) {
    Uninit();
    return -1;
  }
  printf("Start discovering device.\n");

  {
    std::unique_lock<std::mutex> lock(mtx);
    condition_variable.wait(lock);
  }

  printf("Start initialize lvx file.\n");
  if (!lvx_file_handler.InitLvxFile()) {
    Uninit();
    return -1;
  }
  lvx_file_handler.InitLvxFileHeader();

  int i = 0;
  for (i = 0; i < lvx_file_save_time * FRAME_RATE; ++i) {
    std::list<LvxBasePackDetail> point_packet_list_temp;
    {
      std::unique_lock<std::mutex> lock(mtx);
      condition_variable.wait(lock);
      point_packet_list_temp.swap(point_packet_list);
    }

    printf("Finish save %d frame to lvx file.\n", i);
    lvx_file_handler.SaveFrameToLvxFile(point_packet_list_temp);
  }

  lvx_file_handler.CloseLvxFile();

  HubStopSampling(OnStopSampleCallback, NULL);
  printf("stop sample\n");

  Uninit();
}
