/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/* This file contains the implementation of the OTARequestor class. All the core
 * OTA Requestor logic is contained in this class.
 */
#include "OTARequestor.h"

#include <app/server/Server.h>
#include <app/util/util.h>
#include <lib/core/CHIPEncoding.h>
#include <lib/support/CodeUtils.h>
#include <platform/CHIPDeviceLayer.h>

#include "BDXDownloader.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/CommissioneeDeviceProxy.h>
#include <controller/ExampleOperationalCredentialsIssuer.h>

#include <zap-generated/CHIPClientCallbacks.h>
#include <zap-generated/CHIPClusters.h>

using namespace chip;
using namespace chip::app::Clusters;
using namespace chip::bdx;
using namespace chip::Messaging;
using namespace chip::app::Clusters::OtaSoftwareUpdateProvider::Commands;

namespace {
// Global instance of the OTARequestorInterface.
OTARequestorInterface * globalOTARequestorInstance = nullptr;

constexpr uint32_t kImmediateStartDelayMs = 1; // Start the timer with this value when starting OTA "immediately"

// Callbacks for connection management
void OnConnected(void * context, chip::OperationalDeviceProxy * deviceProxy);
Callback::Callback<OnDeviceConnected> mOnConnectedCallback(OnConnected, nullptr);

void OnConnectionFailure(void * context, NodeId deviceId, CHIP_ERROR error);
Callback::Callback<OnDeviceConnectionFailure> mOnConnectionFailureCallback(OnConnectionFailure, nullptr);

void OnQueryImageResponse(void * context, const QueryImageResponse::DecodableType & response);
void OnQueryImageFailure(void * context, EmberAfStatus status);

void StartDelayTimerHandler(chip::System::Layer * systemLayer, void * appState)
{
    VerifyOrReturn(appState != nullptr);
    static_cast<OTARequestor *>(appState)->ConnectToProvider();
}

void OnQueryImageFailure(void * context, EmberAfStatus status)
{
    ChipLogDetail(SoftwareUpdate, "QueryImage failure response %" PRIu8, status);
}

// Called whenever FindOrEstablishSession is successful. Finds the Requestor instance
// and calls the corresponding OTARequestor member function
void OnConnected(void * context, chip::OperationalDeviceProxy * deviceProxy)
{
    OTARequestor * requestorCore = static_cast<OTARequestor *>(GetRequestorInstance());

    VerifyOrDie(requestorCore != nullptr);

    requestorCore->mOnConnected(context, deviceProxy);
}

void OnConnectionFailure(void * context, NodeId deviceId, CHIP_ERROR error)
{
    ChipLogError(SoftwareUpdate, "failed to connect to 0x%" PRIX64 ": %" CHIP_ERROR_FORMAT, deviceId, error.Format());
}
// Finds the Requestor instance and calls the corresponding OTARequestor member function
void OnQueryImageResponse(void * context, const QueryImageResponse::DecodableType & response)
{
    OTARequestor * requestorCore = static_cast<OTARequestor *>(GetRequestorInstance());

    VerifyOrDie(requestorCore != nullptr);

    requestorCore->mOnQueryImageResponse(context, response);
}
} // namespace

void SetRequestorInstance(OTARequestorInterface * instance)
{
    globalOTARequestorInstance = instance;
}

OTARequestorInterface * GetRequestorInstance()
{
    return globalOTARequestorInstance;
}

struct OTARequestor::QueryImageRequest
{
    char location[2];
    QueryImage::Type args;
};

void OTARequestor::mOnQueryImageResponse(void * context, const QueryImageResponse::DecodableType & response)
{
    ChipLogDetail(SoftwareUpdate, "QueryImageResponse responded with action %" PRIu8, response.status);

    CHIP_ERROR err = CHIP_NO_ERROR;

    // TODO: handle QueryImageResponse status types
    if (response.status != EMBER_ZCL_OTA_QUERY_STATUS_UPDATE_AVAILABLE)
    {
        return;
    }

    VerifyOrReturn(mBdxDownloader != nullptr, ChipLogError(SoftwareUpdate, "downloader is null"));

    // TODO: allow caller to provide their own OTADownloader instance and set BDX parameters

    TransferSession::TransferInitData initOptions;
    initOptions.TransferCtlFlags = chip::bdx::TransferControlFlags::kReceiverDrive;
    initOptions.MaxBlockSize     = 1024;
    char testFileDes[9]          = { "test.txt" };
    initOptions.FileDesLength    = static_cast<uint16_t>(strlen(testFileDes));
    initOptions.FileDesignator   = reinterpret_cast<uint8_t *>(testFileDes);

    chip::OperationalDeviceProxy * operationalDeviceProxy = Server::GetInstance().GetOperationalDeviceProxy();
    if (operationalDeviceProxy != nullptr)
    {
        chip::Messaging::ExchangeManager * exchangeMgr = operationalDeviceProxy->GetExchangeManager();
        chip::Optional<chip::SessionHandle> session    = operationalDeviceProxy->GetSecureSession();
        if (exchangeMgr != nullptr && session.HasValue())
        {
            exchangeCtx = exchangeMgr->NewContext(session.Value(), &mBdxMessenger);
        }

        if (exchangeCtx == nullptr)
        {
            ChipLogError(BDX, "unable to allocate ec: exchangeMgr=%p sessionExists? %u", exchangeMgr, session.HasValue());
            return;
        }
    }

    mBdxMessenger.Init(mBdxDownloader, exchangeCtx);
    mBdxDownloader->SetMessageDelegate(&mBdxMessenger);
    err = mBdxDownloader->SetBDXParams(initOptions);
    VerifyOrReturn(err == CHIP_NO_ERROR,
                   ChipLogError(SoftwareUpdate, "Error init BDXDownloader: %" CHIP_ERROR_FORMAT, err.Format()));
    err = mBdxDownloader->BeginPrepareDownload();
    VerifyOrReturn(err == CHIP_NO_ERROR,
                   ChipLogError(SoftwareUpdate, "Error init BDXDownloader: %" CHIP_ERROR_FORMAT, err.Format()));
}

EmberAfStatus OTARequestor::HandleAnnounceOTAProvider(
    chip::app::CommandHandler * commandObj, const chip::app::ConcreteCommandPath & commandPath,
    const chip::app::Clusters::OtaSoftwareUpdateRequestor::Commands::AnnounceOtaProvider::DecodableType & commandData)
{
    auto & providerLocation   = commandData.providerLocation;
    auto & announcementReason = commandData.announcementReason;

    if (commandObj == nullptr || commandObj->GetExchangeContext() == nullptr)
    {
        ChipLogError(SoftwareUpdate, "Cannot access ExchangeContext for FabricIndex");
        return EMBER_ZCL_STATUS_FAILURE;
    }

    mProviderNodeId      = providerLocation;
    mProviderFabricIndex = commandObj->GetExchangeContext()->GetSessionHandle().GetFabricIndex();

    ChipLogProgress(SoftwareUpdate, "OTA Requestor received AnnounceOTAProvider");
    ChipLogDetail(SoftwareUpdate, "  FabricIndex: %" PRIu8, mProviderFabricIndex);
    ChipLogDetail(SoftwareUpdate, "  ProviderNodeID: 0x" ChipLogFormatX64, ChipLogValueX64(mProviderNodeId));
    ChipLogDetail(SoftwareUpdate, "  VendorID: 0x%" PRIx16, commandData.vendorId);
    ChipLogDetail(SoftwareUpdate, "  AnnouncementReason: %" PRIu8, announcementReason);
    if (commandData.metadataForNode.HasValue())
    {
        ChipLogDetail(SoftwareUpdate, "  MetadataForNode: %zu", commandData.metadataForNode.Value().size());
    }

    // If reason is URGENT_UPDATE_AVAILABLE, we start OTA immediately. Otherwise, respect the timer value set in mOtaStartDelayMs.
    // This is done to exemplify what a real-world OTA Requestor might do while also being configurable enough to use as a test app.
    uint32_t msToStart = 0;
    switch (announcementReason)
    {
    case static_cast<uint8_t>(EMBER_ZCL_OTA_ANNOUNCEMENT_REASON_SIMPLE_ANNOUNCEMENT):
    case static_cast<uint8_t>(EMBER_ZCL_OTA_ANNOUNCEMENT_REASON_UPDATE_AVAILABLE):
        msToStart = mOtaStartDelayMs;
        break;
    case static_cast<uint8_t>(EMBER_ZCL_OTA_ANNOUNCEMENT_REASON_URGENT_UPDATE_AVAILABLE):
        msToStart = kImmediateStartDelayMs;
        break;
    default:
        ChipLogError(SoftwareUpdate, "Unexpected announcementReason: %" PRIu8, static_cast<uint8_t>(announcementReason));
        return EMBER_ZCL_STATUS_FAILURE;
    }

    chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(msToStart), StartDelayTimerHandler, this);

    return EMBER_ZCL_STATUS_SUCCESS;
}

CHIP_ERROR OTARequestor::SetupCASESessionManager(chip::FabricIndex fabricIndex)
{
    // A previous CASE session had been established
    if (mCASESessionManager != nullptr)
    {
        if (mCASESessionManager->GetFabricInfo()->GetFabricIndex() != fabricIndex)
        {
            // CSM is per fabric so if fabric index does not match the previous session, CSM needs to be set up again
            chip::Platform::Delete(mCASESessionManager);
            mCASESessionManager = nullptr;
        }
        else
        {
            // Fabric index matches so use previous instance
            return CHIP_NO_ERROR;
        }
    }

    // CSM has not been setup so create a new instance of it
    if (mCASESessionManager == nullptr)
    {
        chip::Server * server         = &(chip::Server::GetInstance());
        chip::FabricInfo * fabricInfo = server->GetFabricTable().FindFabricWithIndex(fabricIndex);
        if (fabricInfo == nullptr)
        {
            ChipLogError(SoftwareUpdate, "Did not find fabric for index %d", fabricIndex);
            return CHIP_ERROR_INVALID_ARGUMENT;
        }

        chip::DeviceProxyInitParams initParams = {
            .sessionManager = &(server->GetSecureSessionManager()),
            .exchangeMgr    = &(server->GetExchangeManager()),
            .idAllocator    = &(server->GetSessionIDAllocator()),
            .fabricInfo     = fabricInfo,
            // TODO: Determine where this should be instantiated
            .imDelegate = chip::Platform::New<chip::Controller::DeviceControllerInteractionModelDelegate>(),
        };

        chip::CASESessionManagerConfig sessionManagerConfig = {
            .sessionInitParams = initParams,
            .dnsCache          = nullptr,
        };

        mCASESessionManager = chip::Platform::New<chip::CASESessionManager>(sessionManagerConfig);
    }

    if (mCASESessionManager == nullptr)
    {
        ChipLogError(SoftwareUpdate, "Failed in creating an instance of CASESessionManager");
        return CHIP_ERROR_NO_MEMORY;
    }

    return CHIP_NO_ERROR;
}

// Converted from SendQueryImageCommand()
void OTARequestor::ConnectToProvider()
{
    chip::NodeId peerNodeId           = mProviderNodeId;
    chip::FabricIndex peerFabricIndex = mProviderFabricIndex;

    Server * server           = &(Server::GetInstance());
    chip::FabricInfo * fabric = server->GetFabricTable().FindFabricWithIndex(peerFabricIndex);
    if (fabric == nullptr)
    {
        ChipLogError(SoftwareUpdate, "Did not find fabric for index %d", peerFabricIndex);
        return;
    }

    chip::DeviceProxyInitParams initParams = {
        .sessionManager = &(server->GetSecureSessionManager()),
        .exchangeMgr    = &(server->GetExchangeManager()),
        .idAllocator    = &(server->GetSessionIDAllocator()),
        .fabricInfo     = fabric,
        // TODO: Determine where this should be instantiated
        .imDelegate = chip::Platform::New<chip::Controller::DeviceControllerInteractionModelDelegate>(),
    };

    chip::OperationalDeviceProxy * operationalDeviceProxy =
        chip::Platform::New<chip::OperationalDeviceProxy>(initParams, fabric->GetPeerIdForNode(peerNodeId));
    if (operationalDeviceProxy == nullptr)
    {
        ChipLogError(SoftwareUpdate, "Failed in creating an instance of OperationalDeviceProxy");
        return;
    }

    server->SetOperationalDeviceProxy(operationalDeviceProxy);

    // Explicitly calling UpdateDeviceData() should not be needed once OperationalDeviceProxy can resolve IP address from node ID
    // and fabric index
    Transport::PeerAddress addr = Transport::PeerAddress::UDP(mIpAddress, CHIP_PORT);
    operationalDeviceProxy->UpdateDeviceData(addr, operationalDeviceProxy->GetMRPConfig());

    CHIP_ERROR err = operationalDeviceProxy->Connect(&mOnConnectedCallback, &mOnConnectionFailureCallback);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(SoftwareUpdate, "Cannot establish connection to peer device: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

// Member function called whenever FindOrEstablishSession is successful
void OTARequestor::mOnConnected(void * context, chip::DeviceProxy * deviceProxy)
{
    switch (onConnectedState)
    {
    case kQueryImage: {
        constexpr EndpointId kOtaProviderEndpoint = 0;

        QueryImageRequest request;
        CHIP_ERROR err = BuildQueryImageRequest(request);
        VerifyOrReturn(err == CHIP_NO_ERROR,
                       ChipLogError(SoftwareUpdate, "Failed to build QueryImage command: %" CHIP_ERROR_FORMAT, err.Format()));

        Controller::OtaSoftwareUpdateProviderCluster cluster;
        cluster.Associate(deviceProxy, kOtaProviderEndpoint);

        err = cluster.InvokeCommand(request.args, /* context = */ nullptr, OnQueryImageResponse, OnQueryImageFailure);
        VerifyOrReturn(err == CHIP_NO_ERROR,
                       ChipLogError(SoftwareUpdate, "Failed to send QueryImage command: %" CHIP_ERROR_FORMAT, err.Format()));

        break;
    }
    case kStartBDX: {
        VerifyOrReturn(mBdxDownloader != nullptr, ChipLogError(SoftwareUpdate, "downloader is null"));

        // TODO: allow caller to provide their own OTADownloader instance and set BDX parameters

        TransferSession::TransferInitData initOptions;
        initOptions.TransferCtlFlags = chip::bdx::TransferControlFlags::kReceiverDrive;
        initOptions.MaxBlockSize     = 1024;
        char testFileDes[9]          = { "test.txt" };
        initOptions.FileDesLength    = static_cast<uint16_t>(strlen(testFileDes));
        initOptions.FileDesignator   = reinterpret_cast<uint8_t *>(testFileDes);

        if (deviceProxy != nullptr)
        {
            chip::Messaging::ExchangeManager * exchangeMgr = deviceProxy->GetExchangeManager();
            chip::Optional<chip::SessionHandle> session    = deviceProxy->GetSecureSession();
            if (exchangeMgr != nullptr && session.HasValue())
            {
                exchangeCtx = exchangeMgr->NewContext(session.Value(), &mBdxMessenger);
            }

            if (exchangeCtx == nullptr)
            {
                ChipLogError(BDX, "unable to allocate ec: exchangeMgr=%p sessionExists? %u", exchangeMgr, session.HasValue());
                return;
            }
        }

        mBdxMessenger.Init(mBdxDownloader, exchangeCtx);
        mBdxDownloader->SetMessageDelegate(&mBdxMessenger);
        CHIP_ERROR err = mBdxDownloader->SetBDXParams(initOptions);
        VerifyOrReturn(err == CHIP_NO_ERROR,
                       ChipLogError(SoftwareUpdate, "Error init BDXDownloader: %" CHIP_ERROR_FORMAT, err.Format()));
        err = mBdxDownloader->BeginPrepareDownload();
        VerifyOrReturn(err == CHIP_NO_ERROR,
                       ChipLogError(SoftwareUpdate, "Error init BDXDownloader: %" CHIP_ERROR_FORMAT, err.Format()));
        break;
    }
    default:
        break;
    }
}

void OTARequestor::TriggerImmediateQuery()
{
    // Perhaps we don't need a separate function ConnectToProvider, revisit this
    ConnectToProvider();
}

CHIP_ERROR OTARequestor::BuildQueryImageRequest(QueryImageRequest & request)
{
    constexpr EmberAfOTADownloadProtocol kProtocolsSupported[] = { EMBER_ZCL_OTA_DOWNLOAD_PROTOCOL_BDX_SYNCHRONOUS };
    constexpr bool kRequestorCanConsent                        = false;
    QueryImage::Type & args                                    = request.args;

    uint16_t vendorId;
    VerifyOrReturnError(Basic::Attributes::VendorID::Get(kRootEndpointId, &vendorId) == EMBER_ZCL_STATUS_SUCCESS,
                        CHIP_ERROR_READ_FAILED);
    args.vendorId = static_cast<VendorId>(vendorId);

    VerifyOrReturnError(Basic::Attributes::ProductID::Get(kRootEndpointId, &args.productId) == EMBER_ZCL_STATUS_SUCCESS,
                        CHIP_ERROR_READ_FAILED);

    VerifyOrReturnError(Basic::Attributes::SoftwareVersion::Get(kRootEndpointId, &args.softwareVersion) == EMBER_ZCL_STATUS_SUCCESS,
                        CHIP_ERROR_READ_FAILED);

    args.protocolsSupported = kProtocolsSupported;
    args.requestorCanConsent.SetValue(kRequestorCanConsent);

    uint16_t hardwareVersion;
    if (Basic::Attributes::HardwareVersion::Get(kRootEndpointId, &hardwareVersion) == EMBER_ZCL_STATUS_SUCCESS)
    {
        args.hardwareVersion.SetValue(hardwareVersion);
    }

    if (Basic::Attributes::Location::Get(kRootEndpointId, MutableCharSpan(request.location)) == EMBER_ZCL_STATUS_SUCCESS)
    {
        args.location.SetValue(CharSpan(request.location));
    }

    return CHIP_NO_ERROR;
}
