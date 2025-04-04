/*
Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of The Linux Foundation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * Changes from Qualcomm Innovation Center are provided under the following license:
 *
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <fcntl.h>
#include <display/drm/sde_drm.h>
#include <utils/debug.h>
#include <utils/sys.h>
#include <utils/rect.h>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

#include "hw_peripheral_drm.h"

#define __CLASS__ "HWPeripheralDRM"

using sde_drm::DRMDisplayType;
using sde_drm::DRMOps;
using sde_drm::DRMPowerMode;
using sde_drm::DppsFeaturePayload;
using sde_drm::DRMDppsFeatureInfo;
using sde_drm::DRMPanelFeatureID;
using sde_drm::DRMPanelFeatureInfo;
using sde_drm::DRMSecureMode;

namespace sdm {

HWPeripheralDRM::HWPeripheralDRM(int32_t display_id, BufferAllocator *buffer_allocator,
                                 HWInfoInterface *hw_info_intf)
  : HWDeviceDRM(buffer_allocator, hw_info_intf) {
  disp_type_ = DRMDisplayType::PERIPHERAL;
  device_name_ = "Peripheral";
  display_id_ = display_id;
}

DisplayError HWPeripheralDRM::Init() {
  DisplayError ret = HWDeviceDRM::Init();
  if (ret != kErrorNone) {
    DLOGE("Init failed for %s", device_name_);
    return ret;
  }

  InitDestScaler();

  PopulateBitClkRates();
  CreatePanelFeaturePropertyMap();

  return kErrorNone;
}

void HWPeripheralDRM::InitDestScaler() {
  if (hw_resource_.hw_dest_scalar_info.count) {
    // Do all destination scaler block resource allocations here.
    dest_scaler_blocks_used_ = 1;
    if (kQuadSplit == mixer_attributes_.split_type) {
      dest_scaler_blocks_used_ = 4;
    } else if (kDualSplit == mixer_attributes_.split_type) {
      dest_scaler_blocks_used_ = 2;
    }
    if (hw_resource_.hw_dest_scalar_info.count >=
        (hw_dest_scaler_blocks_used_ + dest_scaler_blocks_used_)) {
      // Enough destination scaler blocks available so update the static counter.
      hw_dest_scaler_blocks_used_ += dest_scaler_blocks_used_;
    } else {
      dest_scaler_blocks_used_ = 0;
    }
    scalar_data_.resize(dest_scaler_blocks_used_);
    dest_scalar_cache_.resize(dest_scaler_blocks_used_);
    // Update crtc (layer-mixer) configuration info.
    mixer_attributes_.dest_scaler_blocks_used = dest_scaler_blocks_used_;
  }

  topology_control_ = UINT32(sde_drm::DRMTopologyControl::DSPP);
  if (dest_scaler_blocks_used_) {
    topology_control_ |= UINT32(sde_drm::DRMTopologyControl::DEST_SCALER);
  }
}

void HWPeripheralDRM::PopulateBitClkRates() {
  if (!hw_panel_info_.dyn_bitclk_support) {
    return;
  }

  // Group all bit_clk_rates corresponding to DRM_PREFERRED mode.
  uint32_t width = connector_info_.modes[current_mode_index_].mode.hdisplay;
  uint32_t height = connector_info_.modes[current_mode_index_].mode.vdisplay;

  for (auto &mode_info : connector_info_.modes) {
    auto &mode = mode_info.mode;
    if (mode.hdisplay == width && mode.vdisplay == height) {
      for (auto &sub_mode_info : mode_info.sub_modes) {
        for (uint32_t index = 0; index < sub_mode_info.dyn_bitclk_list.size(); index++) {
          if (std::find(bitclk_rates_.begin(), bitclk_rates_.end(),
                sub_mode_info.dyn_bitclk_list[index]) == bitclk_rates_.end()) {
            bitclk_rates_.push_back(sub_mode_info.dyn_bitclk_list[index]);
            DLOGI("Possible bit_clk_rates %" PRIu64, sub_mode_info.dyn_bitclk_list[index]);
          }
        }
      }
    }
  }

  hw_panel_info_.bitclk_rates = bitclk_rates_;
  DLOGI("bit_clk_rates Size %zu", bitclk_rates_.size());
}

DisplayError HWPeripheralDRM::SetJitterConfig(uint32_t jitter_type, float value, uint32_t time) {
  return HWDeviceDRM::SetJitterConfig(jitter_type, value, time);
}

DisplayError HWPeripheralDRM::SetDynamicDSIClock(uint64_t bit_clk_rate) {
  if (last_power_mode_ == DRMPowerMode::DOZE_SUSPEND || last_power_mode_ == DRMPowerMode::OFF) {
    return kErrorNotSupported;
  }

  if (doze_poms_switch_done_ || pending_poms_switch_) {
    return kErrorNotSupported;
  }

  if (vrefresh_) {
    // vrefresh change pending.
    // Defer bit rate clock change.
    return kErrorNotSupported;
  }

  if (GetSupportedBitClkRate(current_mode_index_, bit_clk_rate) ==
      connector_info_.modes[current_mode_index_].curr_bit_clk_rate) {
    return kErrorNone;
  }

  bit_clk_rate_ = bit_clk_rate;
  return kErrorNone;
}

DisplayError HWPeripheralDRM::GetDynamicDSIClock(uint64_t *bit_clk_rate) {
  // Update bit_rate corresponding to current refresh rate.
  *bit_clk_rate = (uint32_t)connector_info_.modes[current_mode_index_].curr_bit_clk_rate;
  return kErrorNone;
}


DisplayError HWPeripheralDRM::SetRefreshRate(uint32_t refresh_rate) {
  if (doze_poms_switch_done_ || pending_poms_switch_) {
    // poms switch in progress
    // Defer any refresh rate setting.
    return kErrorNotSupported;
  }

  DisplayError error = HWDeviceDRM::SetRefreshRate(refresh_rate);
  if (error != kErrorNone) {
    return error;
  }

  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetDisplayMode(const HWDisplayMode hw_display_mode) {
  if (doze_poms_switch_done_ || pending_poms_switch_) {
    return kErrorNotSupported;
  }

  DisplayError error = HWDeviceDRM::SetDisplayMode(hw_display_mode);
  if (error != kErrorNone) {
    return error;
  }

  // update bit clk rates.
  hw_panel_info_.bitclk_rates = bitclk_rates_;

  return kErrorNone;
}

DisplayError HWPeripheralDRM::UpdateTransferTime(uint32_t transfer_time) {
  DisplayError error = HWDeviceDRM::UpdateTransferTime(transfer_time);

  return error;
}

DisplayError HWPeripheralDRM::Validate(HWLayersInfo *hw_layers_info) {
  SetDestScalarData(*hw_layers_info);
  SetIdlePCState();
  SetSelfRefreshState();
  SetVMReqState();

  return HWDeviceDRM::Validate(hw_layers_info);
}

DisplayError HWPeripheralDRM::Commit(HWLayersInfo *hw_layers_info) {
  SetDestScalarData(*hw_layers_info);

  int64_t cwb_fence_fd = -1;
  bool has_fence = SetupConcurrentWriteback(*hw_layers_info, false, &cwb_fence_fd);

  SetIdlePCState();
  SetSelfRefreshState();
  SetVMReqState();

  DisplayError error = HWDeviceDRM::Commit(hw_layers_info);
  shared_ptr<Fence> cwb_fence = Fence::Create(INT(cwb_fence_fd), "cwb_fence");
  if (error != kErrorNone) {
    return error;
  }

  if (has_fence) {
    hw_layers_info->output_buffer->release_fence = cwb_fence;
  }

  CacheDestScalarData();
  PostCommitConcurrentWriteback(hw_layers_info->output_buffer);

  // Initialize to default after successful commit
  synchronous_commit_ = false;
  active_ = true;

  if (pending_poms_switch_) {
    HWDeviceDRM::SetDisplayMode(kModeCommand);
    hw_panel_info_.bitclk_rates = bitclk_rates_;
    doze_poms_switch_done_ = true;
    pending_poms_switch_ = false;
  }

  idle_pc_state_ = sde_drm::DRMIdlePCState::NONE;
  // After commit, update the Self Refresh state
  if (self_refresh_state_ != kSelfRefreshNone) {
    if (self_refresh_state_ == kSelfRefreshReadAlloc) {
      self_refresh_state_ = kSelfRefreshDisableReadAlloc;
    } else if (self_refresh_state_ == kSelfRefreshDisableReadAlloc ||
               self_refresh_state_ == kSelfRefreshWriteAlloc) {
      self_refresh_state_ = kSelfRefreshNone;
    }
  }
  return error;
}

void HWPeripheralDRM::ResetDestScalarCache() {
  for (uint32_t j = 0; j < scalar_data_.size(); j++) {
    dest_scalar_cache_[j] = {};
  }
}

void HWPeripheralDRM::SetDestScalarData(const HWLayersInfo &hw_layer_info) {
  if (!hw_scale_ || !dest_scaler_blocks_used_) {
    return;
  }

  for (uint32_t i = 0; i < dest_scaler_blocks_used_; i++) {
    auto it = hw_layer_info.dest_scale_info_map.find(i);

    if (it == hw_layer_info.dest_scale_info_map.end()) {
      continue;
    }

    HWDestScaleInfo *dest_scale_info = it->second;
    SDEScaler *scale = &scalar_data_[i];
    hw_scale_->SetScaler(dest_scale_info->scale_data, scale);

    sde_drm_dest_scaler_cfg *dest_scalar_data = &sde_dest_scalar_data_.ds_cfg[i];
    dest_scalar_data->flags = 0;
    if (scale->scaler_v2.enable) {
      dest_scalar_data->flags |= SDE_DRM_DESTSCALER_ENABLE;
    }
    if (scale->scaler_v2.de.enable) {
      dest_scalar_data->flags |= SDE_DRM_DESTSCALER_ENHANCER_UPDATE;
    }
    if (dest_scale_info->scale_update) {
      dest_scalar_data->flags |= SDE_DRM_DESTSCALER_SCALE_UPDATE;
    }
    if (hw_panel_info_.partial_update) {
      dest_scalar_data->flags |= SDE_DRM_DESTSCALER_PU_ENABLE;
    }
    dest_scalar_data->index = i;
    dest_scalar_data->lm_width = dest_scale_info->mixer_width;
    dest_scalar_data->lm_height = dest_scale_info->mixer_height;
    dest_scalar_data->scaler_cfg = reinterpret_cast<uint64_t>(&scale->scaler_v2);

    if (std::memcmp(&dest_scalar_cache_[i].scalar_data, scale, sizeof(SDEScaler)) ||
        dest_scalar_cache_[i].flags != dest_scalar_data->flags) {
      needs_ds_update_ = true;
    }
  }

  if (needs_ds_update_) {
    sde_dest_scalar_data_.num_dest_scaler = UINT32(hw_layer_info.dest_scale_info_map.size());
    drm_atomic_intf_->Perform(DRMOps::CRTC_SET_DEST_SCALER_CONFIG, token_.crtc_id,
                              reinterpret_cast<uint64_t>(&sde_dest_scalar_data_));
  }
}

void HWPeripheralDRM::CacheDestScalarData() {
  if (needs_ds_update_) {
    // Cache the destination scalar data during commit
    for (uint32_t i = 0; i < sde_dest_scalar_data_.num_dest_scaler; i++) {
      dest_scalar_cache_[i].flags = sde_dest_scalar_data_.ds_cfg[i].flags;
      dest_scalar_cache_[i].scalar_data = scalar_data_[i];
    }
    needs_ds_update_ = false;
  }
}

void HWPeripheralDRM::SetSelfRefreshState() {
  if (self_refresh_state_ != kSelfRefreshNone) {
    if (self_refresh_state_ == kSelfRefreshReadAlloc) {
      drm_atomic_intf_->Perform(sde_drm::DRMOps::CRTC_SET_CACHE_STATE, token_.crtc_id,
                                sde_drm::DRMCacheState::ENABLED);
    } else if (self_refresh_state_ == kSelfRefreshWriteAlloc) {
      drm_atomic_intf_->Perform(sde_drm::DRMOps::CONNECTOR_SET_CACHE_STATE,
                                cwb_config_.token.conn_id, sde_drm::DRMCacheWBState::ENABLED);
    } else if (self_refresh_state_ == kSelfRefreshDisableReadAlloc) {
      drm_atomic_intf_->Perform(sde_drm::DRMOps::CRTC_SET_CACHE_STATE, token_.crtc_id,
                                sde_drm::DRMCacheState::DISABLED);
    }
  }
}

DisplayError HWPeripheralDRM::Flush(HWLayersInfo *hw_layers_info) {
  DisplayError err = HWDeviceDRM::Flush(hw_layers_info);
  if (err != kErrorNone) {
    return err;
  }

  ResetDestScalarCache();
  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetDppsFeature(void *payload, size_t size) {
  uint32_t obj_id = 0, object_type = 0, feature_id = 0;
  uint64_t value = 0;

  if (size != sizeof(DppsFeaturePayload)) {
    DLOGE("invalid payload size %zu, expected %zu", size, sizeof(DppsFeaturePayload));
    return kErrorParameters;
  }

  DppsFeaturePayload *feature_payload = reinterpret_cast<DppsFeaturePayload *>(payload);
  object_type = feature_payload->object_type;
  feature_id = feature_payload->feature_id;
  value = feature_payload->value;

  if (feature_id == sde_drm::kFeatureAd4Roi) {
    if (feature_payload->value) {
      DisplayDppsAd4RoiCfg *params = reinterpret_cast<DisplayDppsAd4RoiCfg *>
                                                      (feature_payload->value);
      if (!params) {
        DLOGE("invalid playload value %" PRIu64, feature_payload->value);
        return kErrorNotSupported;
      }

      ad4_roi_cfg_.h_x = params->h_start;
      ad4_roi_cfg_.h_y = params->h_end;
      ad4_roi_cfg_.v_x = params->v_start;
      ad4_roi_cfg_.v_y = params->v_end;
      ad4_roi_cfg_.factor_in = params->factor_in;
      ad4_roi_cfg_.factor_out = params->factor_out;

      value = (uint64_t)&ad4_roi_cfg_;
    }
  }

  if (feature_id == sde_drm::kFeatureLtmHistCtrl)
    ltm_hist_en_ = value;

  if (feature_id == sde_drm::kFeatureAbaHistCtrl)
    aba_hist_en_ = value;

  if (object_type == DRM_MODE_OBJECT_CRTC) {
    obj_id = token_.crtc_id;
  } else if (object_type == DRM_MODE_OBJECT_CONNECTOR) {
    obj_id = token_.conn_id;
  } else {
    DLOGE("invalid object type 0x%x", object_type);
    return kErrorUndefined;
  }

  drm_atomic_intf_->Perform(DRMOps::DPPS_CACHE_FEATURE, obj_id, feature_id, value);
  return kErrorNone;
}

DisplayError HWPeripheralDRM::GetDppsFeatureInfo(void *payload, size_t size) {
  if (size != sizeof(DRMDppsFeatureInfo)) {
    DLOGE("invalid payload size %zu, expected %zu", size, sizeof(DRMDppsFeatureInfo));
    return kErrorParameters;
  }
  DRMDppsFeatureInfo *feature_info = reinterpret_cast<DRMDppsFeatureInfo *>(payload);
  feature_info->obj_id = token_.crtc_id;
  drm_mgr_intf_->GetDppsFeatureInfo(feature_info);
  return kErrorNone;
}

DisplayError HWPeripheralDRM::HandleSecureEvent(SecureEvent secure_event,
                                                const HWQosData &qos_data) {
  switch (secure_event) {
    case kTUITransitionPrepare:
    case kTUITransitionUnPrepare:
      tui_state_ = kTUIStateInProgress;
      break;
    case kTUITransitionStart: {
      tui_state_ = kTUIStateStart;
      ControlIdlePowerCollapse(false /* enable */, false /* synchronous */);
      if (hw_panel_info_.mode != kModeCommand) {
        SetQOSData(qos_data);
        SetVMReqState();
        SetIdlePCState();
        DisplayError err = Flush(NULL);
        if (err != kErrorNone) {
          return err;
        }
        SetTUIState();
      }
    }
    break;

    case kTUITransitionEnd: {
      tui_state_ = kTUIStateEnd;
      ResetPropertyCache();
      ControlIdlePowerCollapse(true /* enable */, false /* synchronous */);
      if (hw_panel_info_.mode != kModeCommand || pending_power_state_ == kPowerStateOff) {
        SetQOSData(qos_data);
        SetVMReqState();
        SetIdlePCState();
        DisplayError err = Flush(NULL);
        if (err != kErrorNone) {
          return err;
        }
        SetTUIState();
      }
    }
    break;

    case kSecureDisplayStart: {
      secure_display_active_ = true;
      if (hw_panel_info_.mode != kModeCommand) {
        DisplayError err = Flush(NULL);
        if (err != kErrorNone) {
          return err;
        }
      }
    }
    break;

    case kSecureDisplayEnd: {
      if (hw_panel_info_.mode != kModeCommand) {
        DisplayError err = Flush(NULL);
        if (err != kErrorNone) {
          return err;
        }
      }
      secure_display_active_ = false;
      synchronous_commit_ = true;
    }
    break;

    default:
      DLOGE("Invalid secure event %d", secure_event);
      return kErrorNotSupported;
  }

  return kErrorNone;
}

DisplayError HWPeripheralDRM::ControlIdlePowerCollapse(bool enable, bool synchronous) {
  if (enable == idle_pc_enabled_) {
    return kErrorNone;
  }
  idle_pc_state_ = enable ? sde_drm::DRMIdlePCState::ENABLE : sde_drm::DRMIdlePCState::DISABLE;
  idle_pc_enabled_ = enable;
  return kErrorNone;
}

DisplayError HWPeripheralDRM::PowerOn(const HWQosData &qos_data, SyncPoints *sync_points) {
  DTRACE_SCOPED();
  if (!drm_atomic_intf_) {
    DLOGE("DRM Atomic Interface is null!");
    return kErrorUndefined;
  }

  if (first_cycle_ || tui_state_ != kTUIStateNone) {
    DLOGI("Request deferred TUI state %d", tui_state_);
    pending_power_state_ = kPowerStateOn;
    return kErrorDeferred;
  }
  SetVMReqState();

  if (switch_mode_valid_ && doze_poms_switch_done_ && (current_mode_index_ == cmd_mode_index_)) {
    HWDeviceDRM::SetDisplayMode(kModeVideo);
    hw_panel_info_.bitclk_rates = bitclk_rates_;
    doze_poms_switch_done_ = false;
  }

  if (!idle_pc_enabled_) {
    drm_atomic_intf_->Perform(sde_drm::DRMOps::CRTC_SET_IDLE_PC_STATE, token_.crtc_id,
                              sde_drm::DRMIdlePCState::ENABLE);
  }

  if (sde_dest_scalar_data_.num_dest_scaler) {
    drm_atomic_intf_->Perform(DRMOps::CRTC_SET_DEST_SCALER_CONFIG, token_.crtc_id,
                              reinterpret_cast<uint64_t>(&sde_dest_scalar_data_));
    needs_ds_update_ = true;
  }

  DisplayError err = HWDeviceDRM::PowerOn(qos_data, sync_points);
  if (err != kErrorNone) {
    return err;
  }
  idle_pc_state_ = sde_drm::DRMIdlePCState::NONE;
  idle_pc_enabled_ = true;
  pending_poms_switch_ = false;
  active_ = true;
  SetTUIState();

  CacheDestScalarData();

  return kErrorNone;
}

DisplayError HWPeripheralDRM::PowerOff(bool teardown, SyncPoints *sync_points) {
  DTRACE_SCOPED();
  if (!first_cycle_) {
    drm_mgr_intf_->MarkPanelFeatureForNullCommit(token_,
                                           panel_feature_property_map_[kPanelFeatureDemuraInitCfg]);
  }
  SetVMReqState();
  DisplayError err = kErrorNone;
  if (secure_display_active_) {
    err = Flush(NULL);
    if (err != kErrorNone) {
      return err;
    }
  }

  // QSync mode needs to be reset on device suspend and re-enabled on resume.
  drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_QSYNC_MODE, token_.conn_id,
                            sde_drm::DRMQsyncMode::NONE);

  err = HWDeviceDRM::PowerOff(teardown, sync_points);
  if (err != kErrorNone) {
    return err;
  }

  pending_poms_switch_ = false;
  active_ = false;
  SetTUIState();

  return kErrorNone;
}

DisplayError HWPeripheralDRM::Doze(const HWQosData &qos_data, SyncPoints *sync_points) {
  DTRACE_SCOPED();
  SetVMReqState();

  if (!first_cycle_ && switch_mode_valid_ && !doze_poms_switch_done_ &&
    (current_mode_index_ == video_mode_index_)) {
    if (active_) {
      HWDeviceDRM::SetDisplayMode(kModeCommand);
      hw_panel_info_.bitclk_rates = bitclk_rates_;
      doze_poms_switch_done_ = true;
    } else {
      pending_poms_switch_ = true;
    }
  }
  DisplayError err = HWDeviceDRM::Doze(qos_data, sync_points);
  if (err != kErrorNone) {
    return err;
  }

  active_ = true;

  SetTUIState();
  return kErrorNone;
}

DisplayError HWPeripheralDRM::DozeSuspend(const HWQosData &qos_data, SyncPoints *sync_points) {
  SetVMReqState();

  if (switch_mode_valid_ && !doze_poms_switch_done_ &&
    (current_mode_index_ == video_mode_index_)) {
    HWDeviceDRM::SetDisplayMode(kModeCommand);
    hw_panel_info_.bitclk_rates = bitclk_rates_;
    doze_poms_switch_done_ = true;
  }

  DisplayError err = HWDeviceDRM::DozeSuspend(qos_data, sync_points);
  if (err != kErrorNone) {
    return err;
  }

  pending_poms_switch_ = false;
  active_ = true;

  SetTUIState();
  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetDisplayAttributes(uint32_t index) {
  if (doze_poms_switch_done_ || pending_poms_switch_ || bit_clk_rate_) {
    DLOGW("Bailing. Pending operations: doze_poms_switch_done_=%d, pending_poms_switch_=%d,"
     "bit_clk_rate_=%d", doze_poms_switch_done_, pending_poms_switch_, bit_clk_rate_);
    return kErrorDeferred;
  }

  HWDeviceDRM::SetDisplayAttributes(index);
  // update bit clk rates.
  hw_panel_info_.bitclk_rates = bitclk_rates_;

  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetDisplayDppsAdROI(void *payload) {
  DisplayError err = kErrorNone;
  struct sde_drm::DppsFeaturePayload feature_payload = {};

  if (!payload) {
    DLOGE("Invalid payload parameter");
    return kErrorParameters;
  }

  feature_payload.object_type = DRM_MODE_OBJECT_CRTC;
  feature_payload.feature_id = sde_drm::kFeatureAd4Roi;
  feature_payload.value = (uint64_t)(payload);

  err = SetDppsFeature(&feature_payload, sizeof(feature_payload));
  if (err != kErrorNone) {
    DLOGE("Faid to SetDppsFeature feature_id = %d, err = %d",
           sde_drm::kFeatureAd4Roi, err);
  }

  return err;
}

DisplayError HWPeripheralDRM::SetFrameTrigger(FrameTriggerMode mode) {
  sde_drm::DRMFrameTriggerMode drm_mode = sde_drm::DRMFrameTriggerMode::FRAME_DONE_WAIT_DEFAULT;
  switch (mode) {
  case kFrameTriggerDefault:
    drm_mode = sde_drm::DRMFrameTriggerMode::FRAME_DONE_WAIT_DEFAULT;
    break;
  case kFrameTriggerSerialize:
    drm_mode = sde_drm::DRMFrameTriggerMode::FRAME_DONE_WAIT_SERIALIZE;
    break;
  case kFrameTriggerPostedStart:
    drm_mode = sde_drm::DRMFrameTriggerMode::FRAME_DONE_WAIT_POSTED_START;
    break;
  default:
    DLOGE("Invalid frame trigger mode %d", (int32_t)mode);
    return kErrorParameters;
  }

  int ret = drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_FRAME_TRIGGER,
                                      token_.conn_id, drm_mode);
  if (ret) {
    DLOGE("Failed to perform CONNECTOR_SET_FRAME_TRIGGER, drm_mode %d, ret %d", drm_mode, ret);
    return kErrorUndefined;
  }
  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetPanelBrightness(int level) {
  DTRACE_SCOPED();
  if (pending_power_state_ != kPowerStateNone) {
    DLOGI("Power state %d pending!! Skip for now", pending_power_state_);
    return kErrorDeferred;
  }

#ifdef TRUSTED_VM
  if (first_cycle_) {
    DLOGI("First cycle is not done yet!! Skip for now");
    return kErrorDeferred;
  }
#endif

  char buffer[kMaxSysfsCommandLength] = {0};

  if (brightness_base_path_.empty()) {
    return kErrorHardware;
  }
  if (!active_) {
    return kErrorNone;
  }

  std::string brightness_node(brightness_base_path_ + "brightness");
  int fd = Sys::open_(brightness_node.c_str(), O_RDWR);
#ifdef TRUSTED_VM
  if (connector_info_.backlight_type != "dcs") {
    DLOGW("Failed to open node = %s, error = %s ", brightness_node.c_str(),
          strerror(errno));
    return kErrorFileDescriptor;
  }
#endif
  if (fd < 0) {
    DLOGE("Failed to open node = %s, error = %s ", brightness_node.c_str(),
          strerror(errno));
    return kErrorFileDescriptor;
  }

  int32_t bytes = snprintf(buffer, kMaxSysfsCommandLength, "%d\n", level);
  ssize_t ret = Sys::pwrite_(fd, buffer, static_cast<size_t>(bytes), 0);
  if (ret <= 0) {
    DLOGE("Failed to write to node = %s, error = %s ", brightness_node.c_str(),
          strerror(errno));
    Sys::close_(fd);
    return kErrorHardware;
  }

  Sys::close_(fd);

  return kErrorNone;
}

DisplayError HWPeripheralDRM::GetPanelBrightness(int *level) {
  DTRACE_SCOPED();
  char value[kMaxStringLength] = {0};

  if (!level) {
    DLOGE("Invalid input, null pointer.");
    return kErrorParameters;
  }

  if (brightness_base_path_.empty()) {
    return kErrorHardware;
  }

  std::string brightness_node(brightness_base_path_ + "brightness");
  int fd = Sys::open_(brightness_node.c_str(), O_RDWR);
#ifdef TRUSTED_VM
  if (connector_info_.backlight_type != "dcs") {
    DLOGW("Failed to open brightness node = %s, error = %s", brightness_node.c_str(),
           strerror(errno));
    return kErrorFileDescriptor;
  }
#endif
  if (fd < 0) {
    DLOGE("Failed to open brightness node = %s, error = %s", brightness_node.c_str(),
           strerror(errno));
    return kErrorFileDescriptor;
  }

  if (Sys::pread_(fd, value, sizeof(value), 0) > 0) {
    *level = atoi(value);
  } else {
    DLOGE("Failed to read panel brightness");
    Sys::close_(fd);
    return kErrorHardware;
  }

  Sys::close_(fd);

  return kErrorNone;
}

void HWPeripheralDRM::GetHWPanelMaxBrightness() {
  DTRACE_SCOPED();
  char value[kMaxStringLength] = {0};
  hw_panel_info_.panel_max_brightness = 255.0f;

  // Panel nodes, driver connector creation, and DSI probing all occur in sync, for each DSI. This
  // means that the connector_type_id - 1 will reflect the same # as the panel # for panel node.
  char s[kMaxStringLength] = {};
  snprintf(s, sizeof(s), "/sys/class/backlight/panel%d-backlight/",
           static_cast<int>(connector_info_.type_id - 1));
  brightness_base_path_.assign(s);

  std::string brightness_node(brightness_base_path_ + "max_brightness");
  int fd = Sys::open_(brightness_node.c_str(), O_RDONLY);
#ifdef TRUSTED_VM
  if (connector_info_.backlight_type != "dcs") {
    DLOGW("Failed to open max brightness node = %s, error = %s", brightness_node.c_str(),
          strerror(errno));
    return;
  }
#endif
  if (fd < 0) {
    DLOGE("Failed to open max brightness node = %s, error = %s", brightness_node.c_str(),
          strerror(errno));
    return;
  }

  if (Sys::pread_(fd, value, sizeof(value), 0) > 0) {
    hw_panel_info_.panel_max_brightness = static_cast<float>(atof(value));
    DLOGI_IF(kTagDriverConfig, "Max brightness = %f", hw_panel_info_.panel_max_brightness);
  } else {
    DLOGE("Failed to read max brightness. error = %s", strerror(errno));
  }

  Sys::close_(fd);
  return;
}

DisplayError HWPeripheralDRM::SetBLScale(uint32_t level) {
  int ret = drm_atomic_intf_->Perform(DRMOps::DPPS_CACHE_FEATURE,
              token_.conn_id, sde_drm::kFeatureSvBlScale, level);
  if (ret) {
    DLOGE("Failed to set backlight scale level %d, ret %d", level, ret);
    return kErrorUndefined;
  }
  return kErrorNone;
}

DisplayError HWPeripheralDRM::GetPanelBrightnessBasePath(std::string *base_path) const {
  if (!base_path) {
    DLOGE("Invalid base_path is null pointer");
    return kErrorParameters;
  }

  if (brightness_base_path_.empty()) {
    DLOGE("brightness_base_path_ is empty");
    return kErrorHardware;
  }

  *base_path = brightness_base_path_;
  return kErrorNone;
}

DisplayError HWPeripheralDRM::EnableSelfRefresh(SelfRefreshState self_refresh_state) {
  if (self_refresh_state != kSelfRefreshNone) {
    self_refresh_state_ = self_refresh_state;
  }
  return kErrorNone;
}

void HWPeripheralDRM::ResetPropertyCache() {
  drm_atomic_intf_->Perform(sde_drm::DRMOps::PLANES_RESET_CACHE, token_.crtc_id);
  drm_atomic_intf_->Perform(sde_drm::DRMOps::CRTC_RESET_CACHE, token_.crtc_id);
}

void HWPeripheralDRM::CreatePanelFeaturePropertyMap() {
  panel_feature_property_map_.clear();
  panel_feature_property_map_[kPanelFeatureSPRInitCfg] = sde_drm::kDRMPanelFeatureSPRInit;
  panel_feature_property_map_[kPanelFeatureSPRPackType] = sde_drm::kDRMPanelFeatureSPRPackType;
  panel_feature_property_map_[kPanelFeatureDemuraInitCfg] = sde_drm::kDRMPanelFeatureDemuraInit;
  panel_feature_property_map_[kPanelFeatureDsppIndex] = sde_drm::kDRMPanelFeatureDsppIndex;
  panel_feature_property_map_[kPanelFeatureDsppSPRInfo] = sde_drm::kDRMPanelFeatureDsppSPRInfo;
  panel_feature_property_map_[kPanelFeatureDsppRCInfo] = sde_drm::kDRMPanelFeatureDsppRCInfo;
  panel_feature_property_map_[kPanelFeatureDsppDemuraInfo] =
    sde_drm::kDRMPanelFeatureDsppDemuraInfo;
  panel_feature_property_map_[kPanelFeatureRCInitCfg] = sde_drm::kDRMPanelFeatureRCInit;
  panel_feature_property_map_[kPanelFeatureDemuraPanelId] = sde_drm::kDRMPanelFeaturePanelId;
}

int HWPeripheralDRM::GetPanelFeature(PanelFeaturePropertyInfo *feature_info) {
  int ret = 0;
  DRMPanelFeatureInfo drm_feature = {};

  if (!feature_info) {
    DLOGE("Invalid object pointer of PanelFeaturePropertyInfo");
    return -EINVAL;
  }

  auto it = panel_feature_property_map_.find(feature_info->prop_id);
  if (it ==  panel_feature_property_map_.end()) {
    DLOGE("Failed to find prop-map entry for id %d", feature_info->prop_id);
    return -EINVAL;
  }

  drm_feature.prop_id = panel_feature_property_map_[feature_info->prop_id];
  drm_feature.prop_ptr = feature_info->prop_ptr;
  drm_feature.prop_size = feature_info->prop_size;

  switch (feature_info->prop_id) {
    case kPanelFeatureSPRInitCfg:
    case kPanelFeatureDemuraInitCfg:
    case kPanelFeatureDsppIndex:
    case kPanelFeatureDsppSPRInfo:
    case kPanelFeatureDsppDemuraInfo:
    case kPanelFeatureDsppRCInfo:
    case kPanelFeatureRCInitCfg:
      drm_feature.obj_type = DRM_MODE_OBJECT_CRTC;
      drm_feature.obj_id =  token_.crtc_id;
     break;
    case kPanelFeatureSPRPackType:
    case kPanelFeatureDemuraPanelId:
      drm_feature.obj_type = DRM_MODE_OBJECT_CONNECTOR;
      drm_feature.obj_id =  token_.conn_id;
     break;
    default:
     DLOGE("obj id population for property %d not implemented", feature_info->prop_id);
     return -EINVAL;
  }

  drm_mgr_intf_->GetPanelFeature(&drm_feature);

  feature_info->version = drm_feature.version;
  feature_info->prop_size = drm_feature.prop_size;

  return ret;
}

int HWPeripheralDRM::SetPanelFeature(const PanelFeaturePropertyInfo &feature_info) {
  int ret = 0;
  DRMPanelFeatureInfo drm_feature = {};
  drm_feature.prop_id = panel_feature_property_map_[feature_info.prop_id];
  drm_feature.prop_ptr = feature_info.prop_ptr;
  drm_feature.version = feature_info.version;
  drm_feature.prop_size = feature_info.prop_size;

  switch (feature_info.prop_id) {
    case kPanelFeatureSPRInitCfg:
    case kPanelFeatureRCInitCfg:
    case kPanelFeatureDemuraInitCfg:
      drm_feature.obj_type = DRM_MODE_OBJECT_CRTC;
      drm_feature.obj_id =  token_.crtc_id;
     break;
    case kPanelFeatureSPRPackType:
      drm_feature.obj_type = DRM_MODE_OBJECT_CONNECTOR;
      drm_feature.obj_id =  token_.conn_id;
     break;
    default:
     DLOGE("Set Panel feature property %d not implemented", feature_info.prop_id);
     return -EINVAL;
  }

  DLOGI("Set Panel feature property %d", feature_info.prop_id);
  drm_mgr_intf_->SetPanelFeature(drm_feature);

  return ret;
}

void HWPeripheralDRM::SetVMReqState() {
  if (tui_state_ == kTUIStateStart) {
    drm_atomic_intf_->Perform(sde_drm::DRMOps::CRTC_SET_VM_REQ_STATE, token_.crtc_id,
                              sde_drm::DRMVMRequestState::RELEASE);
    DLOGI("Release resources to SVM");
    if (ltm_hist_en_)
      drm_atomic_intf_->Perform(sde_drm::DRMOps::DPPS_CACHE_FEATURE, token_.crtc_id,
                                sde_drm::kFeatureLtmHistCtrl, 0);
    if (aba_hist_en_)
      drm_atomic_intf_->Perform(sde_drm::DRMOps::DPPS_CACHE_FEATURE, token_.crtc_id,
                                sde_drm::kFeatureAbaHistCtrl, 0);
  } else if (tui_state_ == kTUIStateEnd) {
    drm_atomic_intf_->Perform(sde_drm::DRMOps::CRTC_SET_VM_REQ_STATE, token_.crtc_id,
                              sde_drm::DRMVMRequestState::ACQUIRE);
    DLOGI("Acquire resources from SVM");
    if (ltm_hist_en_)
      drm_atomic_intf_->Perform(sde_drm::DRMOps::DPPS_CACHE_FEATURE, token_.crtc_id,
                                sde_drm::kFeatureLtmHistCtrl, 1);
    if (aba_hist_en_)
      drm_atomic_intf_->Perform(sde_drm::DRMOps::DPPS_CACHE_FEATURE, token_.crtc_id,
                                sde_drm::kFeatureAbaHistCtrl, 1);
  } else if (tui_state_ == kTUIStateNone) {
    drm_atomic_intf_->Perform(sde_drm::DRMOps::CRTC_SET_VM_REQ_STATE, token_.crtc_id,
                              sde_drm::DRMVMRequestState::NONE);
  }
}

DisplayError HWPeripheralDRM::SetAlternateDisplayConfig(uint32_t *alt_config) {
  uint32_t curr_mode_flag = 0;
  sde_drm::DRMModeInfo current_mode = connector_info_.modes[current_mode_index_];
  sde_drm::DRMSubModeInfo sub_mode = current_mode.sub_modes[current_mode.curr_submode_index];
  uint32_t curr_compression = current_mode.curr_compression_mode;

  if (current_mode.cur_panel_mode & DRM_MODE_FLAG_CMD_MODE_PANEL) {
    curr_mode_flag = DRM_MODE_FLAG_CMD_MODE_PANEL;
  } else if (current_mode.cur_panel_mode & DRM_MODE_FLAG_VID_MODE_PANEL) {
    curr_mode_flag = DRM_MODE_FLAG_VID_MODE_PANEL;
  }

  // First try to perform compression mode switch within same mode
  for (uint32_t submode_idx = 0; submode_idx < current_mode.sub_modes.size(); submode_idx++) {
    if ((curr_compression != current_mode.sub_modes[submode_idx].panel_compression_mode)) {
      connector_info_.modes[current_mode_index_].curr_submode_index = submode_idx;
      connector_info_.modes[current_mode_index_].curr_compression_mode =
              current_mode.sub_modes[submode_idx].panel_compression_mode;
      SetTopology(connector_info_.modes[current_mode_index_].sub_modes[submode_idx].topology,
                  &display_attributes_[current_mode_index_].topology);
      SetDisplaySwitchMode(current_mode_index_);
      panel_compression_changed_ = current_mode.sub_modes[submode_idx].panel_compression_mode;
      *alt_config = current_mode_index_;
      return kErrorNone;
    }
  }

  // If there is no compression switch possible within current mode, try with other modes
  for (uint32_t mode_index = 0; mode_index < connector_info_.modes.size(); mode_index++) {
    if ((current_mode.mode.vrefresh == connector_info_.modes[mode_index].mode.vrefresh) &&
        (curr_mode_flag & connector_info_.modes[mode_index].cur_panel_mode)) {
      for (uint32_t submode_idx = 0; submode_idx <
           connector_info_.modes[mode_index].sub_modes.size(); submode_idx++) {
        if ((curr_compression !=
             connector_info_.modes[mode_index].sub_modes[submode_idx].panel_compression_mode)) {
          connector_info_.modes[mode_index].curr_submode_index = submode_idx;
          SetTopology(connector_info_.modes[mode_index].sub_modes[submode_idx].topology,
                      &display_attributes_[mode_index].topology);
          connector_info_.modes[mode_index].curr_compression_mode =
                connector_info_.modes[mode_index].sub_modes[submode_idx].panel_compression_mode;
          SetDisplayAttributes(mode_index);
          panel_compression_changed_ = connector_info_.modes[mode_index].curr_compression_mode;
          *alt_config = mode_index;
          return kErrorNone;
        }
      }
    }
  }

  return kErrorNotSupported;
}

DisplayError HWPeripheralDRM::GetQsyncFps(uint32_t *qsync_fps) {
  uint32_t qsync_min_fps = connector_info_.modes[current_mode_index_].qsync_min_fps;
  if (qsync_min_fps > 0) {
    *qsync_fps = qsync_min_fps;
    return kErrorNone;
  }

  return kErrorNotSupported;
}

}  // namespace sdm
