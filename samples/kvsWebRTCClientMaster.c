#include "Samples.h"

extern PSampleConfiguration gSampleConfiguration;

INT32 main(INT32 argc, CHAR *argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    SignalingClientCallbacks signalingClientCallbacks;
    SignalingClientInfo clientInfo;
    UINT32 frameSize;
    PSampleConfiguration pSampleConfiguration = NULL;
    CLIENT_HANDLE clientHandle = INVALID_CLIENT_HANDLE_VALUE;
    STREAM_HANDLE streamHandle = INVALID_STREAM_HANDLE_VALUE;
    PDeviceInfo pDeviceInfo = NULL;
    PStreamInfo pStreamInfo = NULL;
    PClientCallbacks pClientCallbacks = NULL;
    PAwsCredentials pAwsCredentials;
    BOOL persistence = FALSE;

    signal(SIGINT, sigintHandler);

    // do trickle ICE by default
    printf("[KVS Master] Using trickleICE by default\n");

    CHK_STATUS(createSampleConfiguration(argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME,
            SIGNALING_CHANNEL_ROLE_TYPE_MASTER,
            TRUE,
            TRUE,
            &pSampleConfiguration));
    printf("[KVS Master] Created signaling channel %s\n", (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    // Set the audio and video handlers
    pSampleConfiguration->audioSource = sendAudioPackets;
    pSampleConfiguration->videoSource = sendVideoPackets;
    pSampleConfiguration->receiveAudioVideoSource = sampleReceiveAudioFrame;
    pSampleConfiguration->onDataChannel = onDataChannel;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
    printf("[KVS Master] Finished setting audio and video handlers\n");

    // Check if the samples are present

    CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./h264SampleFrames/frame-001.h264"));
    printf("[KVS Master] Checked sample video frame availability....available\n");

    CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./opusSampleFrames/sample-001.opus"));
    printf("[KVS Master] Checked sample audio frame availability....available\n");

    // Initialize KVS WebRTC. This must be done before anything else, and must only be done once.
    CHK_STATUS(initKvsWebRtc());
    printf("[KVS Master] KVS WebRTC initialization completed successfully\n");

    signalingClientCallbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
    signalingClientCallbacks.messageReceivedFn = masterMessageReceived;
    signalingClientCallbacks.errorReportFn = NULL;
    signalingClientCallbacks.stateChangeFn = signalingClientStateChanged;
    signalingClientCallbacks.customData = (UINT64) pSampleConfiguration;

    clientInfo.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
    STRCPY(clientInfo.clientId, SAMPLE_MASTER_CLIENT_ID);

    CHK_STATUS(createSignalingClientSync(&clientInfo,
            &pSampleConfiguration->channelInfo,
            &signalingClientCallbacks,
            pSampleConfiguration->pCredentialProvider,
            &pSampleConfiguration->signalingClientHandle));
    printf("[KVS Master] Signaling client created successfully\n");

    // Enable the processing of the messages
    CHK_STATUS(signalingClientConnectSync(pSampleConfiguration->signalingClientHandle));
    printf("[KVS Master] Signaling client connection to socket established\n");

    if (argc > 2) {
        // Enabling persistence if the second argument is supplied with the stream name
        printf("[KVS Master] Streaming to KVS stream %s\n", argv[2]);

        // Create KVS stream object graph
        CHK_STATUS(createDefaultDeviceInfo(&pDeviceInfo));
        CHK_STATUS(createRealtimeVideoStreamInfoProvider(argv[2], DEFAULT_RETENTION_PERIOD,
                DEFAULT_BUFFER_DURATION, &pStreamInfo));
        CHK_STATUS(pSampleConfiguration->pCredentialProvider->getCredentialsFn(
                pSampleConfiguration->pCredentialProvider, &pAwsCredentials));
        CHK_STATUS(createDefaultCallbacksProviderWithAwsCredentials(pAwsCredentials->accessKeyId,
                                                                    pAwsCredentials->secretKey,
                                                                    pAwsCredentials->sessionToken,
                                                                    MAX_UINT64,
                                                                    pSampleConfiguration->channelInfo.pRegion,
                                                                    pSampleConfiguration->pCaCertPath,
                                                                    NULL,
                                                                    NULL,
         //                                                           FALSE,
                                                                    &pClientCallbacks));

        CHK_STATUS(createKinesisVideoClient(pDeviceInfo, pClientCallbacks, &clientHandle));
        CHK_STATUS(createKinesisVideoStreamSync(clientHandle, pStreamInfo, &streamHandle));
        persistence = TRUE;
    }

    // Store the client and the stream handles in the config
    pSampleConfiguration->clientHandle = clientHandle;
    pSampleConfiguration->streamHandle = streamHandle;

    gSampleConfiguration = pSampleConfiguration;

    printf("[KVS Master] Beginning audio-video streaming...check the stream over channel %s\n",
            (argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME));

    if (persistence) {
        CHK_STATUS(startSenderMediaThreads(pSampleConfiguration));
    }

    // Checking for termination
    CHK_STATUS(sessionCleanupWait(pSampleConfiguration));

    printf("[KVS Master] Streaming session terminated. Awaiting for the stream termination.\n");

    CHK_STATUS(stopKinesisVideoStreamSync(streamHandle));
    CHK_STATUS(freeKinesisVideoStream(&streamHandle));
    CHK_STATUS(freeKinesisVideoClient(&clientHandle));

CleanUp:
    printf("[KVS Master] Cleaning up....\n");
    CHK_LOG_ERR_NV(retStatus);

    if (pSampleConfiguration != NULL) {
        // Kick of the termination sequence
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        // Join the threads
        if (IS_VALID_TID_VALUE(pSampleConfiguration->videoSenderTid)) {
            THREAD_JOIN(pSampleConfiguration->videoSenderTid, NULL);
        }

        if (IS_VALID_TID_VALUE(pSampleConfiguration->audioSenderTid)) {
            THREAD_JOIN(pSampleConfiguration->audioSenderTid, NULL);
        }

        CHK_LOG_ERR_NV(freeSignalingClient(&pSampleConfiguration->signalingClientHandle));
        CHK_LOG_ERR_NV(freeSampleConfiguration(&pSampleConfiguration));
    }

    CHK_LOG_ERR_NV(freeDeviceInfo(&pDeviceInfo));
    CHK_LOG_ERR_NV(freeStreamInfoProvider(&pStreamInfo));
    CHK_LOG_ERR_NV(freeKinesisVideoStream(&streamHandle));
    CHK_LOG_ERR_NV(freeKinesisVideoClient(&clientHandle));
    CHK_LOG_ERR_NV(freeCallbacksProvider(&pClientCallbacks));

    printf("[KVS Master] Cleanup done\n");
    return (INT32) retStatus;
}

STATUS readFrameFromDisk(PBYTE pFrame, PUINT32 pSize, PCHAR frameFilePath)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 size = 0;

    CHK(pSize != NULL, STATUS_NULL_ARG);
    size = *pSize;

    // Get the size and read into frame
    CHK_STATUS(readFile(frameFilePath, TRUE, pFrame, &size));

CleanUp:

    if (pSize != NULL) {
        *pSize = (UINT32) size;
    }

    return retStatus;
}

PVOID sendVideoPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    UINT32 fileIndex = 0, frameSize, i;
    CHAR filePath[MAX_PATH_LEN + 1];
    STATUS status;

    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);

    frame.presentationTs = 0;
    frame.decodingTs = 0;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        SNPRINTF(filePath, MAX_PATH_LEN, "./h264SampleFrames/frame-%03d.h264", fileIndex % NUMBER_OF_H264_FRAME_FILES + 1);
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, filePath));

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->videoBufferSize) {
            CHK(NULL != (pSampleConfiguration->pVideoFrameBuffer = (PBYTE) MEMREALLOC(pSampleConfiguration->pVideoFrameBuffer, frameSize)), STATUS_NOT_ENOUGH_MEMORY);
            pSampleConfiguration->videoBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pVideoFrameBuffer;
        frame.size = frameSize;
        frame.trackId = DEFAULT_VIDEO_TRACK_ID;

        frame.flags = fileIndex % DEFAULT_KEY_FRAME_INTERVAL == 0 ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;
        frame.index = fileIndex++;

        CHK_STATUS(readFrameFromDisk(frame.frameData, &frameSize, filePath));

        frame.presentationTs += SAMPLE_VIDEO_FRAME_DURATION;
        frame.decodingTs = frame.presentationTs;

        if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->updatingSampleStreamingSessionList)) {
            ATOMIC_INCREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
            for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
                status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &frame);
                if (STATUS_FAILED(status)) {
                    DLOGD("writeFrame failed with 0x%08x", status);
                }
            }

            // Output the frame to KVS stream as well
            if (IS_VALID_STREAM_HANDLE(pSampleConfiguration->streamHandle)) {
                status = putKinesisVideoFrame(pSampleConfiguration->streamHandle, &frame);
                if (STATUS_FAILED(status)) {
                    DLOGD("putKinesisVideoFrame failed with 0x%08x", status);
                }
            }

            ATOMIC_DECREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
        }

        THREAD_SLEEP(SAMPLE_VIDEO_FRAME_DURATION);
    }

CleanUp:

    CHK_LOG_ERR_NV(retStatus);
    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID sendAudioPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    UINT32 i;
    STATUS status;

    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);

    frame.presentationTs = 0;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_OPUS_FRAME_FILES + 1;
        SNPRINTF(filePath, MAX_PATH_LEN, "./opusSampleFrames/sample-%03d.opus", fileIndex);
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, filePath));

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->audioBufferSize) {
            CHK(NULL != (pSampleConfiguration->pAudioFrameBuffer = (PBYTE) MEMREALLOC(pSampleConfiguration->pAudioFrameBuffer, frameSize)), STATUS_NOT_ENOUGH_MEMORY);
            pSampleConfiguration->audioBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pAudioFrameBuffer;
        frame.size = frameSize;

        CHK_STATUS(readFrameFromDisk(frame.frameData, &frameSize, filePath));

        frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;

        if (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->updatingSampleStreamingSessionList)) {
            ATOMIC_INCREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
            for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
                status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pAudioRtcRtpTransceiver, &frame);
                if (STATUS_FAILED(status)) {
                    DLOGD("writeFrame failed with 0x%08x", status);
                }
            }
            ATOMIC_DECREMENT(&pSampleConfiguration->streamingSessionListReadingThreadCount);
        }

        THREAD_SLEEP(SAMPLE_AUDIO_FRAME_DURATION);
    }

CleanUp:

    CHK_LOG_ERR_NV(retStatus);
    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID sampleReceiveAudioFrame(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    CHK(pSampleStreamingSession != NULL, STATUS_NULL_ARG);

    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver,
                       (UINT64) pSampleStreamingSession,
                       sampleFrameHandler));
CleanUp:

    CHK_LOG_ERR_NV(retStatus);
    return (PVOID) (ULONG_PTR) retStatus;
}
