// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cdm_adapter.h"
#include <chrono>

#define DCHECK(condition) assert(condition)

#include "../base/limits.h"

namespace media {

uint64_t gtc()
{
#ifdef OS_WIN
	return GetTickCount64();
#else
	struct timespec tp;
	clock_gettime(CLOCK_REALTIME, &tp);
	return  tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
#endif
}

namespace {

static void* GetCdmHost(int host_interface_version, void* user_data)
{
	if (host_interface_version != cdm::Host_8::kVersion || !user_data)
		return nullptr;
	CdmAdapter* cdm_adapter = static_cast<CdmAdapter*>(user_data);

	return static_cast<cdm::Host_8*>(cdm_adapter);
}

// Returns a pointer to the requested CDM upon success.
// Returns NULL if an error occurs or the requested |cdm_interface_version| or
// |key_system| is not supported or another error occurs.
// The caller should cast the returned pointer to the type matching
// |cdm_interface_version|.
// Caller retains ownership of arguments and must call Destroy() on the returned
// object.
typedef void* (*CreateCdmFunc)(int cdm_interface_version,
	const char* key_system,
	uint32_t key_system_size,
	GetCdmHostFunc get_cdm_host_func,
	void* user_data);
}  // namespace

/*******************************         CdmAdapter        ****************************************/


CdmAdapter::CdmAdapter(
    const std::string& key_system,
	const std::string& cdm_path,
    const CdmConfig& cdm_config)
: key_system_(key_system)
,cdm_config_(cdm_config)
, cdm_(0)
, library_(0)
, active_buffer_(0)
, timer_expired_(0)
, timer_context_(0)
{
  //DCHECK(!key_system_.empty());
  Initialize(cdm_path);
}

CdmAdapter::~CdmAdapter() 
{
	if (!cdm_)
		return;
	cdm_->Destroy();
	cdm_ = 0;
	base::UnloadNativeLibrary(library_);
}

void CdmAdapter::Initialize(const std::string& cdm_path)
{
	if (cdm_)
		cdm_->Destroy();

	base::NativeLibraryLoadError error;
	base::UnloadNativeLibrary(library_);
#if defined(OS_WIN)	
	library_ = base::LoadNativeLibraryDynamically(cdm_path);
#else
	library_ = base::LoadNativeLibrary(cdm_path, 0);
#endif
	if (!library_)
		return;

	CreateCdmFunc create_cdm_func = reinterpret_cast<CreateCdmFunc>(base::GetFunctionPointerFromNativeLibrary(library_, "CreateCdmInstance"));
	if (!create_cdm_func)
	{
		base::UnloadNativeLibrary(library_);
		library_ = 0;
		return;
	}

	cdm_ = reinterpret_cast<cdm::ContentDecryptionModule*>(create_cdm_func(cdm::ContentDecryptionModule_8::kVersion, key_system_.data(), key_system_.size(), GetCdmHost, this));
  
	if (cdm_)
	{
		cdm_->Initialize(cdm_config_.allow_distinctive_identifier,
			cdm_config_.allow_persistent_state);
	}
	else
	{
		base::UnloadNativeLibrary(library_);
		library_ = 0;
	}
}

void CdmAdapter::SetServerCertificate(uint32_t promise_id,
	const uint8_t* server_certificate_data,
	uint32_t server_certificate_data_size)
{
	if (server_certificate_data_size < limits::kMinCertificateLength ||
		server_certificate_data_size > limits::kMaxCertificateLength) {
    return;
  }
	cdm_->SetServerCertificate(promise_id, server_certificate_data,
		server_certificate_data_size);
}

void CdmAdapter::CreateSessionAndGenerateRequest(uint32_t promise_id,
	cdm::SessionType session_type,
	cdm::InitDataType init_data_type,
	const uint8_t* init_data,
	uint32_t init_data_size)
{
  cdm_->CreateSessionAndGenerateRequest(
      promise_id, session_type,
      init_data_type, init_data,
      init_data_size);
}

void CdmAdapter::LoadSession(uint32_t promise_id,
	cdm::SessionType session_type,
	const char* session_id,
	uint32_t session_id_size)
{
  cdm_->LoadSession(promise_id, session_type,
                    session_id, session_id_size);
}

void CdmAdapter::UpdateSession(uint32_t promise_id,
	const char* session_id,
	uint32_t session_id_size,
	const uint8_t* response,
	uint32_t response_size)
{
	license_ = std::string((const char*)response, response_size);
	cdm_->UpdateSession(promise_id, session_id, session_id_size,
                      response, response_size);
}

void CdmAdapter::UpdateSession(const uint8_t* response, uint32_t response_size)
{
	UpdateSession(2, session_id_.c_str(), session_id_.size(), response, response_size);
}

void CdmAdapter::UpdateSession()
{
	UpdateSession((const uint8_t*)license_.c_str(), license_.size());
}

void CdmAdapter::CloseSession(uint32_t promise_id,
	const char* session_id,
	uint32_t session_id_size)
{
  cdm_->CloseSession(promise_id, session_id, session_id_size);
}

void CdmAdapter::RemoveSession(uint32_t promise_id,
	const char* session_id,
	uint32_t session_id_size)
{
  cdm_->RemoveSession(promise_id, session_id, session_id_size);
}

void CdmAdapter::TimerExpired(void* context)
{ 
	cdm_->TimerExpired(context);
}

cdm::Status CdmAdapter::Decrypt(const cdm::InputBuffer& encrypted_buffer,
	cdm::DecryptedBlock* decrypted_buffer)
{
	if (timer_expired_ && gtc() > timer_expired_)
	{
		timer_expired_ = 0;
		TimerExpired(timer_context_);
	}
  //Sleep(10);
  active_buffer_ = decrypted_buffer->DecryptedBuffer();
	cdm::Status ret = cdm_->Decrypt(encrypted_buffer, decrypted_buffer);
	active_buffer_ = 0;
	return ret;
}

cdm::Status CdmAdapter::InitializeAudioDecoder(
	const cdm::AudioDecoderConfig& audio_decoder_config)
{
	return cdm_->InitializeAudioDecoder(audio_decoder_config);
}

cdm::Status CdmAdapter::InitializeVideoDecoder(
	const cdm::VideoDecoderConfig& video_decoder_config)
{
	return cdm_->InitializeVideoDecoder(video_decoder_config);
}

void CdmAdapter::DeinitializeDecoder(cdm::StreamType decoder_type)
{
	cdm_->DeinitializeDecoder(decoder_type);
}

void CdmAdapter::ResetDecoder(cdm::StreamType decoder_type)
{
	cdm_->ResetDecoder(decoder_type);
}

cdm::Status CdmAdapter::DecryptAndDecodeFrame(const cdm::InputBuffer& encrypted_buffer,
	cdm::VideoFrame* video_frame)
{
	return cdm_->DecryptAndDecodeFrame(encrypted_buffer, video_frame);
}

cdm::Status CdmAdapter::DecryptAndDecodeSamples(const cdm::InputBuffer& encrypted_buffer,
	cdm::AudioFrames* audio_frames)
{
	return cdm_->DecryptAndDecodeSamples(encrypted_buffer, audio_frames);
}

void CdmAdapter::OnPlatformChallengeResponse(
	const cdm::PlatformChallengeResponse& response)
{
	cdm_->OnPlatformChallengeResponse(response);
}

void CdmAdapter::OnQueryOutputProtectionStatus(cdm::QueryResult result,
	uint32_t link_mask,
	uint32_t output_protection_mask)
{
	cdm_->OnQueryOutputProtectionStatus(result, link_mask,
		output_protection_mask);
}

/******************************** HOST *****************************************/

cdm::Buffer* CdmAdapter::Allocate(uint32_t capacity)
{
	return active_buffer_;
}

void CdmAdapter::SetTimer(int64_t delay_ms, void* context)
{
	//timer_context_ = context;
	//timer_expired_ = gtc() + delay_ms;
}

cdm::Time CdmAdapter::GetCurrentWallTime()
{
	cdm::Time res = static_cast<cdm::Time>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
	return res / 1000.0;
}

void CdmAdapter::OnResolvePromise(uint32_t promise_id)
{
}

void CdmAdapter::OnResolveNewSessionPromise(uint32_t promise_id,
                                            const char* session_id,
                                            uint32_t session_id_size) 
{
}

void CdmAdapter::OnRejectPromise(uint32_t promise_id,
                                 cdm::Error error,
                                 uint32_t system_code,
                                 const char* error_message,
                                 uint32_t error_message_size)
{
}

void CdmAdapter::OnSessionMessage(const char* session_id,
                                  uint32_t session_id_size,
                                  cdm::MessageType message_type,
                                  const char* message,
                                  uint32_t message_size,
                                  const char* legacy_destination_url,
                                  uint32_t legacy_destination_url_size)
{
	session_id_ = std::string(session_id, session_id_size);
	message_ = std::string(message, message_size);
	message_type_ = message_type;
}

void CdmAdapter::OnSessionKeysChange(const char* session_id,
                                     uint32_t session_id_size,
                                     bool has_additional_usable_key,
                                     const cdm::KeyInformation* keys_info,
                                     uint32_t keys_info_count)
{
	for (uint32_t i(0); i < keys_info_count; ++i)
		if (keys_info[i].status == cdm::kUsable)
			usable_key_id_ = std::string(reinterpret_cast<const char*>(keys_info[i].key_id), keys_info[i].key_id_size);
}

void CdmAdapter::OnExpirationChange(const char* session_id,
                                    uint32_t session_id_size,
                                    cdm::Time new_expiry_time)
{
}

void CdmAdapter::OnSessionClosed(const char* session_id,
                                 uint32_t session_id_size)
{
}

void CdmAdapter::OnLegacySessionError(const char* session_id,
                                      uint32_t session_id_size,
                                      cdm::Error error,
                                      uint32_t system_code,
                                      const char* error_message,
                                      uint32_t error_message_size)
{
}

void CdmAdapter::SendPlatformChallenge(const char* service_id,
                                       uint32_t service_id_size,
                                       const char* challenge,
                                       uint32_t challenge_size)
{
}

void CdmAdapter::EnableOutputProtection(uint32_t desired_protection_mask)
{
  QueryOutputProtectionStatus();
}

void CdmAdapter::QueryOutputProtectionStatus()
{
	cdm_->OnQueryOutputProtectionStatus(cdm::kQuerySucceeded, cdm::kLinkTypeInternal, cdm::kProtectionHDCP);
}

void CdmAdapter::OnDeferredInitializationDone(cdm::StreamType stream_type,
                                              cdm::Status decoder_status)
{
}

// The CDM owns the returned object and must call FileIO::Close() to release it.
cdm::FileIO* CdmAdapter::CreateFileIO(cdm::FileIOClient* client)
{
  return nullptr;
}

bool CdmAdapter::SessionValid()
{ 
	return !session_id_.empty() && !message_.empty();
}

}  // namespace media
